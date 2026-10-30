#!/usr/bin/env python3
"""Own selected session names on the private bus and forward them.

Calls and signals go through the xdg-dbus-proxy socket, never directly to the
user's session bus and never for org.freedesktop.portal.Desktop.
"""

from __future__ import annotations

import os
import signal
import sys
import traceback

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
gi.require_version("GLibUnix", "2.0")
from gi.repository import Gio, GLib, GLibUnix

FORBIDDEN = frozenset(
    {
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.login1",
        "org.gnome.DisplayManager",
        "org.freedesktop.DBus",
    }
)

# DO_NOT_QUEUE: fail instead of sitting behind another owner.
NAME_FLAG_DO_NOT_QUEUE = 4
PRIMARY_OWNER = 1
CALL_TIMEOUT_MS = 20000


def log(text: str) -> None:
    print(text, file=sys.stderr, flush=True)


def copy_fd_list(src: Gio.UnixFDList | None) -> Gio.UnixFDList | None:
    if src is None or src.get_length() == 0:
        return None
    dst = Gio.UnixFDList.new()
    for index in range(src.get_length()):
        fd = src.get(index)
        if isinstance(fd, tuple):
            fd = fd[0]
        dst.append(fd)
        os.close(fd)
    return dst


def connect(address: str) -> Gio.DBusConnection:
    return Gio.DBusConnection.new_for_address_sync(
        address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None,
        None,
    )


class NameBridge:
    def __init__(self, private: Gio.DBusConnection, forward: Gio.DBusConnection, name: str) -> None:
        self.private = private
        self.forward = forward
        self.name = name
        self.private.add_filter(self.filter_cb)
        result = self.private.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "RequestName",
            GLib.Variant("(su)", (name, NAME_FLAG_DO_NOT_QUEUE)),
            GLib.VariantType.new("(u)"),
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )
        code = int(result.unpack()[0])
        if code != PRIMARY_OWNER:
            raise RuntimeError(f"{name} RequestName sonucu {code}, birincil sahip olunamadı")
        self.forward.signal_subscribe(
            name,
            None,
            None,
            None,
            None,
            Gio.DBusSignalFlags.NONE,
            self.on_signal,
            None,
        )
        log(f"ad alındı: {name} sahip={self.private.get_unique_name()}")

    def filter_cb(self, connection, message, incoming, user_data=None):
        try:
            if not incoming:
                return message
            if message.get_message_type() != Gio.DBusMessageType.METHOD_CALL:
                return message
            dest = message.get_destination()
            own = connection.get_unique_name()
            if dest != self.name and dest != own:
                return message
            iface = message.get_interface()
            if iface == "org.freedesktop.DBus.Peer":
                return message
            member = message.get_member()
            path = message.get_path()
            if not member or not path:
                return message
            flags = int(message.get_flags())
            info = {
                "path": path,
                "iface": iface,
                "member": member,
                "body": message.get_body(),
                "fds": copy_fd_list(message.get_unix_fd_list()),
                "sender": message.get_sender(),
                "serial": message.get_serial(),
                "no_reply": bool(flags & int(Gio.DBusMessageFlags.NO_REPLY_EXPECTED)),
                "original": message,
            }
            GLib.idle_add(self.forward_call, info)
            return None
        except Exception:
            traceback.print_exc()
            return message

    def forward_call(self, info: dict) -> bool:
        try:
            iface = info["iface"] if info["iface"] else None
            call = Gio.DBusMessage.new_method_call(
                self.name,
                info["path"],
                iface,
                info["member"],
            )
            if info["body"] is not None:
                call.set_body(info["body"])
            if info["fds"] is not None:
                call.set_unix_fd_list(info["fds"])
            log(f"ileti {self.name} {info['path']} {iface or '-'}.{info['member']}")
            if info["no_reply"]:
                self.forward.send_message(call, Gio.DBusSendMessageFlags.NONE)
                return False
            self.forward.send_message_with_reply(
                call,
                Gio.DBusSendMessageFlags.NONE,
                CALL_TIMEOUT_MS,
                None,
                self.on_reply,
                info,
            )
        except Exception as exc:
            traceback.print_exc()
            self.send_error(info, str(exc))
        return False

    def on_reply(self, connection, result, info=None) -> None:
        try:
            reply = connection.send_message_with_reply_finish(result)
        except GLib.Error as exc:
            self.send_error(info, exc.message)
            return
        if reply is None:
            self.send_error(info, "vekil yanıt vermedi")
            return
        try:
            if reply.get_message_type() == Gio.DBusMessageType.ERROR:
                body = reply.get_body()
                text = ""
                if body is not None:
                    unpacked = body.unpack()
                    if unpacked:
                        text = str(unpacked[0])
                out = Gio.DBusMessage.new_method_error_literal(
                    info["original"],
                    reply.get_error_name() or "org.freedesktop.DBus.Error.Failed",
                    text,
                )
            else:
                out = Gio.DBusMessage.new_method_reply(info["original"])
                body = reply.get_body()
                if body is not None:
                    out.set_body(body)
                fds = copy_fd_list(reply.get_unix_fd_list())
                if fds is not None:
                    out.set_unix_fd_list(fds)
            ok, _serial = self.private.send_message(out, Gio.DBusSendMessageFlags.NONE)
            if not ok:
                log(f"yanıt gönderilemedi: {self.name} {info['member']}")
            else:
                log(f"yanıt {self.name} {info['member']}")
        except Exception as exc:
            traceback.print_exc()
            self.send_error(info, str(exc))

    def send_error(self, info: dict, text: str) -> None:
        if info.get("no_reply"):
            return
        try:
            err = Gio.DBusMessage.new_method_error_literal(
                info["original"],
                "org.freedesktop.DBus.Error.Failed",
                text,
            )
            self.private.send_message(err, Gio.DBusSendMessageFlags.NONE)
            log(f"hata {self.name} {info.get('member')}: {text}")
        except Exception:
            traceback.print_exc()

    def on_signal(self, connection, sender, path, iface, member, params, user_data=None) -> None:
        try:
            msg = Gio.DBusMessage.new_signal(path, iface, member)
            if params is not None:
                msg.set_body(params)
            self.private.send_message(msg, Gio.DBusSendMessageFlags.NONE)
        except Exception:
            traceback.print_exc()


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "kullanım: crd-proxy-relay.py <özel-adres> <vekil-adres> <ad> [ad...]",
            file=sys.stderr,
        )
        return 2
    private_addr = sys.argv[1]
    forward_addr = sys.argv[2]
    names = sys.argv[3:]
    for name in names:
        if name in FORBIDDEN or not name or " " in name or "." not in name:
            log(f"reddedilen ad: {name}")
            return 2
    try:
        forward = connect(forward_addr)
    except GLib.Error as exc:
        log(f"vekil veriyoluna bağlanılamadı: {exc.message}")
        return 1
    bridges: list[NameBridge] = []
    for name in names:
        try:
            private = connect(private_addr)
            bridges.append(NameBridge(private, forward, name))
        except (GLib.Error, RuntimeError) as exc:
            message = exc.message if isinstance(exc, GLib.Error) else str(exc)
            log(f"{name} kurulamadı: {message}")
            return 1
    if not bridges:
        log("iletilecek ad yok")
        return 1
    loop = GLib.MainLoop()

    def stop() -> bool:
        loop.quit()
        return False

    GLibUnix.signal_add(GLib.PRIORITY_HIGH, signal.SIGTERM, stop)
    GLibUnix.signal_add(GLib.PRIORITY_HIGH, signal.SIGINT, stop)

    def announce() -> bool:
        print("ready", flush=True)
        return False

    GLib.idle_add(announce)
    loop.run()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        sys.exit(1)
