#!/usr/bin/env python3
"""ScreenCast.CreateSession ve ScreenCast.Start ölçümü.

Özel bir dbus-daemon açar. Gerçek oturum veriyolunda ada sahip çıkılmaz.
RemoteDesktop oturumu aynı süreçte ayrı yol olarak açılır; ikisi de aynı
yakalama akışını bildirir. Canlı giriş kapısı açılmaz.
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib

import probe

ROOT = os.path.dirname(os.path.abspath(__file__))
EVIDENCE = os.path.join(ROOT, "kanit", "screencast.txt")
BUS_NAME = probe.BUS_NAME
OBJECT_PATH = probe.OBJECT_PATH
REMOTE = probe.REMOTE
SCREEN = probe.SCREEN
CLIPBOARD = probe.CLIPBOARD


def expect_fail(connection, interface, method, parameters, label, failures):
    try:
        connection.call_sync(
            BUS_NAME,
            OBJECT_PATH,
            interface,
            method,
            parameters,
            None,
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        )
    except GLib.Error as exc:
        return exc.message
    failures.append(f"{label} kabul edildi")
    return None


def stream_of(results, label, failures):
    streams = None if results is None else results.get("streams")
    if not streams:
        failures.append(f"{label} streams boş")
        return None
    node_id, props = streams[0]
    position = tuple(props.get("position", ()))
    size = tuple(props.get("size", ()))
    if int(node_id) <= 0:
        failures.append(f"{label} node {node_id}")
    if props.get("mapping_id") != "HDMI-A-1":
        failures.append(f"{label} mapping_id {props.get('mapping_id')!r}")
    if size != (2560, 1440):
        failures.append(f"{label} size {size}")
    if position != (0, 0):
        failures.append(f"{label} position {position}")
    if props.get("source_type") != 1:
        failures.append(f"{label} source_type {props.get('source_type')!r}")
    return node_id, props, position, size


def classify(fd, label, measured, failures):
    ok, detail = probe.fd_is_socket(fd)
    measured[f"{label}_fd_valid"] = detail
    if not ok:
        failures.append(f"{label} fd geçersiz: {detail}")
        return None
    try:
        facts = probe.classify_pipewire_fd(fd)
    except OSError as exc:
        failures.append(f"{label} sınıflanamadı: {exc}")
        return None
    measured[f"{label}_fd_kind"] = facts["kind"]
    measured[f"{label}_peer_path"] = facts["peer_path"]
    measured[f"{label}_peer_pid"] = facts["peer_pid"]
    if facts["kind"] == "socketpair":
        failures.append(f"{label} socketpair döndürdü")
    elif facts["kind"] != "pipewire":
        failures.append(f"{label} pipewire soketi değil: {facts['peer_path']}")
    return facts


def write_evidence(measured, failures):
    os.makedirs(os.path.dirname(EVIDENCE), exist_ok=True)
    lines = ["=== screencast ==="]
    for key in sorted(measured):
        lines.append(f"{key}={measured[key]}")
    if failures:
        lines.append("failures:")
        for item in failures:
            lines.append(f"- {item}")
    else:
        lines.append("failures=none")
    with open(EVIDENCE, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main():
    failures = []
    measured = {}
    daemon = None
    portal = None
    work = None
    connection = None
    opened = []

    if not os.path.isfile(probe.BINARY):
        failures.append(f"ikili yok: {probe.BINARY}")
        write_evidence(measured, failures)
        return 1

    try:
        probe.refuse_real_bus(measured, failures)

        work = tempfile.mkdtemp(prefix="crd-screencast-")
        socket_path = os.path.join(work, "bus")
        config_path = os.path.join(work, "bus.conf")
        with open(config_path, "w", encoding="utf-8") as handle:
            handle.write(textwrap.dedent(f"""\
                <!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
                 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
                <busconfig>
                  <listen>unix:path={socket_path}</listen>
                  <type>session</type>
                  <auth>EXTERNAL</auth>
                  <policy context="default">
                    <allow own="*"/>
                    <allow send_destination="*"/>
                    <allow receive_sender="*"/>
                    <allow send_interface="*"/>
                    <allow receive_type="method_call"/>
                    <allow receive_type="method_return"/>
                    <allow receive_type="error"/>
                    <allow receive_type="signal"/>
                  </policy>
                </busconfig>
                """))

        daemon = subprocess.Popen(
            ["dbus-daemon", "--config-file", config_path, "--nofork", "--nopidfile", "--nosyslog", "--print-address=1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        address = daemon.stdout.readline().strip()
        measured["private_bus"] = address
        real = os.environ.get("DBUS_SESSION_BUS_ADDRESS", "")
        if not address.startswith("unix:"):
            failures.append(f"dbus-daemon adres vermedi: {address!r}")
            return 1
        if real and address.split(",guid=")[0] == real.split(",guid=")[0]:
            failures.append("özel veriyolu gerçek oturum veriyolu")
            return 1

        portal_env = os.environ.copy()
        portal_env.pop("CRD_OLAY_KANIT", None)
        portal = subprocess.Popen(
            [probe.BINARY, address],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=portal_env,
        )
        flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        probe.wait_for_name(connection, failures)
        if any("ad alınamadı" in item for item in failures):
            return 1

        measured["screencast_version"] = probe.get_property(connection, SCREEN, "version")
        if measured["screencast_version"] != probe.EXPECT_SC_VERSION:
            failures.append(f"ScreenCast.version {measured['screencast_version']}")

        unique = connection.get_unique_name()
        screen_path = probe.handle_path("session", unique, "cast_session")
        remote_path = probe.handle_path("session", unique, "desk_session")
        measured["screencast_session"] = screen_path
        measured["remote_session"] = remote_path
        if screen_path == remote_path:
            failures.append("oturum yolları aynı")

        created = probe.call_request(
            connection,
            SCREEN,
            "CreateSession",
            GLib.Variant("(a{sv})", (probe.dict_variant({
                "handle_token": ("s", "cast_create"),
                "session_handle_token": ("s", "cast_session"),
            }),)),
            "cast_create",
            failures,
            "ScreenCast.CreateSession",
        )
        if created is None:
            return 1
        measured["response_create"] = created["code"]
        measured["session_handle"] = created["results"].get("session_handle")
        if created["results"].get("session_handle") != screen_path:
            failures.append(f"session_handle {created['results'].get('session_handle')!r}")

        early = expect_fail(
            connection,
            SCREEN,
            "Start",
            GLib.Variant("(osa{sv})", (screen_path, "", probe.dict_variant({
                "handle_token": ("s", "cast_early"),
            }))),
            "ScreenCast.Start kaynaksız",
            failures,
        )
        measured["start_without_sources"] = "rejected" if early else "accepted"

        devices_error = expect_fail(
            connection,
            REMOTE,
            "SelectDevices",
            GLib.Variant("(oa{sv})", (screen_path, probe.dict_variant({
                "handle_token": ("s", "cast_devices"),
                "types": ("u", 3),
            }))),
            "RemoteDesktop.SelectDevices ekran oturumunda",
            failures,
        )
        measured["screencast_select_devices"] = "rejected" if devices_error else "accepted"

        clip_error = expect_fail(
            connection,
            CLIPBOARD,
            "RequestClipboard",
            GLib.Variant("(oa{sv})", (screen_path, {})),
            "RequestClipboard ekran oturumunda",
            failures,
        )
        measured["screencast_clipboard"] = "rejected" if clip_error else "accepted"

        sources = probe.call_request(
            connection,
            SCREEN,
            "SelectSources",
            GLib.Variant("(oa{sv})", (screen_path, probe.dict_variant({
                "handle_token": ("s", "cast_sources"),
                "multiple": ("b", True),
                "types": ("u", 1),
                "cursor_mode": ("u", 4),
            }))),
            "cast_sources",
            failures,
            "ScreenCast.SelectSources",
        )
        if sources is None:
            return 1
        measured["response_select_sources"] = sources["code"]

        remote_start_error = expect_fail(
            connection,
            REMOTE,
            "Start",
            GLib.Variant("(osa{sv})", (screen_path, "", probe.dict_variant({
                "handle_token": ("s", "cast_remote_start"),
            }))),
            "RemoteDesktop.Start ekran oturumunda",
            failures,
        )
        measured["screencast_remote_start"] = "rejected" if remote_start_error else "accepted"

        started = probe.call_request(
            connection,
            SCREEN,
            "Start",
            GLib.Variant("(osa{sv})", (screen_path, "", probe.dict_variant({
                "handle_token": ("s", "cast_start"),
            }))),
            "cast_start",
            failures,
            "ScreenCast.Start",
        )
        if started is None:
            return 1
        measured["response_start"] = started["code"]
        if "devices" in started["results"] or "clipboard_enabled" in started["results"]:
            failures.append("ScreenCast.Start RemoteDesktop sonucu döndürdü")
        parsed = stream_of(started["results"], "ScreenCast.Start", failures)
        if parsed is not None:
            node_id, props, position, size = parsed
            measured["stream_count"] = len(started["results"].get("streams") or [])
            measured["stream_node"] = node_id
            measured["mapping_id"] = props.get("mapping_id")
            measured["source_type"] = props.get("source_type")
            measured["position"] = ",".join(str(item) for item in position)
            measured["size"] = ",".join(str(item) for item in size)
        else:
            measured["stream_count"] = 0

        eis_error = expect_fail(
            connection,
            REMOTE,
            "ConnectToEIS",
            GLib.Variant("(oa{sv})", (screen_path, {})),
            "ConnectToEIS ekran oturumunda",
            failures,
        )
        measured["screencast_eis"] = "rejected" if eis_error else "accepted"

        pipewire_fd = probe.call_fd(connection, SCREEN, "OpenPipeWireRemote", screen_path)
        opened.append(pipewire_fd)
        facts = classify(pipewire_fd, "pipewire", measured, failures)
        if facts is not None and facts["kind"] == "pipewire" and parsed is not None:
            width, height = parsed[3]
            captured = probe.run_frame_consumer(
                pipewire_fd,
                parsed[0],
                width,
                height,
                measured,
                failures,
            )
            measured["stream_node_kind"] = "capture" if captured else "unverified"
        elif "stream_node_kind" not in measured:
            measured["stream_node_kind"] = "unverified"

        remote_created = probe.call_request(
            connection,
            REMOTE,
            "CreateSession",
            GLib.Variant("(a{sv})", (probe.dict_variant({
                "handle_token": ("s", "desk_create"),
                "session_handle_token": ("s", "desk_session"),
            }),)),
            "desk_create",
            failures,
            "RemoteDesktop.CreateSession",
        )
        if remote_created is None:
            return 1
        measured["remote_response_create"] = remote_created["code"]
        if remote_created["results"].get("session_handle") != remote_path:
            failures.append("RemoteDesktop session_handle ekran oturumuyla karıştı")

        screen_on_remote = expect_fail(
            connection,
            SCREEN,
            "Start",
            GLib.Variant("(osa{sv})", (remote_path, "", probe.dict_variant({
                "handle_token": ("s", "desk_cast_start"),
            }))),
            "ScreenCast.Start masaüstü oturumunda",
            failures,
        )
        measured["remote_screencast_start"] = "rejected" if screen_on_remote else "accepted"

        remote_devices = probe.call_request(
            connection,
            REMOTE,
            "SelectDevices",
            GLib.Variant("(oa{sv})", (remote_path, probe.dict_variant({
                "handle_token": ("s", "desk_devices"),
                "types": ("u", 3),
            }))),
            "desk_devices",
            failures,
            "RemoteDesktop.SelectDevices",
        )
        if remote_devices is None:
            return 1
        measured["remote_response_select_devices"] = remote_devices["code"]

        remote_sources = probe.call_request(
            connection,
            SCREEN,
            "SelectSources",
            GLib.Variant("(oa{sv})", (remote_path, probe.dict_variant({
                "handle_token": ("s", "desk_sources"),
                "multiple": ("b", True),
                "types": ("u", 1),
                "cursor_mode": ("u", 4),
            }))),
            "desk_sources",
            failures,
            "RemoteDesktop SelectSources",
        )
        if remote_sources is None:
            return 1
        measured["remote_response_select_sources"] = remote_sources["code"]

        remote_started = probe.call_request(
            connection,
            REMOTE,
            "Start",
            GLib.Variant("(osa{sv})", (remote_path, "", probe.dict_variant({
                "handle_token": ("s", "desk_start"),
            }))),
            "desk_start",
            failures,
            "RemoteDesktop.Start",
        )
        if remote_started is None:
            return 1
        measured["remote_response_start"] = remote_started["code"]
        remote_parsed = stream_of(remote_started["results"], "RemoteDesktop.Start", failures)
        if remote_parsed is not None:
            measured["remote_stream_node"] = remote_parsed[0]
            if parsed is not None and int(remote_parsed[0]) != int(parsed[0]):
                failures.append(f"akış düğümleri farklı: {parsed[0]} != {remote_parsed[0]}")
            else:
                measured["streams_same_node"] = "yes"
        remote_fd = probe.call_fd(connection, SCREEN, "OpenPipeWireRemote", remote_path)
        opened.append(remote_fd)
        classify(remote_fd, "remote_pipewire", measured, failures)

        if measured.get("real_owner_before") != measured.get("real_owner_after"):
            failures.append("gerçek portal sahibi değişti")
        if measured.get("stream_node_kind") != "capture":
            if not any("kare" in item or "socketpair" in item or "pipewire" in item for item in failures):
                failures.append("akış düğümü doğrulanmadı")
        return 0 if not failures else 1
    except Exception as exc:
        failures.append(f"{type(exc).__name__}: {exc}")
        return 1
    finally:
        for fd in opened:
            try:
                os.close(fd)
            except OSError:
                pass
        if connection is not None:
            try:
                connection.close_sync(None)
            except GLib.Error:
                pass
        probe.stop_process(portal)
        probe.stop_process(daemon)
        if portal is not None and portal.stderr is not None:
            stderr = portal.stderr.read().strip().replace("\n", " | ")
            measured["portal_stderr"] = stderr
            if "socketpair" in stderr or "placeholder" in stderr:
                failures.append("portal yer tutucu izi bıraktı")
            if "giris_set_live_send" in stderr:
                failures.append("canlı kapı izi")
        if daemon is not None and daemon.stderr is not None:
            daemon_err = daemon.stderr.read().strip()
            if daemon_err:
                measured["daemon_stderr"] = daemon_err.replace("\n", " | ")
        if portal is not None and portal.poll() is None:
            portal.send_signal(signal.SIGKILL)
            portal.wait(timeout=2)
        if daemon is not None and daemon.poll() is None:
            daemon.send_signal(signal.SIGKILL)
            daemon.wait(timeout=2)
        if work is not None:
            shutil.rmtree(work, ignore_errors=True)
        write_evidence(measured, failures)


if __name__ == "__main__":
    sys.exit(main())
