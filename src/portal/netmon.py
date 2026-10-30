#!/usr/bin/env python3
"""NetworkMonitor sürüm 3'ü crd-portal üzerinde ölçer.

Değerler portalın döndürdüğü sonuç ile bu süreçteki GNetworkMonitor'dan
okunur. Sabit çevrimiçi beklenmez. Gerçek oturum veriyolunda
org.freedesktop.portal.Desktop adına sahip çıkılmaz.
"""

import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import xml.etree.ElementTree as ET

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib

import probe

ROOT = os.path.dirname(os.path.abspath(__file__))
EVIDENCE = os.path.join(ROOT, "kanit", "netmon.txt")
DOCUMENT = "/usr/share/dbus-1/interfaces/org.freedesktop.portal.NetworkMonitor.xml"
IFACE = "org.freedesktop.portal.NetworkMonitor"
SYMBOL = "netmon_register"
CONNECTIVITY = {
    1: "yalnız yerel",
    2: "sınırlı",
    3: "kısıtlı kapı",
    4: "tam ağ",
}


def dbus_bool(value):
    return "true" if bool(value) else "false"


def nm_map(path):
    text = subprocess.check_output(["nm", path], text=True, stderr=subprocess.STDOUT)
    found = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[0] == "U":
            found[parts[-1]] = "U"
        elif len(parts) >= 3:
            found[parts[-1]] = parts[-2]
    return found


def call(connection, method, parameters, reply, timeout):
    return connection.call_sync(
        probe.BUS_NAME,
        probe.OBJECT_PATH,
        IFACE,
        method,
        parameters,
        GLib.VariantType.new(reply),
        Gio.DBusCallFlags.NONE,
        timeout,
        None,
    )


def call_checked(connection, method, parameters, reply, timeout):
    try:
        value = call(connection, method, parameters, reply, timeout)
    except GLib.Error as exc:
        return None, exc.message
    return probe.unwrap(value), None


def gio_status():
    monitor = Gio.NetworkMonitor.get_default()
    connectivity = int(monitor.get_connectivity())
    return {
        "type": monitor.__gtype__.name,
        "available": bool(monitor.get_network_available()),
        "metered": bool(monitor.get_network_metered()),
        "connectivity": connectivity,
    }


def gio_can_reach(hostname, port, timeout_ms):
    monitor = Gio.NetworkMonitor.get_default()
    address = Gio.NetworkAddress.new(hostname, port)
    holder = {}
    loop = GLib.MainLoop()

    def finish(source, result):
        try:
            holder["value"] = bool(source.can_reach_finish(result))
        except GLib.Error as exc:
            holder["error"] = exc.message
        if loop.is_running():
            loop.quit()

    monitor.can_reach_async(address, None, finish)
    GLib.timeout_add(timeout_ms, lambda: loop.quit() or False)
    loop.run()
    if "value" not in holder and "error" not in holder:
        holder["error"] = "GNetworkMonitor.can_reach zaman aşımı"
    return holder


def changed_in_introspection(xml_text):
    root = ET.fromstring(xml_text)
    for interface in root.findall("interface"):
        if interface.get("name") != IFACE:
            continue
        for item in interface.findall("signal"):
            if item.get("name") == "changed" and len(list(item)) == 0:
                return True
    return False


def write_evidence(measured, failures):
    os.makedirs(os.path.dirname(EVIDENCE), exist_ok=True)
    lines = ["=== crd-portal NetworkMonitor ==="]
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
    rc = 1

    measured["belge"] = DOCUMENT
    try:
        digest = hashlib.sha256(open(DOCUMENT, "rb").read()).hexdigest()
    except OSError as exc:
        failures.append(f"belge okunamadı: {exc}")
        digest = "absent"
    measured["belge_sha256"] = digest
    measured["kaynak"] = "src/portal/portal.c"

    if not os.path.isfile(probe.BINARY):
        failures.append(f"ikili yok: {probe.BINARY}")
        write_evidence(measured, failures)
        return 1

    try:
        object_path = os.path.join(ROOT, "portal.o")
        portal_nm = nm_map(object_path)
        binary_nm = nm_map(probe.BINARY)
        linked = (
            portal_nm.get(SYMBOL) == "U"
            and portal_nm.get("netmon_unregister") == "U"
            and binary_nm.get(SYMBOL) in ("T", "t")
            and binary_nm.get("netmon_unregister") in ("T", "t")
            and "crd_portal_network_status" not in portal_nm
            and "g_network_monitor_get_default" not in portal_nm
        )
        measured["kayit"] = SYMBOL
        measured["portal.c_linked"] = "yes" if linked else "no"
        if not linked:
            failures.append(f"{SYMBOL} portal.o üzerinden libcrd_netmon ile bağlı değil")

        probe.refuse_real_bus(measured, failures)
        owner_before = measured.get("real_owner_before", "absent")

        work = tempfile.mkdtemp(prefix="crd-portal-netmon-")
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

        exe = os.readlink(f"/proc/{portal.pid}/exe")
        measured["portal_exe"] = exe
        if os.path.basename(exe) != "crd-portal":
            failures.append(f"portal süreç adı {exe}")
        cmdline = open(f"/proc/{portal.pid}/cmdline", "rb").read().replace(b"\0", b" ").decode()
        if "crd-netmon" in cmdline:
            failures.append("ayrı crd-netmon süreci")

        owner_during = probe.session_name_owner(real) if real else "unset"
        measured["real_owner_during"] = owner_during
        if owner_during != owner_before:
            failures.append(f"gerçek sahip ölçüm sırasında değişti: {owner_before} -> {owner_during}")

        gio = gio_status()
        measured["gio_type"] = gio["type"]
        measured["gio_available"] = dbus_bool(gio["available"])
        measured["gio_metered"] = dbus_bool(gio["metered"])
        measured["gio_connectivity"] = gio["connectivity"]

        status, status_error = call_checked(connection, "GetStatus", None, "(a{sv})", 5000)
        if status_error is not None:
            failures.append(f"GetStatus: {status_error}")
        else:
            body = status[0]
            available = bool(body.get("available"))
            metered = bool(body.get("metered"))
            connectivity = body.get("connectivity")
            measured["GetStatus"] = (
                f'a{{sv}} available={dbus_bool(available)} '
                f'metered={dbus_bool(metered)} connectivity={connectivity}'
            )
            measured["available"] = dbus_bool(available)
            measured["metered"] = dbus_bool(metered)
            measured["connectivity"] = connectivity
            measured["anlam"] = CONNECTIVITY.get(int(connectivity), "belgede yok")
            keys = set(body)
            if keys != {"available", "metered", "connectivity"}:
                failures.append(f"GetStatus anahtarları {sorted(keys)}")
            if not isinstance(body.get("available"), bool) or not isinstance(body.get("metered"), bool):
                failures.append("GetStatus available/metered boolean değil")
            if not isinstance(connectivity, int):
                failures.append(f"GetStatus connectivity tipi {type(connectivity).__name__}")
            if available != gio["available"] or metered != gio["metered"] or int(connectivity) != gio["connectivity"]:
                failures.append(
                    "GetStatus GNetworkMonitor ile uyuşmuyor: "
                    f"portal {available},{metered},{connectivity} "
                    f"gio {gio['available']},{gio['metered']},{gio['connectivity']}"
                )

        for method, key, gio_key in (
            ("GetAvailable", "GetAvailable", "available"),
            ("GetMetered", "GetMetered", "metered"),
        ):
            value, error = call_checked(connection, method, None, "(b)", 5000)
            if error is not None:
                failures.append(f"{method}: {error}")
                continue
            measured[key] = f"b {dbus_bool(value[0])}"
            if bool(value[0]) != gio[gio_key]:
                failures.append(f"{method} GNetworkMonitor ile uyuşmuyor")
            if "available" in measured and method == "GetAvailable" and dbus_bool(value[0]) != measured["available"]:
                failures.append("GetAvailable GetStatus ile uyuşmuyor")
            if "metered" in measured and method == "GetMetered" and dbus_bool(value[0]) != measured["metered"]:
                failures.append("GetMetered GetStatus ile uyuşmuyor")

        connectivity_value, connectivity_error = call_checked(connection, "GetConnectivity", None, "(u)", 5000)
        if connectivity_error is not None:
            failures.append(f"GetConnectivity: {connectivity_error}")
        else:
            measured["GetConnectivity"] = f"u {connectivity_value[0]}"
            if int(connectivity_value[0]) != gio["connectivity"]:
                failures.append("GetConnectivity GNetworkMonitor ile uyuşmuyor")
            if "connectivity" in measured and int(connectivity_value[0]) != int(measured["connectivity"]):
                failures.append("GetConnectivity GetStatus ile uyuşmuyor")

        try:
            version = connection.call_sync(
                probe.BUS_NAME,
                probe.OBJECT_PATH,
                "org.freedesktop.DBus.Properties",
                "Get",
                GLib.Variant("(ss)", (IFACE, "version")),
                GLib.VariantType.new("(v)"),
                Gio.DBusCallFlags.NONE,
                5000,
                None,
            )
        except GLib.Error as exc:
            failures.append(f"version: {exc.message}")
        else:
            unpacked = probe.unwrap(version)
            measured["version"] = unpacked[0]
            if unpacked[0] != 3:
                failures.append(f"version {unpacked[0]}")

        xml_value = connection.call_sync(
            probe.BUS_NAME,
            probe.OBJECT_PATH,
            "org.freedesktop.DBus.Introspectable",
            "Introspect",
            None,
            GLib.VariantType.new("(s)"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )
        xml_text = probe.unwrap(xml_value)[0]
        changed = changed_in_introspection(xml_text)
        measured["changed"] = "yes" if changed else "no"
        if not changed:
            failures.append("changed sinyali arayüzde yok")

        reached, reach_error = call_checked(
            connection,
            "CanReach",
            GLib.Variant("(su)", ("example.com", 443)),
            "(b)",
            20000,
        )
        gio_reach = gio_can_reach("example.com", 443, 20000)
        measured["gio_CanReach_example.com_443"] = (
            dbus_bool(gio_reach["value"]) if "value" in gio_reach else f"error:{gio_reach.get('error')}"
        )
        if reach_error is not None:
            failures.append(f"CanReach: {reach_error}")
        else:
            measured["CanReach_example.com_443"] = f"b {dbus_bool(reached[0])}"
            if "value" not in gio_reach:
                failures.append(f"GNetworkMonitor.can_reach: {gio_reach.get('error')}")
            elif bool(reached[0]) != gio_reach["value"]:
                failures.append("CanReach GNetworkMonitor ile uyuşmuyor")

        empty, empty_error = call_checked(
            connection,
            "CanReach",
            GLib.Variant("(su)", ("", 443)),
            "(b)",
            5000,
        )
        measured["empty_hostname"] = empty_error or repr(empty)
        if empty_error is None:
            failures.append("boş hostname kabul edildi")

        wide, wide_error = call_checked(
            connection,
            "CanReach",
            GLib.Variant("(su)", ("example.com", 70000)),
            "(b)",
            5000,
        )
        measured["port_70000"] = wide_error or repr(wide)
        if wide_error is None:
            failures.append("kapı 70000 kabul edildi")

    except Exception as exc:
        failures.append(f"{type(exc).__name__}: {exc}")
        return 1
    finally:
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
            if "ağ kaynağı:" in stderr:
                failures.append("portal ikinci ağ izleyicisini tutuyor")
        if portal is not None and portal.poll() is None:
            portal.send_signal(signal.SIGKILL)
            portal.wait(timeout=2)
        if daemon is not None and daemon.poll() is None:
            daemon.send_signal(signal.SIGKILL)
            daemon.wait(timeout=2)
        real = os.environ.get("DBUS_SESSION_BUS_ADDRESS", "")
        if real:
            owner_after = probe.session_name_owner(real)
            measured["real_owner_after"] = owner_after
            if measured.get("real_owner_before") not in (None, owner_after):
                if not any("gerçek" in item for item in failures):
                    failures.append(
                        f"gerçek sahip sonda değişti: {measured.get('real_owner_before')} -> {owner_after}"
                    )
        if work is not None:
            shutil.rmtree(work, ignore_errors=True)
        rc = 0 if not failures else 1
        write_evidence(measured, failures)
    return rc


if __name__ == "__main__":
    sys.exit(main())
