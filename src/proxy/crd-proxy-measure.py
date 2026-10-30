#!/usr/bin/env python3
"""Host yüzündeki vekil soketinden portal zincirini ölçer.

CreateSession, SelectDevices, SelectSources, Start, OpenPipeWireRemote.
OpenPipeWireRemote ucunun baytları okunmaz; yalnız türü ve karşı uç yolu yazılır.
Gerçek oturum veriyoluna bağlanılmaz. ConnectToEIS çağrılmaz.
"""

from __future__ import annotations

import os
import socket
import stat
import struct
import sys

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib

SO_DOMAIN = getattr(socket, "SO_DOMAIN", 39)
BUS_NAME = "org.freedesktop.portal.Desktop"
OBJECT_PATH = "/org/freedesktop/portal/desktop"
REMOTE = "org.freedesktop.portal.RemoteDesktop"
SCREEN = "org.freedesktop.portal.ScreenCast"
REQUEST = "org.freedesktop.portal.Request"
SESSION = "org.freedesktop.portal.Session"
FACE_SUFFIX = "/crd-for-linux-proxy/bus"

KEYS = (
    "unique_name",
    "create_response",
    "create_request",
    "session_handle",
    "select_devices_response",
    "select_sources_response",
    "start_response",
    "stream_count",
    "stream_node",
    "mapping_id",
    "position",
    "size",
    "pipewire_fd_valid",
    "pipewire_fd_kind",
    "pipewire_fd_domain",
    "pipewire_fd_type",
    "pipewire_peer_path",
    "pipewire_peer_pid",
    "expected_peer",
    "session_closed",
)


def unwrap(value):
    if isinstance(value, GLib.Variant):
        return unwrap(value.unpack())
    if isinstance(value, dict):
        return {key: unwrap(item) for key, item in value.items()}
    if isinstance(value, list):
        return [unwrap(item) for item in value]
    if isinstance(value, tuple):
        return tuple(unwrap(item) for item in value)
    return value


def address_path(address: str) -> str:
    raw = address.split(",", 1)[0]
    prefix = "unix:path="
    if raw.startswith(prefix):
        return raw[len(prefix) :]
    return ""


def one_line(text: str) -> str:
    return " ".join(text.replace("\n", " ").split())


def dict_variant(fields: dict) -> dict:
    return {key: GLib.Variant(kind, value) for key, (kind, value) in fields.items()}


def fd_is_socket(fd: int) -> tuple[bool, str]:
    try:
        mode = os.fstat(fd).st_mode
    except OSError as exc:
        return False, f"fstat:{exc}"
    if not stat.S_ISSOCK(mode):
        return False, f"mode:{mode}"
    return True, f"sock:{mode}"


def take_fd(fd_list, index: int) -> int:
    fd = fd_list.get(index)
    if isinstance(fd, tuple):
        fd = fd[0]
    return int(fd)


def classify_pipewire_fd(fd: int) -> dict:
    """Soket baytlarını okumaz. socketpair kendi pid'ine boş yolla gider."""
    held = socket.socket(fileno=fd)
    try:
        domain = held.getsockopt(socket.SOL_SOCKET, SO_DOMAIN)
        kind = held.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE)
        peer = held.getpeername()
        raw = held.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
    finally:
        held.detach()
    pid, _uid, _gid = struct.unpack("iII", raw)
    if isinstance(peer, bytes):
        path = peer.decode("utf-8", "surrogateescape")
    else:
        path = peer or ""
    path = path.rstrip("\x00")
    if path.startswith("\x00"):
        shown = "@" + path[1:]
    else:
        shown = path
    domain_name = "AF_UNIX" if domain == socket.AF_UNIX else str(domain)
    type_name = "SOCK_STREAM" if kind == socket.SOCK_STREAM else str(kind)
    base = os.path.basename(shown)
    if domain == socket.AF_UNIX and kind == socket.SOCK_STREAM and path == "" and pid == os.getpid():
        label = "socketpair"
    elif (
        domain == socket.AF_UNIX
        and kind == socket.SOCK_STREAM
        and base == "pipewire-0"
        and path == shown
        and pid != os.getpid()
        and pid > 0
    ):
        label = "pipewire"
    else:
        label = "other"
    return {
        "kind": label,
        "domain": domain_name,
        "type": type_name,
        "peer_path": shown,
        "peer_pid": str(pid),
    }


class Responses:
    def __init__(self, connection: Gio.DBusConnection) -> None:
        self.connection = connection
        self.pending: dict[str, tuple] = {}
        self.loop: GLib.MainLoop | None = None
        self.sub = connection.signal_subscribe(
            None,
            REQUEST,
            "Response",
            None,
            None,
            Gio.DBusSignalFlags.NONE,
            self.on_signal,
        )

    def on_signal(self, _conn, _sender, object_path, _iface, _signal_name, params, *_args):
        code, results = unwrap(params)
        self.pending[object_path] = (code, results)
        if self.loop is not None and self.loop.is_running():
            self.loop.quit()

    def close(self) -> None:
        if self.sub:
            self.connection.signal_unsubscribe(self.sub)
            self.sub = 0

    def wait(self, path: str):
        if path in self.pending:
            return self.pending[path]
        self.loop = GLib.MainLoop()
        GLib.timeout_add(8000, self._timeout)
        self.loop.run()
        self.loop = None
        return self.pending.get(path)

    def _timeout(self) -> bool:
        if self.loop is not None and self.loop.is_running():
            self.loop.quit()
        return False


def call_request(connection, responses: Responses, interface: str, method: str, parameters, label: str, failures: list[str]):
    try:
        reply = connection.call_sync(
            BUS_NAME,
            OBJECT_PATH,
            interface,
            method,
            parameters,
            GLib.VariantType.new("(o)"),
            Gio.DBusCallFlags.NONE,
            8000,
            None,
        )
    except GLib.Error as exc:
        failures.append(f"{label}: {one_line(exc.message)}")
        return None
    returned = unwrap(reply)[0]
    if not isinstance(returned, str) or not returned.startswith(f"{OBJECT_PATH}/request/"):
        failures.append(f"{label} dönüş yolu {returned!r}")
        return None
    got = responses.wait(returned)
    if got is None:
        failures.append(f"{label} Response gelmedi")
        return None
    code, results = got
    if code != 0:
        failures.append(f"{label} Response kodu {code}")
    try:
        connection.call_sync(
            BUS_NAME,
            returned,
            REQUEST,
            "Close",
            GLib.Variant.new_tuple(),
            GLib.VariantType.new("()"),
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        )
    except GLib.Error as exc:
        failures.append(f"{label} Request.Close: {one_line(exc.message)}")
    return returned, code, results


def call_fd(connection, method: str, session_path: str) -> int:
    outcome = connection.call_with_unix_fd_list_sync(
        BUS_NAME,
        OBJECT_PATH,
        SCREEN,
        method,
        GLib.Variant("(oa{sv})", (session_path, {})),
        GLib.VariantType.new("(h)"),
        Gio.DBusCallFlags.NONE,
        8000,
        None,
    )
    if not isinstance(outcome, tuple) or len(outcome) != 2:
        raise RuntimeError(f"{method} dönüşü fd listesi değil")
    variant, fd_list = outcome
    if fd_list is None:
        raise RuntimeError(f"{method} fd listesi boş")
    index = unwrap(variant)[0]
    return take_fd(fd_list, index)


def close_session(connection, session_path: str, measured: dict) -> None:
    closed = {}
    loop = GLib.MainLoop()

    def on_closed(_conn, _sender, object_path, _iface, _signal_name, _params, *_args):
        closed["path"] = object_path
        if loop.is_running():
            loop.quit()

    sub = connection.signal_subscribe(
        None,
        SESSION,
        "Closed",
        session_path,
        None,
        Gio.DBusSignalFlags.NONE,
        on_closed,
    )
    try:
        connection.call_sync(
            BUS_NAME,
            session_path,
            SESSION,
            "Close",
            GLib.Variant.new_tuple(),
            GLib.VariantType.new("()"),
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        )
        if "path" not in closed:
            GLib.timeout_add(3000, lambda: loop.quit() or False)
            loop.run()
    except GLib.Error as exc:
        measured["session_closed"] = "no: " + one_line(exc.message)
        return
    finally:
        connection.signal_unsubscribe(sub)
    if closed.get("path") == session_path:
        measured["session_closed"] = "yes"
    else:
        measured["session_closed"] = "no"


def write_facts(path: str, measured: dict, failures: list[str]) -> None:
    lines = []
    for key in KEYS:
        if key in measured:
            lines.append(f"{key}={measured[key]}")
    if failures:
        lines.append("failures=" + " | ".join(one_line(item) for item in failures))
    else:
        lines.append("failures=none")
    text = "\n".join(lines) + "\n"
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    sys.stdout.write(text)


def refuse_address(address: str, failures: list[str]) -> bool:
    face = address_path(address)
    real = address_path(os.environ.get("DBUS_SESSION_BUS_ADDRESS", ""))
    runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    runtime_bus = os.path.join(runtime, "bus") if runtime else ""
    if not face.endswith(FACE_SUFFIX):
        failures.append(f"adres host soketi değil: {face or address}")
        return False
    if real and face == real:
        failures.append("gerçek oturum veriyolu reddedildi")
        return False
    if runtime_bus and face == runtime_bus:
        failures.append("gerçek oturum veriyolu reddedildi")
        return False
    return True


def positive_pair(value) -> tuple[int, int] | None:
    if not isinstance(value, tuple) or len(value) != 2:
        return None
    width, height = value
    if isinstance(width, bool) or isinstance(height, bool):
        return None
    if not isinstance(width, int) or not isinstance(height, int):
        return None
    if width <= 0 or height <= 0:
        return None
    return width, height


def measure(address: str, measured: dict, failures: list[str]) -> None:
    runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    expected = os.path.join(runtime, "pipewire-0") if runtime else ""
    measured["expected_peer"] = expected
    flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
    connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
    responses = Responses(connection)
    session_path = None
    fd = None
    try:
        measured["unique_name"] = connection.get_unique_name() or ""
        connection.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "GetNameOwner",
            GLib.Variant("(s)", (BUS_NAME,)),
            GLib.VariantType.new("(s)"),
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        )
        created = call_request(
            connection,
            responses,
            REMOTE,
            "CreateSession",
            GLib.Variant(
                "(a{sv})",
                (
                    dict_variant(
                        {
                            "handle_token": ("s", "proxy_create"),
                            "session_handle_token": ("s", "proxy_ses"),
                        }
                    ),
                ),
            ),
            "CreateSession",
            failures,
        )
        if created is None:
            return
        request_path, code, results = created
        measured["create_request"] = request_path
        measured["create_response"] = code
        session_path = results.get("session_handle") if isinstance(results, dict) else None
        measured["session_handle"] = session_path if isinstance(session_path, str) else ""
        if not isinstance(session_path, str) or not session_path.startswith(f"{OBJECT_PATH}/session/"):
            failures.append(f"session_handle {session_path!r}")
            session_path = None
            return
        if code != 0:
            return

        devices = call_request(
            connection,
            responses,
            REMOTE,
            "SelectDevices",
            GLib.Variant(
                "(oa{sv})",
                (
                    session_path,
                    dict_variant(
                        {
                            "handle_token": ("s", "proxy_devices"),
                            "types": ("u", 3),
                        }
                    ),
                ),
            ),
            "SelectDevices",
            failures,
        )
        if devices is None:
            return
        measured["select_devices_response"] = devices[1]
        if devices[1] != 0:
            return

        sources = call_request(
            connection,
            responses,
            SCREEN,
            "SelectSources",
            GLib.Variant(
                "(oa{sv})",
                (
                    session_path,
                    dict_variant(
                        {
                            "handle_token": ("s", "proxy_sources"),
                            "multiple": ("b", True),
                            "types": ("u", 1),
                            "cursor_mode": ("u", 4),
                        }
                    ),
                ),
            ),
            "SelectSources",
            failures,
        )
        if sources is None:
            return
        measured["select_sources_response"] = sources[1]
        if sources[1] != 0:
            return

        started = call_request(
            connection,
            responses,
            REMOTE,
            "Start",
            GLib.Variant(
                "(osa{sv})",
                (
                    session_path,
                    "",
                    dict_variant({"handle_token": ("s", "proxy_start")}),
                ),
            ),
            "Start",
            failures,
        )
        if started is None:
            return
        measured["start_response"] = started[1]
        if started[1] != 0:
            return
        results = started[2] if isinstance(started[2], dict) else {}
        streams = results.get("streams")
        measured["stream_count"] = 0 if not isinstance(streams, (list, tuple)) else len(streams)
        if not streams:
            failures.append("streams boş")
            return
        node_id, props = streams[0]
        if not isinstance(props, dict):
            failures.append("akış özellikleri yok")
            return
        size = positive_pair(props.get("size"))
        position = props.get("position")
        mapping = props.get("mapping_id")
        measured["stream_node"] = node_id
        measured["mapping_id"] = mapping if isinstance(mapping, str) else ""
        if isinstance(position, tuple):
            measured["position"] = ",".join(str(item) for item in position)
        else:
            measured["position"] = ""
        if size is None:
            failures.append(f"size {props.get('size')!r}")
            measured["size"] = ""
        else:
            measured["size"] = f"{size[0]},{size[1]}"
        if isinstance(node_id, bool) or not isinstance(node_id, int) or node_id <= 0:
            failures.append(f"stream_node {node_id!r}")
        if not isinstance(mapping, str) or mapping == "":
            failures.append(f"mapping_id {mapping!r}")
        if failures:
            return

        try:
            fd = call_fd(connection, "OpenPipeWireRemote", session_path)
        except (GLib.Error, RuntimeError, TypeError, ValueError) as exc:
            message = exc.message if isinstance(exc, GLib.Error) else str(exc)
            failures.append(f"OpenPipeWireRemote: {one_line(message)}")
            return
        ok, detail = fd_is_socket(fd)
        measured["pipewire_fd_valid"] = detail
        if not ok:
            failures.append(f"OpenPipeWireRemote fd geçersiz: {detail}")
            return
        try:
            facts = classify_pipewire_fd(fd)
        except OSError as exc:
            failures.append(f"OpenPipeWireRemote ucu sınıflanamadı: {exc}")
            return
        measured["pipewire_fd_kind"] = facts["kind"]
        measured["pipewire_fd_domain"] = facts["domain"]
        measured["pipewire_fd_type"] = facts["type"]
        measured["pipewire_peer_path"] = facts["peer_path"]
        measured["pipewire_peer_pid"] = facts["peer_pid"]
        if facts["kind"] == "socketpair":
            failures.append("OpenPipeWireRemote socketpair döndürdü")
        elif facts["kind"] != "pipewire":
            failures.append(f"OpenPipeWireRemote pipewire soketi değil: {facts['peer_path']}")
        elif facts["peer_path"] != expected or expected == "":
            failures.append(f"peer {facts['peer_path']!r} beklenen {expected!r}")
    finally:
        if fd is not None:
            os.close(fd)
            fd = None
        if session_path and "session_closed" not in measured:
            close_session(connection, session_path, measured)
        responses.close()
        try:
            connection.close_sync(None)
        except GLib.Error:
            pass


def main() -> int:
    if len(sys.argv) != 3:
        print("kullanım: crd-proxy-measure.py <host-soketi> <çıktı>", file=sys.stderr)
        return 2
    address = sys.argv[1]
    result_path = sys.argv[2]
    measured: dict = {}
    failures: list[str] = []
    exit_code = 1
    try:
        if not refuse_address(address, failures):
            exit_code = 2
        else:
            measure(address, measured, failures)
            exit_code = 0 if not failures else 1
    except Exception as exc:
        failures.append(f"{type(exc).__name__}: {one_line(str(exc))}")
        exit_code = 1
    finally:
        try:
            write_facts(result_path, measured, failures)
        except OSError as exc:
            print(f"çıktı yazılamadı: {exc}", file=sys.stderr)
            exit_code = 1
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
