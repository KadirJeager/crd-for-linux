#!/usr/bin/env python3
"""Özel bir dbus-daemon açar, crd-portal'ı ona bağlar, portal zincirini ölçer.

Çıkış kodu 0 yalnız ölçülen koşullar tutuyorsa. Gerçek oturum veriyolunda
org.freedesktop.portal.Desktop adına sahip çıkılmaz.
"""

import hashlib
import os
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import textwrap
import time

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib

ROOT = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(ROOT, "crd-portal")
EIS_PROBE = os.path.join(ROOT, "eis-probe")
CLIP_IO = os.path.join(ROOT, "clip-io")
EVIDENCE = os.path.join(ROOT, "kanit", "probe.txt")
EIS_EVIDENCE = os.path.join(ROOT, "kanit", "eis.txt")
OLAY_EVIDENCE = os.path.join(ROOT, "kanit", "olay.txt")
PANO_EVIDENCE = os.path.join(ROOT, "kanit", "pano.txt")
FRAME = os.path.join(ROOT, "frame-al")
FRAME_PPM = os.path.join(ROOT, "kanit", "frame.ppm")
OLCUM = os.path.join(ROOT, "kanit", "olcum.txt")
PAYLOAD = b"crd-portal-pano-test-2026-10-29"
SO_DOMAIN = getattr(socket, "SO_DOMAIN", 39)
BUS_NAME = "org.freedesktop.portal.Desktop"
OBJECT_PATH = "/org/freedesktop/portal/desktop"
REMOTE = "org.freedesktop.portal.RemoteDesktop"
SCREEN = "org.freedesktop.portal.ScreenCast"
CLIPBOARD = "org.freedesktop.portal.Clipboard"
REQUEST = "org.freedesktop.portal.Request"
SESSION = "org.freedesktop.portal.Session"

# Belge revizyonları (29 Ekim 2026): RemoteDesktop 2, ScreenCast 6, Clipboard 1.
EXPECT_RD_VERSION = 2
EXPECT_SC_VERSION = 6
EXPECT_CLIP_VERSION = 1


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


def sender_element(unique_name):
    if not unique_name or not unique_name.startswith(":"):
        raise RuntimeError(f"eşsiz ad yok: {unique_name!r}")
    return unique_name[1:].replace(".", "_")


def handle_path(kind, unique_name, token):
    return f"{OBJECT_PATH}/{kind}/{sender_element(unique_name)}/{token}"


def stop_process(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)


def session_name_owner(address):
    flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
    connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
    try:
        value = connection.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "GetNameOwner",
            GLib.Variant("(s)", (BUS_NAME,)),
            GLib.VariantType.new("(s)"),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        return unwrap(value)[0]
    except GLib.Error as exc:
        if "NameHasNoOwner" in exc.message or "not found" in exc.message.lower():
            return "absent"
        return f"error:{exc.message}"
    finally:
        connection.close_sync(None)


def refuse_real_bus(measured, failures):
    real = os.environ.get("DBUS_SESSION_BUS_ADDRESS", "")
    measured["real_session_bus"] = real or "unset"
    if not real:
        failures.append("DBUS_SESSION_BUS_ADDRESS yok; red sınaması yapılamadı")
        return
    before = session_name_owner(real)
    measured["real_owner_before"] = before
    proc = subprocess.run(
        [BINARY, real],
        capture_output=True,
        text=True,
        timeout=2,
        check=False,
    )
    measured["refusal_exit"] = proc.returncode
    measured["refusal_stderr"] = proc.stderr.strip()
    after = session_name_owner(real)
    measured["real_owner_after"] = after
    if proc.returncode != 3:
        failures.append(f"gerçek veriyolu red çıkışı {proc.returncode}, beklenen 3")
    if "reddedildi" not in proc.stderr:
        failures.append("red mesajı yok")
    if before != after:
        failures.append(f"gerçek veriyolu sahibi değişti: {before} -> {after}")


def wait_for_name(connection, failures):
    deadline = GLib.get_monotonic_time() + 20 * GLib.USEC_PER_SEC
    last_error = "ad görünmedi"
    while GLib.get_monotonic_time() < deadline:
        try:
            connection.call_sync(
                "org.freedesktop.DBus",
                "/org/freedesktop/DBus",
                "org.freedesktop.DBus",
                "GetNameOwner",
                GLib.Variant("(s)", (BUS_NAME,)),
                GLib.VariantType.new("(s)"),
                Gio.DBusCallFlags.NONE,
                500,
                None,
            )
            return
        except GLib.Error as exc:
            last_error = exc.message
            pump(50)
    failures.append(f"özel veriyolunda ad alınamadı: {last_error}")


def pump(milliseconds):
    loop = GLib.MainLoop()
    GLib.timeout_add(milliseconds, lambda: loop.quit() or False)
    loop.run()


def call_request(connection, interface, method, parameters, token, failures, label):
    unique = connection.get_unique_name()
    path = handle_path("request", unique, token)
    holder = {}
    loop = GLib.MainLoop()

    def on_signal(conn, sender, object_path, iface, signal_name, params):
        code, results = unwrap(params)
        holder["code"] = code
        holder["results"] = results
        holder["signal_path"] = object_path
        if loop.is_running():
            loop.quit()

    subscription = connection.signal_subscribe(
        BUS_NAME,
        REQUEST,
        "Response",
        path,
        None,
        Gio.DBusSignalFlags.NONE,
        on_signal,
    )
    try:
        reply = connection.call_sync(
            BUS_NAME,
            OBJECT_PATH,
            interface,
            method,
            parameters,
            GLib.VariantType.new("(o)"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )
        returned = unwrap(reply)[0]
        holder["returned"] = returned
        if "code" not in holder:
            GLib.timeout_add(3000, lambda: loop.quit() or False)
            loop.run()
    finally:
        connection.signal_unsubscribe(subscription)

    if holder.get("returned") != path:
        failures.append(f"{label} dönüş yolu {holder.get('returned')!r} != {path}")
    if holder.get("signal_path") not in (None, path):
        failures.append(f"{label} sinyal yolu {holder.get('signal_path')}")
    if "code" not in holder:
        failures.append(f"{label} Response gelmedi")
        return None
    if holder["code"] != 0:
        failures.append(f"{label} Response kodu {holder['code']}")
    close_reply = connection.call_sync(
        BUS_NAME,
        path,
        REQUEST,
        "Close",
        GLib.Variant.new_tuple(),
        GLib.VariantType.new("()"),
        Gio.DBusCallFlags.NONE,
        3000,
        None,
    )
    if unwrap(close_reply) != ():
        failures.append(f"{label} Request.Close dönüşü {close_reply}")
    return holder


def dict_variant(fields):
    return {key: GLib.Variant(kind, value) for key, (kind, value) in fields.items()}


def fd_is_socket(fd):
    try:
        mode = os.fstat(fd).st_mode
    except OSError as exc:
        return False, f"fstat:{exc}"
    if not stat.S_ISSOCK(mode):
        return False, f"mode:{mode}"
    return True, f"sock:{mode}"


def call_fd(connection, interface, method, session_path):
    outcome = connection.call_with_unix_fd_list_sync(
        BUS_NAME,
        OBJECT_PATH,
        interface,
        method,
        GLib.Variant("(oa{sv})", (session_path, {})),
        GLib.VariantType.new("(h)"),
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    )
    if not isinstance(outcome, tuple) or len(outcome) != 2:
        raise RuntimeError(f"{method} dönüşü fd listesi değil: {type(outcome).__name__}")
    variant, fd_list = outcome
    index = unwrap(variant)[0]
    fd = fd_list.get(index)
    return fd


def get_property(connection, interface, name):
    value = connection.call_sync(
        BUS_NAME,
        OBJECT_PATH,
        "org.freedesktop.DBus.Properties",
        "Get",
        GLib.Variant("(ss)", (interface, name)),
        GLib.VariantType.new("(v)"),
        Gio.DBusCallFlags.NONE,
        3000,
        None,
    )
    return unwrap(value)[0]


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def show_text(blob):
    if blob is None:
        return "absent"
    if blob == b"":
        return "(empty or unavailable)"
    try:
        text = blob.decode("utf-8")
    except UnicodeDecodeError:
        return blob.hex()
    return text.replace("\r", "\\r").replace("\n", "\\n")


def mark(ok):
    return "OK" if ok else "FAIL"


def classify_pipewire_fd(fd):
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
        and pid != os.getpid()
    ):
        label = "pipewire"
    else:
        label = "other"
    return {
        "kind": label,
        "domain": domain_name,
        "type": type_name,
        "peer_path": shown,
        "peer_pid": pid,
    }


def verify_frame(node_id, width, height, measured, failures):
    if not os.path.isfile(FRAME_PPM) or not os.path.isfile(OLCUM):
        failures.append("kare kanıtı yazılmadı")
        return False
    file_size = os.path.getsize(FRAME_PPM)
    measured["frame_ppm_bytes"] = file_size
    with open(FRAME_PPM, "rb") as handle:
        magic = handle.readline()
        dims = handle.readline()
        depth = handle.readline()
        body = handle.read()
    measured["frame_ppm_header"] = (
        f"{magic.decode('ascii', 'replace').strip()} "
        f"{dims.decode('ascii', 'replace').strip()} "
        f"{depth.decode('ascii', 'replace').strip()}"
    )
    if magic != b"P6\n" or depth != b"255\n":
        failures.append(f"ppm başlığı {magic!r} {depth!r}")
        return False
    try:
        got_w, got_h = (int(part) for part in dims.split())
    except ValueError:
        failures.append(f"ppm boyutu {dims!r}")
        return False
    if (got_w, got_h) != (width, height):
        failures.append(f"ppm boyutu {got_w}x{got_h}")
        return False
    expect = got_w * got_h * 3
    accounted = len(magic) + len(dims) + len(depth) + len(body)
    if len(body) != expect or file_size != accounted:
        failures.append(f"ppm gövdesi {len(body)} beklenen {expect} dosya {file_size}")
        return False
    first = body[:3]
    flat = True
    for offset in range(0, len(body), 3):
        if body[offset:offset + 3] != first:
            flat = False
            break
    measured["frame_flat"] = flat
    if flat:
        failures.append("kare tek renk")
        return False
    file_hash = hashlib.sha256(magic + dims + depth + body).hexdigest()
    measured["frame_sha256"] = file_hash
    fields = {}
    with open(OLCUM, encoding="utf-8") as handle:
        for line in handle:
            if "=" not in line:
                continue
            key, value = line.strip().split("=", 1)
            fields[key] = value
    measured["olcum_node_id"] = fields.get("node_id", "")
    measured["olcum_format"] = fields.get("format", "")
    measured["olcum_size"] = fields.get("size", "")
    measured["olcum_sha256"] = fields.get("sha256", "")
    if fields.get("node_id") != str(int(node_id)):
        failures.append(f"ölçü node_id {fields.get('node_id')} != {node_id}")
        return False
    if fields.get("sha256") != file_hash:
        failures.append("ölçü sha256 dosyayla uyuşmuyor")
        return False
    if fields.get("format") not in ("BGRx", "RGBx", "BGRA", "RGBA"):
        failures.append(f"ölçü biçimi {fields.get('format')!r}")
        return False
    if fields.get("width") != str(width) or fields.get("height") != str(height):
        failures.append(f"ölçü boyutu {fields.get('width')}x{fields.get('height')}")
        return False
    if fields.get("size") != f"{width}x{height}":
        failures.append(f"ölçü size {fields.get('size')!r}")
        return False
    return True


def run_frame_consumer(fd, node_id, width, height, measured, failures):
    os.makedirs(os.path.dirname(FRAME_PPM), exist_ok=True)
    for path in (FRAME_PPM, OLCUM):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    if not os.path.isfile(FRAME):
        failures.append("frame-al yok")
        return False
    try:
        proc = subprocess.run(
            [FRAME, str(fd), str(int(node_id)), str(int(width)), str(int(height)), FRAME_PPM, OLCUM],
            pass_fds=(fd,),
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        failures.append(f"kare tüketicisi zaman aşımı: {exc}")
        return False
    except OSError as exc:
        failures.append(f"kare tüketicisi: {exc}")
        return False
    measured["frame_consumer_exit"] = proc.returncode
    err = (proc.stderr or "").strip().replace("\n", " | ")
    out = (proc.stdout or "").strip().replace("\n", " | ")
    if err:
        measured["frame_consumer_stderr"] = err
    if out:
        measured["frame_consumer_stdout"] = out
    if proc.returncode != 0:
        failures.append(f"kare tüketicisi çıkış {proc.returncode}: {err or out}")
        return False
    return verify_frame(node_id, width, height, measured, failures)


def olay_proof_ok(failures):
    if not os.path.isfile(OLAY_EVIDENCE):
        failures.append("olay kanıtı yok")
        return False
    with open(OLAY_EVIDENCE, encoding="utf-8") as handle:
        text = handle.read()
    required = (
        "\nolay_geldi=1\n",
        "\nkayit_motion_absolute=1\n",
        "\nkayit_motion=x=1280 y=720 x_extent=2560 y_extent=1440\n",
        "\nkayit_key=1\n",
        "\nwl_giris_istegi=0\n",
        "\ncanli_kapi=kapali\n",
        "\noverall=pass\n",
    )
    missing = [item.strip() for item in required if item not in f"\n{text}"]
    if missing or "\nkayit_key_ayrinti=evdev=" not in f"\n{text}" or " state=1\n" not in text:
        failures.append("olay kanıtı geçmedi: " + text.replace("\n", " | "))
        return False
    return True


def run_eis_probe(eis_fd):
    if not os.path.isfile(EIS_PROBE):
        write_text(EIS_EVIDENCE, "source=ConnectToEIS\noverall=fail\nreason=eis-probe yok\n")
        return False
    try:
        proc = subprocess.run(
            [EIS_PROBE, str(eis_fd), EIS_EVIDENCE, OLAY_EVIDENCE],
            pass_fds=(eis_fd,),
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        write_text(EIS_EVIDENCE, f"source=ConnectToEIS\noverall=fail\nreason={exc}\n")
        return False
    if not os.path.isfile(EIS_EVIDENCE):
        err = (proc.stderr or "").strip()
        write_text(
            EIS_EVIDENCE,
            f"source=ConnectToEIS\noverall=fail\nhelper_exit={proc.returncode}\nhelper_stderr={err}\n",
        )
        return False
    with open(EIS_EVIDENCE, encoding="utf-8") as handle:
        text = handle.read()
    return (
        proc.returncode == 0
        and "\noverall=pass\n" in f"\n{text}"
        and "\nseat_added=yes\n" in f"\n{text}"
        and "\nkeyboard=yes\n" in f"\n{text}"
        and "\nabsolute_pointer=yes\n" in f"\n{text}"
    )


def clip_get(path):
    proc = subprocess.run(
        [CLIP_IO, "get", path],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "clip-io get başarısız").strip()
        raise RuntimeError(detail)
    line = proc.stdout.strip()
    if line == "empty=1":
        return b""
    if not line.startswith("bytes="):
        raise RuntimeError(f"clip-io get çıktısı {line!r}")
    expect = int(line.split("=", 1)[1])
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) != expect:
        raise RuntimeError(f"pano boyutu {len(data)} != {expect}")
    return data


def wait_clipboard(path, expected, attempts=8):
    last = None
    last_error = None
    for _ in range(attempts):
        try:
            last = clip_get(path)
            last_error = None
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            last = None
            last_error = exc
        if last == expected:
            return last, None
        time.sleep(0.25)
    return last, last_error


def stop_hold(proc):
    if proc is None or proc.poll() is not None:
        return
    try:
        if proc.stdin is not None:
            proc.stdin.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)


def start_hold(path, data):
    with open(path, "wb") as handle:
        handle.write(data)
    proc = subprocess.Popen(
        [CLIP_IO, "hold", path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )
    ready, _, _ = select.select([proc.stdout], [], [], 10)
    if not ready:
        stop_hold(proc)
        raise RuntimeError("pano tutucusu hazır bildirmedi")
    line = proc.stdout.readline().strip()
    if line != b"ready":
        err = b""
        if proc.stderr is not None:
            err = proc.stderr.read()
        stop_hold(proc)
        raise RuntimeError(f"pano tutucusu {line!r} {err!r}")
    return proc


def persist_clipboard(original):
    if original == b"":
        proc = subprocess.run(
            [CLIP_IO, "clear"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or "clear başarısız").strip())
        return
    subprocess.run(
        ["wl-copy", "--type", "text/plain"],
        input=original,
        timeout=10,
        check=True,
        start_new_session=True,
    )


def call_out_fd(connection, method, parameters):
    outcome = connection.call_with_unix_fd_list_sync(
        BUS_NAME,
        OBJECT_PATH,
        CLIPBOARD,
        method,
        parameters,
        GLib.VariantType.new("(h)"),
        Gio.DBusCallFlags.NONE,
        8000,
        None,
    )
    if not isinstance(outcome, tuple) or len(outcome) != 2:
        raise RuntimeError(f"{method} dönüşü fd listesi değil")
    variant, fd_list = outcome
    index = unwrap(variant)[0]
    return fd_list.get(index)


def read_until_eof(fd):
    chunks = []
    while True:
        blob = os.read(fd, 65536)
        if not blob:
            break
        chunks.append(blob)
    return b"".join(chunks)


def listen_owner(connection, session_path):
    box = {"options": None, "loop": GLib.MainLoop(), "sub": 0}

    def on_signal(conn, sender, object_path, iface, signal_name, params):
        handle, options = unwrap(params)
        if handle != session_path:
            return
        box["options"] = options
        if box["loop"].is_running():
            box["loop"].quit()

    box["sub"] = connection.signal_subscribe(
        BUS_NAME,
        CLIPBOARD,
        "SelectionOwnerChanged",
        None,
        None,
        Gio.DBusSignalFlags.NONE,
        on_signal,
    )
    return box


def wait_owner(box, timeout_s=8):
    if box["options"] is None:
        GLib.timeout_add(int(timeout_s * 1000), lambda: box["loop"].quit() or False)
        box["loop"].run()
    return box["options"]


def exercise_clipboard(connection, session_path, work):
    """Kullanıcının text/plain panosunu saklar, dener, aynen geri yükler."""
    original = None
    payload = PAYLOAD
    mutated = False
    hold = None
    lines = []
    checks = []
    owner_text = "absent"
    selection_read = None
    wayland_back = None
    restored = None

    def finish(ok):
        lines.append(f"original text/plain: {show_text(original)}")
        if selection_read is not None:
            lines.append(f"selection read: {show_text(selection_read)}")
        lines.append(f"roundtrip test payload: {show_text(payload)}")
        lines.append(f"roundtrip match: {mark(selection_read == payload)}")
        lines.append(f"wayland readback match: {mark(wayland_back == PAYLOAD)}")
        lines.append(f"verify restore match: {mark(original is not None and restored == original)}")
        lines.append(f"session_is_owner: {owner_text}")
        if original is not None:
            lines.append(f"original_sha256: {hashlib.sha256(original).hexdigest()}")
        if restored is not None:
            lines.append(f"restore_sha256: {hashlib.sha256(restored).hexdigest()}")
        if checks:
            lines.append("notes:")
            for item in checks:
                lines.append(f"- {item}")
        lines.append(f"overall={'pass' if ok else 'fail'}")
        write_text(PANO_EVIDENCE, "\n".join(lines) + "\n")
        return ok

    try:
        if not os.path.isfile(CLIP_IO):
            checks.append("clip-io yok")
            return finish(False)
        original_path = os.path.join(work, "original.bin")
        current_path = os.path.join(work, "current.bin")
        hold_path = os.path.join(work, "hold.bin")
        original = clip_get(original_path)
        if original == payload:
            payload = PAYLOAD + b"-2"

        seen = {}

        def on_transfer(conn, sender, object_path, iface, signal_name, params):
            handle, mime, serial = unwrap(params)
            if handle == session_path and mime == "text/plain":
                seen["serial"] = serial
                seen["mime"] = mime

        transfer_sub = connection.signal_subscribe(
            BUS_NAME,
            CLIPBOARD,
            "SelectionTransfer",
            None,
            None,
            Gio.DBusSignalFlags.NONE,
            on_transfer,
        )
        try:
            connection.call_sync(
                BUS_NAME,
                OBJECT_PATH,
                CLIPBOARD,
                "SetSelection",
                GLib.Variant("(oa{sv})", (session_path, {
                    "mime_types": GLib.Variant("as", ["text/plain"]),
                })),
                GLib.VariantType.new("()"),
                Gio.DBusCallFlags.NONE,
                5000,
                None,
            )
            if "serial" not in seen:
                pump(1000)
            if "serial" not in seen:
                deadline = time.monotonic() + 3
                while "serial" not in seen and time.monotonic() < deadline:
                    pump(100)
        finally:
            connection.signal_unsubscribe(transfer_sub)
        if "serial" not in seen:
            checks.append("SelectionTransfer gelmedi")
            return finish(False)

        mutated = True
        write_fd = call_out_fd(
            connection,
            "SelectionWrite",
            GLib.Variant("(ou)", (session_path, int(seen["serial"]))),
        )
        try:
            remaining = payload
            while remaining:
                wrote = os.write(write_fd, remaining)
                if wrote <= 0:
                    break
                remaining = remaining[wrote:]
        finally:
            os.close(write_fd)
        connection.call_sync(
            BUS_NAME,
            OBJECT_PATH,
            CLIPBOARD,
            "SelectionWriteDone",
            GLib.Variant("(oub)", (session_path, int(seen["serial"]), True)),
            GLib.VariantType.new("()"),
            Gio.DBusCallFlags.NONE,
            8000,
            None,
        )

        read_fd = call_out_fd(
            connection,
            "SelectionRead",
            GLib.Variant("(os)", (session_path, "text/plain")),
        )
        try:
            selection_read = read_until_eof(read_fd)
        finally:
            os.close(read_fd)
        if selection_read != payload:
            checks.append("SelectionRead deneme dizgisini vermedi")

        wayland_back, wayland_error = wait_clipboard(current_path, payload)
        if wayland_error is not None:
            checks.append(f"wayland okuma: {wayland_error}")
        elif wayland_back != payload:
            checks.append(f"wayland okuma {show_text(wayland_back)!r}")

        # Portal, sahip olduğunu kendi okumasıyla görsün; sonra yerel değişiklik gelsin.
        time.sleep(0.6)
        owner_box = listen_owner(connection, session_path)
        try:
            if original == b"":
                clear_proc = subprocess.run(
                    [CLIP_IO, "clear"],
                    capture_output=True,
                    text=True,
                    timeout=15,
                    check=False,
                )
                if clear_proc.returncode != 0:
                    checks.append((clear_proc.stderr or "clear başarısız").strip())
            else:
                hold = start_hold(hold_path, original)
            options = wait_owner(owner_box)
        finally:
            if owner_box["sub"]:
                connection.signal_unsubscribe(owner_box["sub"])
        if isinstance(options, dict) and options.get("session_is_owner") is False:
            owner_text = "false"
        elif isinstance(options, dict) and "session_is_owner" in options:
            owner_text = repr(options.get("session_is_owner"))
            checks.append(f"session_is_owner {owner_text}")
        else:
            checks.append("SelectionOwnerChanged session_is_owner=false gelmedi")

        if original != b"":
            persist_clipboard(original)
        stop_hold(hold)
        hold = None
        restored, restore_error = wait_clipboard(current_path, original)
        if restore_error is not None:
            checks.append(f"geri yükleme okuması: {restore_error}")
        if restored != original:
            checks.append("geri yüklenen içerik özgün içerikten farklı")
        else:
            mutated = False

        ok = (
            selection_read == payload
            and wayland_back == payload
            and restored == original
            and owner_text == "false"
        )
        return finish(ok)
    except Exception as exc:
        checks.append(f"{type(exc).__name__}: {exc}")
        if mutated:
            try:
                if original == b"":
                    subprocess.run([CLIP_IO, "clear"], timeout=15, check=False)
                elif original is not None:
                    persist_clipboard(original)
                stop_hold(hold)
                hold = None
                restored, restore_error = wait_clipboard(os.path.join(work, "restore.bin"), original)
                if restore_error is not None:
                    checks.append(f"acil geri yükleme: {restore_error}")
                if restored == original:
                    mutated = False
            except Exception as restore_exc:
                checks.append(f"geri yükleme hatası: {restore_exc}")
        return finish(False)
    finally:
        stop_hold(hold)


def write_evidence(measured, failures):
    os.makedirs(os.path.dirname(EVIDENCE), exist_ok=True)
    lines = ["=== crd-portal probe ==="]
    for key in sorted(measured):
        lines.append(f"{key}={measured[key]}")
    if failures:
        lines.append("failures:")
        for item in failures:
            lines.append(f"- {item}")
    else:
        lines.append("failures=none")
    text = "\n".join(lines) + "\n"
    with open(EVIDENCE, "w", encoding="utf-8") as handle:
        handle.write(text)


def main():
    failures = []
    measured = {}
    daemon = None
    portal = None
    work = None
    connection = None

    if not os.path.isfile(BINARY):
        failures.append(f"ikili yok: {BINARY}")
        write_evidence(measured, failures)
        return 1

    try:
        refuse_real_bus(measured, failures)

        work = tempfile.mkdtemp(prefix="crd-portal-probe-")
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

        try:
            os.remove(OLAY_EVIDENCE)
        except FileNotFoundError:
            pass
        portal_env = os.environ.copy()
        portal_env["CRD_OLAY_KANIT"] = OLAY_EVIDENCE
        portal = subprocess.Popen(
            [BINARY, address],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=portal_env,
        )
        flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        wait_for_name(connection, failures)
        if failures and any("ad alınamadı" in item for item in failures):
            return 1

        measured["remote_desktop_version"] = get_property(connection, REMOTE, "version")
        measured["available_device_types"] = get_property(connection, REMOTE, "AvailableDeviceTypes")
        measured["screencast_version"] = get_property(connection, SCREEN, "version")
        measured["available_source_types"] = get_property(connection, SCREEN, "AvailableSourceTypes")
        measured["available_cursor_modes"] = get_property(connection, SCREEN, "AvailableCursorModes")
        measured["clipboard_version"] = get_property(connection, CLIPBOARD, "version")
        if measured["remote_desktop_version"] != EXPECT_RD_VERSION:
            failures.append(f"RemoteDesktop.version {measured['remote_desktop_version']}")
        if (int(measured["available_device_types"]) & 3) != 3:
            failures.append(f"AvailableDeviceTypes {measured['available_device_types']}")
        if measured["screencast_version"] != EXPECT_SC_VERSION:
            failures.append(f"ScreenCast.version {measured['screencast_version']}")
        if (int(measured["available_source_types"]) & 1) != 1:
            failures.append(f"AvailableSourceTypes {measured['available_source_types']}")
        if (int(measured["available_cursor_modes"]) & 4) != 4:
            failures.append(f"AvailableCursorModes {measured['available_cursor_modes']}")
        if measured["clipboard_version"] != EXPECT_CLIP_VERSION:
            failures.append(f"Clipboard.version {measured['clipboard_version']}")

        unique = connection.get_unique_name()
        session_token = "probe_session"
        session_path = handle_path("session", unique, session_token)
        measured["session_path"] = session_path

        created = call_request(
            connection,
            REMOTE,
            "CreateSession",
            GLib.Variant("(a{sv})", (dict_variant({
                "handle_token": ("s", "probe_create"),
                "session_handle_token": ("s", session_token),
            }),)),
            "probe_create",
            failures,
            "CreateSession",
        )
        if created is None:
            return 1
        measured["response_create"] = created["code"]
        session_handle = created["results"].get("session_handle")
        measured["session_handle"] = session_handle
        if session_handle != session_path:
            failures.append(f"session_handle {session_handle!r} != {session_path}")

        devices = call_request(
            connection,
            REMOTE,
            "SelectDevices",
            GLib.Variant("(oa{sv})", (session_path, dict_variant({
                "handle_token": ("s", "probe_devices"),
                "types": ("u", 3),
            }))),
            "probe_devices",
            failures,
            "SelectDevices",
        )
        if devices is None:
            return 1
        measured["response_select_devices"] = devices["code"]

        stray = []

        def on_stray(*args):
            stray.append(args[-2] if len(args) >= 2 else args)

        stray_sub = connection.signal_subscribe(
            BUS_NAME, REQUEST, "Response", None, None, Gio.DBusSignalFlags.NONE, on_stray,
        )
        clipboard_reply = connection.call_sync(
            BUS_NAME,
            OBJECT_PATH,
            CLIPBOARD,
            "RequestClipboard",
            GLib.Variant("(oa{sv})", (session_path, {})),
            GLib.VariantType.new("()"),
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        )
        pump(200)
        connection.signal_unsubscribe(stray_sub)
        measured["clipboard_reply"] = repr(unwrap(clipboard_reply))
        measured["clipboard_response_signals"] = len(stray)
        if len(stray) != 0:
            failures.append(f"RequestClipboard Response sinyali verdi: {len(stray)}")

        sources = call_request(
            connection,
            SCREEN,
            "SelectSources",
            GLib.Variant("(oa{sv})", (session_path, dict_variant({
                "handle_token": ("s", "probe_sources"),
                "multiple": ("b", True),
                "types": ("u", 1),
                "cursor_mode": ("u", 4),
            }))),
            "probe_sources",
            failures,
            "SelectSources",
        )
        if sources is None:
            return 1
        measured["response_select_sources"] = sources["code"]

        started = call_request(
            connection,
            REMOTE,
            "Start",
            GLib.Variant("(osa{sv})", (session_path, "", dict_variant({
                "handle_token": ("s", "probe_start"),
            }))),
            "probe_start",
            failures,
            "Start",
        )
        if started is None:
            return 1
        measured["response_start"] = started["code"]
        results = started["results"]
        measured["clipboard_enabled"] = results.get("clipboard_enabled")
        streams = results.get("streams")
        measured["stream_count"] = 0 if streams is None else len(streams)
        if results.get("clipboard_enabled") is not True:
            failures.append(f"clipboard_enabled {results.get('clipboard_enabled')!r}")
        if not streams:
            failures.append("streams boş")
        else:
            node_id, props = streams[0]
            position = tuple(props.get("position", ()))
            size = tuple(props.get("size", ()))
            measured["stream_node"] = node_id
            measured["mapping_id"] = props.get("mapping_id")
            measured["position"] = ",".join(str(item) for item in position)
            measured["size"] = ",".join(str(item) for item in size)
            if props.get("mapping_id") != "HDMI-A-1":
                failures.append(f"mapping_id {props.get('mapping_id')!r}")
            if position != (0, 0):
                failures.append(f"position {position}")
            if size != (2560, 1440):
                failures.append(f"size {size}")

        pipewire_fd = call_fd(connection, SCREEN, "OpenPipeWireRemote", session_path)
        eis_fd = call_fd(connection, REMOTE, "ConnectToEIS", session_path)
        pipewire_ok, pipewire_detail = fd_is_socket(pipewire_fd)
        eis_ok, eis_detail = fd_is_socket(eis_fd)
        measured["pipewire_fd"] = pipewire_fd
        measured["pipewire_fd_valid"] = pipewire_detail
        measured["eis_fd"] = eis_fd
        measured["eis_fd_valid"] = eis_detail
        if not pipewire_ok:
            failures.append(f"OpenPipeWireRemote fd geçersiz: {pipewire_detail}")
        else:
            try:
                facts = classify_pipewire_fd(pipewire_fd)
            except OSError as exc:
                facts = None
                failures.append(f"OpenPipeWireRemote ucu sınıflanamadı: {exc}")
            if facts is not None:
                measured["pipewire_fd_kind"] = facts["kind"]
                measured["pipewire_fd_domain"] = facts["domain"]
                measured["pipewire_fd_type"] = facts["type"]
                measured["pipewire_peer_path"] = facts["peer_path"]
                measured["pipewire_peer_pid"] = facts["peer_pid"]
                if facts["kind"] == "socketpair":
                    failures.append("OpenPipeWireRemote socketpair döndürdü")
                elif facts["kind"] != "pipewire":
                    failures.append(f"OpenPipeWireRemote pipewire soketi değil: {facts['peer_path']}")
                elif measured.get("size") == "2560,1440" and int(measured.get("stream_node", 0)) > 0:
                    captured = run_frame_consumer(
                        pipewire_fd,
                        measured["stream_node"],
                        2560,
                        1440,
                        measured,
                        failures,
                    )
                    measured["stream_node_kind"] = "capture" if captured else "unverified"
                else:
                    measured["stream_node_kind"] = "unverified"
                    failures.append("akış boyutu kare alınmadan kaldı")
        if "stream_node" in measured and measured.get("stream_node_kind") != "capture":
            if "stream_node_kind" not in measured:
                measured["stream_node_kind"] = "unverified"
            if not any("kare" in item or "socketpair" in item or "pipewire" in item or "akış" in item for item in failures):
                failures.append(f"akış düğümü doğrulanmadı: {measured.get('stream_node_kind')}")
        if not eis_ok:
            failures.append(f"ConnectToEIS fd geçersiz: {eis_detail}")
        elif not run_eis_probe(eis_fd):
            failures.append("ConnectToEIS kanıtı geçmedi")
        elif not olay_proof_ok(failures):
            pass
        os.close(pipewire_fd)
        os.close(eis_fd)

        if not failures:
            if not exercise_clipboard(connection, session_path, work):
                failures.append("pano kanıtı geçmedi")

        try:
            connection.call_sync(
                BUS_NAME,
                OBJECT_PATH,
                "org.freedesktop.host.portal.Registry",
                "Register",
                GLib.Variant("(sa{sv})", ("com.google.ChromeRemoteDesktop", {})),
                None,
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            )
            measured["registry"] = "accepted"
            failures.append("Registry.Register kabul edildi")
        except GLib.Error as exc:
            measured["registry"] = exc.message

        closed = {}
        closed_loop = GLib.MainLoop()

        def on_closed(conn, sender, object_path, iface, signal_name, params):
            closed["details"] = unwrap(params)[0]
            closed["path"] = object_path
            if closed_loop.is_running():
                closed_loop.quit()

        closed_sub = connection.signal_subscribe(
            BUS_NAME, SESSION, "Closed", session_path, None, Gio.DBusSignalFlags.NONE, on_closed,
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
            if "details" not in closed:
                GLib.timeout_add(3000, lambda: closed_loop.quit() or False)
                closed_loop.run()
        finally:
            connection.signal_unsubscribe(closed_sub)
        measured["session_closed_path"] = closed.get("path", "absent")
        measured["session_closed_details"] = repr(closed.get("details", "absent"))
        if closed.get("path") != session_path:
            failures.append(f"Session.Closed yolu {closed.get('path')}")
        if not isinstance(closed.get("details"), dict):
            failures.append(f"Session.Closed ayrıntısı {closed.get('details')!r}")

        return 0 if not failures else 1
    except Exception as exc:
        failures.append(f"{type(exc).__name__}: {exc}")
        return 1
    finally:
        if connection is not None:
            try:
                connection.close_sync(None)
            except GLib.Error:
                pass
        stop_process(portal)
        stop_process(daemon)
        if portal is not None:
            stderr = portal.stderr.read() if portal.stderr is not None else ""
            measured["portal_stderr"] = stderr.strip().replace("\n", " | ")
            stderr_text = measured["portal_stderr"]
            if "socketpair" in stderr_text or "placeholder" in stderr_text:
                failures.append("portal yer tutucu izi bıraktı")
        if daemon is not None and daemon.stderr is not None:
            daemon_err = daemon.stderr.read().strip()
            if daemon_err:
                measured["daemon_stderr"] = daemon_err.replace("\n", " | ")
        if work is not None:
            shutil.rmtree(work, ignore_errors=True)
        write_evidence(measured, failures)


if __name__ == "__main__":
    sys.exit(main())
