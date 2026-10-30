#!/usr/bin/env bash
# NetworkMonitor kanıtı. Host başlatılmaz. portal.c bağlanmaz.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/crd-netmon"
CONF="${SCRIPT_DIR}/bus.conf"
KANIT_DIR="${SCRIPT_DIR}/kanit"
KANIT="${KANIT_DIR}/netmon.txt"
NAME="org.freedesktop.portal.Desktop"
PATH_OBJ="/org/freedesktop/portal/desktop"
IFACE="org.freedesktop.portal.NetworkMonitor"
REAL_BUS="unix:path=/run/user/1000/bus"

tmp="$(mktemp -d /tmp/crd-netmon-olc.XXXXXX)"
sock="${tmp}/bus"
netmon_pid=""
dbus_pid=""
evidence_written=0
failures=()

stop_pid() {
  local pid="${1:-}"
  local _
  if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
    return 0
  fi
  kill -TERM "${pid}" 2>/dev/null || true
  for _ in $(seq 1 50); do
    kill -0 "${pid}" 2>/dev/null || return 0
    sleep 0.1
  done
  kill -KILL "${pid}" 2>/dev/null || true
}

cleanup() {
  stop_pid "${netmon_pid}"
  stop_pid "${dbus_pid}"
  if [[ "${evidence_written}" -eq 0 ]]; then
    mkdir -p "${KANIT_DIR}"
    {
      echo "Bu parça portal.c'ye bağlanmaz."
      echo "Host çalıştırılmadı."
      echo "kanıt tamamlanamadı"
      if [[ ${#failures[@]} -gt 0 ]]; then
        printf 'fail=%s\n' "${failures[@]}"
      fi
    } >"${KANIT}"
  fi
  rm -rf "${tmp}"
  rm -f /tmp/crd-netmon-placeholder
}
trap cleanup EXIT

fail() {
  failures+=("$1")
  echo "FAIL: $1" >&2
}

real_owner() {
  busctl --user call org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus GetNameOwner s "${NAME}" 2>"${tmp}/owner.err" || true
}

real_status_field() {
  local key="$1"
  busctl --user status "${NAME}" 2>/dev/null | awk -F= -v k="${key}" '$1==k {print substr($0, length(k)+2); exit}'
}

call_private() {
  busctl --address="${address}" call "${NAME}" "${PATH_OBJ}" "$@"
}

mkdir -p "${KANIT_DIR}"

if [[ ! -x "${BIN}" ]]; then
  fail "crd-netmon yok"
fi
if [[ ! -S /run/user/1000/bus ]]; then
  fail "gerçek oturum soketi görünmüyor"
fi

owner_before="$(real_owner)"
pid_before="$(real_status_field PID)"
cmd_before="$(real_status_field CommandLine)"
if [[ -z "${owner_before}" || -z "${pid_before}" ]]; then
  fail "gerçek sahip okunamadı: ${owner_before}"
fi

address=""
if [[ -x "${BIN}" && -S /run/user/1000/bus ]]; then
  dbus-daemon --config-file="${CONF}" \
    --address="unix:path=${sock}" \
    --nofork --nopidfile --print-address \
    >"${tmp}/dbus.out" 2>"${tmp}/dbus.err" &
  dbus_pid=$!
  for _ in $(seq 1 50); do
    if [[ -s "${tmp}/dbus.out" ]]; then
      address="$(head -n 1 "${tmp}/dbus.out")"
      break
    fi
    if ! kill -0 "${dbus_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.05
  done
  if [[ -z "${address}" ]]; then
    fail "özel veriyolu açılmadı: $(cat "${tmp}/dbus.err" 2>/dev/null)"
  fi
  if [[ -e /tmp/crd-netmon-placeholder ]]; then
    fail "yer tutucu soket oluştu"
  fi
fi

netmon_exit=""
acquired=0
stderr_text=""
stdout_text=""
if [[ -n "${address}" ]]; then
  "${BIN}" "${address}" >"${tmp}/netmon.out" 2>"${tmp}/netmon.err" &
  netmon_pid=$!
  for _ in $(seq 1 50); do
    if grep -q "ad alındı:" "${tmp}/netmon.err" 2>/dev/null; then
      break
    fi
    if ! kill -0 "${netmon_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  stderr_text="$(cat "${tmp}/netmon.err" 2>/dev/null || true)"
  stdout_text="$(cat "${tmp}/netmon.out" 2>/dev/null || true)"
  if grep -q "ad alındı: ${NAME}" <<<"${stderr_text}"; then
    acquired=1
  else
    fail "ad alınamadı: ${stderr_text}"
  fi
fi

monitor_type="$(sed -n 's/^ağ kaynağı: //p' <<<"${stderr_text}" | head -n 1)"
if [[ "${acquired}" -eq 1 ]]; then
  case "${monitor_type}" in
    GNetworkMonitorNM|GNetworkMonitorNetlink) ;;
    *) fail "ağ kaynağı gerçek değil: ${monitor_type:-yok}" ;;
  esac
fi

status_raw=""
metered_raw=""
available_raw=""
connectivity_raw=""
version_prop=""
version_get=""
introspect=""
canreach_raw=""
canreach_rc=0
empty_host_rc=0
empty_host_err=""
bad_port_rc=0
bad_port_err=""
private_pid=""
owner_during=""

if grep -q "ad alındı:" <<<"${stderr_text}"; then
  status_raw="$(call_private "${IFACE}" GetStatus 2>"${tmp}/status.err" || true)"
  metered_raw="$(call_private "${IFACE}" GetMetered 2>"${tmp}/metered.err" || true)"
  available_raw="$(call_private "${IFACE}" GetAvailable 2>"${tmp}/available.err" || true)"
  connectivity_raw="$(call_private "${IFACE}" GetConnectivity 2>"${tmp}/connectivity.err" || true)"
  version_prop="$(busctl --address="${address}" get-property "${NAME}" "${PATH_OBJ}" "${IFACE}" version 2>"${tmp}/version.err" || true)"
  version_get="$(call_private org.freedesktop.DBus.Properties Get ss "${IFACE}" version 2>"${tmp}/versionget.err" || true)"
  introspect="$(busctl --address="${address}" introspect "${NAME}" "${PATH_OBJ}" "${IFACE}" 2>"${tmp}/intro.err" || true)"
  set +e
  canreach_raw="$(timeout 15 busctl --address="${address}" call "${NAME}" "${PATH_OBJ}" "${IFACE}" CanReach su example.com 443 2>"${tmp}/canreach.err")"
  canreach_rc=$?
  timeout 10 busctl --address="${address}" call "${NAME}" "${PATH_OBJ}" "${IFACE}" CanReach su "" 80 >"${tmp}/empty.out" 2>"${tmp}/empty.err"
  empty_host_rc=$?
  timeout 10 busctl --address="${address}" call "${NAME}" "${PATH_OBJ}" "${IFACE}" CanReach su 127.0.0.1 70000 >"${tmp}/port.out" 2>"${tmp}/port.err"
  bad_port_rc=$?
  set -e
  empty_host_err="$(cat "${tmp}/empty.err" 2>/dev/null || true)"
  bad_port_err="$(cat "${tmp}/port.err" 2>/dev/null || true)"
  private_pid="$(busctl --address="${address}" status "${NAME}" 2>/dev/null | awk -F= '$1=="PID"{print $2; exit}')"
  owner_during="$(real_owner)"
fi

python3 - "${tmp}/status_fields" "${status_raw}" <<'PY' >"${tmp}/parsed.env"
import re, sys
out, text = sys.argv[1], sys.argv[2].strip()
match = re.fullmatch(r"a\{sv\} (\d+) (.*)", text)
pairs = []
if match:
    pairs = re.findall(r'"([^"]+)" ([A-Za-z]+) (\S+)', match.group(2))
    if int(match.group(1)) != len(pairs):
        pairs = []
wanted = {"available": "b", "metered": "b", "connectivity": "u"}
ok = len(pairs) == 3 and {k: t for k, t, _ in pairs} == wanted
values = {k: v for k, _, v in pairs}
with open(out, "w", encoding="utf-8") as fh:
    fh.write("parse_ok=%s\n" % ("yes" if ok else "no"))
    for key in ("available", "metered", "connectivity"):
        fh.write("%s=%s\n" % (key, values.get(key, "")))
PY
# python yazdı; kabuk değişkenleri aşağıda okunur
parse_ok="$(sed -n 's/^parse_ok=//p' "${tmp}/status_fields")"
st_available="$(sed -n 's/^available=//p' "${tmp}/status_fields")"
st_metered="$(sed -n 's/^metered=//p' "${tmp}/status_fields")"
st_connectivity="$(sed -n 's/^connectivity=//p' "${tmp}/status_fields")"

changed_line="$(awk '$1==".changed" && $2=="signal" {print; exit}' <<<"${introspect}")"
if [[ "${acquired}" -eq 1 ]]; then
  if [[ "${parse_ok}" != "yes" ]]; then
    fail "GetStatus ayrışmadı: ${status_raw}"
  fi
  if [[ "${metered_raw}" != "b ${st_metered}" ]]; then
    fail "GetMetered GetStatus ile uyuşmuyor: ${metered_raw}"
  fi
  if [[ "${available_raw}" != "b ${st_available}" ]]; then
    fail "GetAvailable GetStatus ile uyuşmuyor: ${available_raw}"
  fi
  if [[ "${connectivity_raw}" != "u ${st_connectivity}" ]]; then
    fail "GetConnectivity GetStatus ile uyuşmuyor: ${connectivity_raw}"
  fi
  if [[ "${version_prop}" != "u 3" || "${version_get}" != "v u 3" ]]; then
    fail "version=3 değil: prop=${version_prop} get=${version_get}"
  fi
  if [[ -z "${changed_line}" ]]; then
    fail "changed sinyali arayüzde yok"
  fi
  if [[ "${canreach_rc}" -ne 0 || "${canreach_raw}" != b\ * ]]; then
    fail "CanReach boolean dönmedi rc=${canreach_rc} raw=${canreach_raw}"
  fi
  if [[ "${empty_host_rc}" -eq 0 ]]; then
    fail "boş hostname kabul edildi"
  fi
  if [[ "${bad_port_rc}" -eq 0 ]]; then
    fail "70000 kapısı kabul edildi"
  fi
fi
if [[ -n "${stdout_text}" ]]; then
  fail "stdout boş değil"
fi
if [[ -n "${netmon_pid}" && "${private_pid}" != "${netmon_pid}" ]]; then
  fail "özel veriyolu sahibi pid=${private_pid} süreç=${netmon_pid}"
fi
if [[ -n "${owner_during}" && "${owner_during}" != "${owner_before}" ]]; then
  fail "çalışırken gerçek sahip değişti: ${owner_before} -> ${owner_during}"
fi

gio_type=""
gio_available=""
gio_metered=""
gio_connectivity=""
gio_text="$(python3 - <<'PY'
from gi.repository import Gio
monitor = Gio.NetworkMonitor.get_default()
print(monitor.__gtype__.name)
print("true" if monitor.get_network_available() else "false")
print("true" if monitor.get_network_metered() else "false")
print(int(monitor.get_connectivity()))
PY
)" || fail "GNetworkMonitor okunamadı"
gio_type="$(sed -n '1p' <<<"${gio_text}")"
gio_available="$(sed -n '2p' <<<"${gio_text}")"
gio_metered="$(sed -n '3p' <<<"${gio_text}")"
gio_connectivity="$(sed -n '4p' <<<"${gio_text}")"
if [[ "${parse_ok}" == "yes" ]]; then
  if [[ "${st_available}" != "${gio_available}" || "${st_metered}" != "${gio_metered}" || "${st_connectivity}" != "${gio_connectivity}" ]]; then
    sleep 0.4
    status_raw="$(call_private "${IFACE}" GetStatus 2>>"${tmp}/status.err" || true)"
    python3 - "${tmp}/status_fields" "${status_raw}" <<'PY'
import re, sys
out, text = sys.argv[1], sys.argv[2].strip()
match = re.fullmatch(r"a\{sv\} (\d+) (.*)", text)
pairs = re.findall(r'"([^"]+)" ([A-Za-z]+) (\S+)', match.group(2)) if match else []
if not match or int(match.group(1)) != len(pairs):
    pairs = []
wanted = {"available": "b", "metered": "b", "connectivity": "u"}
ok = len(pairs) == 3 and {k: t for k, t, _ in pairs} == wanted
values = {k: v for k, _, v in pairs}
with open(out, "w", encoding="utf-8") as fh:
    fh.write("parse_ok=%s\n" % ("yes" if ok else "no"))
    for key in ("available", "metered", "connectivity"):
        fh.write("%s=%s\n" % (key, values.get(key, "")))
PY
    parse_ok="$(sed -n 's/^parse_ok=//p' "${tmp}/status_fields")"
    st_available="$(sed -n 's/^available=//p' "${tmp}/status_fields")"
    st_metered="$(sed -n 's/^metered=//p' "${tmp}/status_fields")"
    st_connectivity="$(sed -n 's/^connectivity=//p' "${tmp}/status_fields")"
    metered_raw="$(call_private "${IFACE}" GetMetered 2>>"${tmp}/metered.err" || true)"
    gio_text="$(python3 - <<'PY'
from gi.repository import Gio
monitor = Gio.NetworkMonitor.get_default()
print(monitor.__gtype__.name)
print("true" if monitor.get_network_available() else "false")
print("true" if monitor.get_network_metered() else "false")
print(int(monitor.get_connectivity()))
PY
)"
    gio_type="$(sed -n '1p' <<<"${gio_text}")"
    gio_available="$(sed -n '2p' <<<"${gio_text}")"
    gio_metered="$(sed -n '3p' <<<"${gio_text}")"
    gio_connectivity="$(sed -n '4p' <<<"${gio_text}")"
  fi
  if [[ "${st_available}" != "${gio_available}" || "${st_metered}" != "${gio_metered}" || "${st_connectivity}" != "${gio_connectivity}" || "${monitor_type}" != "${gio_type}" ]]; then
    fail "GetStatus GNetworkMonitor ile uyuşmuyor portal=${st_available}/${st_metered}/${st_connectivity} gio=${gio_available}/${gio_metered}/${gio_connectivity} tip=${monitor_type}/${gio_type}"
  fi
fi

if [[ -n "${netmon_pid}" ]]; then
  kill -TERM "${netmon_pid}" 2>/dev/null || true
  set +e
  wait "${netmon_pid}"
  netmon_exit=$?
  set -e
  netmon_pid=""
fi
if [[ "${acquired}" -eq 1 && "${netmon_exit}" != "0" ]]; then
  fail "crd-netmon kapanış kodu ${netmon_exit:-yok}"
fi
stop_pid "${dbus_pid}"
dbus_pid=""
if [[ -S "${sock}" ]]; then
  fail "özel soket durduktan sonra duruyor"
fi

refuse_one() {
  local label="$1"
  local addr="$2"
  local rc
  set +e
  timeout 2 "${BIN}" "${addr}" >"${tmp}/refuse.out" 2>"${tmp}/refuse.err"
  rc=$?
  set -e
  printf '%s\n' "${rc}" >"${tmp}/refuse_${label}.rc"
  cp "${tmp}/refuse.err" "${tmp}/refuse_${label}.err"
  if [[ "${rc}" -ne 3 ]]; then
    fail "${label} çıkış ${rc}"
  fi
  if ! grep -q "gerçek oturum veriyolu reddedildi" "${tmp}/refuse.err"; then
    fail "${label} reddetme iletisi yok"
  fi
}

if [[ -x "${BIN}" ]]; then
  refuse_one bare "/run/user/1000/bus"
  refuse_one unix "${REAL_BUS}"
  refuse_one guid "unix:path=/run/user/1000/bus,guid=olc"
  refuse_one parent "unix:path=/tmp/../run/user/1000/bus"
  if [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    refuse_one env "${DBUS_SESSION_BUS_ADDRESS}"
  else
    fail "DBUS_SESSION_BUS_ADDRESS yok"
  fi
fi

owner_after="$(real_owner)"
pid_after="$(real_status_field PID)"
cmd_after="$(real_status_field CommandLine)"
if [[ "${owner_after}" != "${owner_before}" || "${pid_after}" != "${pid_before}" ]]; then
  fail "gerçek sahip değişti önce=${owner_before}/${pid_before} sonra=${owner_after}/${pid_after}"
fi

left_netmon="$(pgrep -x crd-netmon || true)"
if [[ -n "${left_netmon}" ]]; then
  fail "crd-netmon kaldı: ${left_netmon}"
fi
if [[ -n "${dbus_pid}" ]] && kill -0 "${dbus_pid}" 2>/dev/null; then
  fail "dbus-daemon kaldı"
fi

LIB="${SCRIPT_DIR}/libcrd_netmon.a"
archive_symbol=no
binary_symbol=no
archive_has_main=no
if [[ ! -f "${LIB}" ]]; then
  fail "libcrd_netmon.a yok"
else
  if nm "${LIB}" | grep -q ' netmon_register$'; then
    archive_symbol=yes
  else
    fail "kütüphanede netmon_register yok"
  fi
  if nm "${LIB}" | grep -q ' main$'; then
    archive_has_main=yes
    fail "kütüphane main içeriyor"
  fi
fi
if [[ -x "${BIN}" ]] && nm "${BIN}" | grep -q ' netmon_register$'; then
  binary_symbol=yes
else
  fail "crd-netmon netmon_register içermiyor"
fi
if grep -nE 'g_bus_get|G_BUS_TYPE_SESSION|dbus_bus_get' \
    "${SCRIPT_DIR}/netmon.c" "${SCRIPT_DIR}/crd-netmon.c" "${SCRIPT_DIR}/netmon.h" >/dev/null; then
  fail "kaynak oturum veriyoluna bağlanıyor"
fi
if grep -nE 'portal\.h|crd_portal|libcrd_portal' \
    "${SCRIPT_DIR}/netmon.c" "${SCRIPT_DIR}/crd-netmon.c" "${SCRIPT_DIR}/netmon.h" "${SCRIPT_DIR}/Makefile" >/dev/null; then
  fail "portal.c bağı var"
fi
if nm "${BIN}" 2>/dev/null | grep -E 'crd_portal|libcrd_portal' >/dev/null; then
  fail "ikili portal sembolü taşıyor"
fi

case "${st_connectivity}" in
  1) anlam="yerel, internet yolu yok" ;;
  2) anlam="sınırlı bağlantı" ;;
  3) anlam="tutsak portal" ;;
  4) anlam="tam ağ" ;;
  *) anlam="bilinmeyen" ;;
esac
if [[ "${st_available}" == "false" ]]; then
  ag_durumu="yok"
else
  ag_durumu="var"
fi

xml_sha="$(sha256sum /usr/share/dbus-1/interfaces/org.freedesktop.portal.NetworkMonitor.xml | awk '{print $1}')"
changed_yn="no"
if [[ -n "${changed_line}" ]]; then
  changed_yn="yes"
fi
owner_same="no"
if [[ "${owner_before}" == "${owner_after}" && "${pid_before}" == "${pid_after}" && -n "${owner_before}" ]]; then
  owner_same="yes"
fi
process_left="yes"
if [[ -z "${left_netmon}" ]]; then
  process_left="no"
fi
fail_text="none"
if [[ ${#failures[@]} -gt 0 ]]; then
  fail_text="$(printf '%s; ' "${failures[@]}")"
fi

mkdir -p "${KANIT_DIR}"
cat >"${KANIT}" <<EOF
Bu parça portal.c'ye bağlanmaz.
Host çalıştırılmadı.
tarih=$(date -Iseconds)
belge=/usr/share/dbus-1/interfaces/org.freedesktop.portal.NetworkMonitor.xml
belge_sha256=${xml_sha}
ikili=src/netmon/crd-netmon
kaynak=src/netmon/netmon.c
kütüphane=src/netmon/libcrd_netmon.a
fonksiyon=netmon_register
arşivde_fonksiyon=${archive_symbol}
ikilide_fonksiyon=${binary_symbol}
arşivde_main=${archive_has_main}
portal_c_linked=no
özel_veriyolu=${address}
ad=${NAME}
yol=${PATH_OBJ}
arayüz=${IFACE}
ağ_kaynağı=${monitor_type}
gio_type=${gio_type}

ağ: ${ag_durumu}
GetStatus=${status_raw}
available=${st_available}
metered=${st_metered}
connectivity=${st_connectivity}
anlam=${anlam}
GetMetered=${metered_raw}
GetAvailable=${available_raw}
GetConnectivity=${connectivity_raw}
version=${version_prop}
Properties.Get=${version_get}
CanReach_example.com_443=${canreach_raw}
CanReach_rc=${canreach_rc}
boş_hostname_rc=${empty_host_rc}
boş_hostname=${empty_host_err}
kapı_70000_rc=${bad_port_rc}
kapı_70000=${bad_port_err}
gio_available=${gio_available}
gio_metered=${gio_metered}
gio_connectivity=${gio_connectivity}

changed_signal=${changed_yn}
changed_line=${changed_line}
introspection:
${introspect}

gerçek_veriyolu=${REAL_BUS}
real_owner_before=${owner_before}
real_owner_during=${owner_during}
real_owner_after=${owner_after}
real_pid_before=${pid_before}
real_pid_after=${pid_after}
real_command_before=${cmd_before}
real_command_after=${cmd_after}
real_owner_unchanged=${owner_same}
refusal_bare=$(cat "${tmp}/refuse_bare.rc" 2>/dev/null || true)
refusal_unix=$(cat "${tmp}/refuse_unix.rc" 2>/dev/null || true)
refusal_guid=$(cat "${tmp}/refuse_guid.rc" 2>/dev/null || true)
refusal_parent=$(cat "${tmp}/refuse_parent.rc" 2>/dev/null || true)
refusal_env=$(cat "${tmp}/refuse_env.rc" 2>/dev/null || true)
refusal_stderr=$(cat "${tmp}/refuse_unix.err" 2>/dev/null || true)
netmon_exit=${netmon_exit}
processes_left=${process_left}
private_pid=${private_pid}
stderr:
${stderr_text}
failures=${fail_text}
EOF
evidence_written=1

if [[ ${#failures[@]} -gt 0 ]]; then
  exit 1
fi
