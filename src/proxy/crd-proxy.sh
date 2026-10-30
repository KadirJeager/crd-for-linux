#!/usr/bin/env bash
# Host-facing D-Bus endpoint for CRD.
# The user's DBUS_SESSION_BUS_ADDRESS is never assigned.
# Each start copies src/portal/crd-portal onto src/proxy/crd-portal and executes only the copy.
# org.freedesktop.portal.Desktop is owned on the private bus by that copy.
# Other session names are forwarded through xdg-dbus-proxy. login1 is not.
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BUS_CONF="${SCRIPT_DIR}/bus.conf"
readonly PORTAL_BIN="${SCRIPT_DIR}/crd-portal"
readonly PORTAL_SOURCE="${SCRIPT_DIR}/../portal/crd-portal"
readonly RELAY="${SCRIPT_DIR}/crd-proxy-relay.py"
readonly MEASURE="${SCRIPT_DIR}/crd-proxy-measure.py"
readonly KANIT_DIR="${SCRIPT_DIR}/kanit"
readonly KANIT="${KANIT_DIR}/proxy.txt"
readonly RUNTIME="${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR yok}"
# CRD_PROXY_STATE: koltuk izleyicisi soketi paylaşılan bir dizine alır.
# Portal ve host, ekranı paylaşılan kullanıcı (CRD_HOST_USER) olarak bağlanır.
readonly STATE="${CRD_PROXY_STATE:-${RUNTIME}/crd-for-linux-proxy}"
readonly PRIVATE="${STATE}/private"
readonly FORWARD_SOCK="${STATE}/forward"
readonly FACE_SOCK="${STATE}/bus"
readonly PRIVATE_ADDR="unix:path=${PRIVATE}"
readonly FORWARD_ADDR="unix:path=${FORWARD_SOCK}"
readonly FACE_ADDR="unix:path=${FACE_SOCK}"
readonly PORTAL_NAME="org.freedesktop.portal.Desktop"

# Session destinations found in libremoting_core.so (object path next to the
# name) and in third_party/chromium-157. portal.Desktop is not in this list.
readonly -a HOST_CANDIDATES=(
  org.freedesktop.portal.Documents
  org.freedesktop.ScreenSaver
  org.freedesktop.systemd1
  org.gnome.Mutter.RemoteDesktop
  org.gnome.Mutter.ScreenCast
  org.gnome.Mutter.DisplayConfig
  org.gnome.ScreenSaver
)

REAL_BUS="${DBUS_SESSION_BUS_ADDRESS:?DBUS_SESSION_BUS_ADDRESS yok}"

usage() {
  echo "kullanım: $(basename "$0") --start|--stop|--proof" >&2
  exit 1
}

die() {
  echo "crd-proxy: $*" >&2
  exit 1
}

pid_is_ours() {
  local pid="$1"
  local args
  args="$(ps -p "${pid}" -o args= 2>/dev/null || true)"
  [[ "${args}" == *crd-for-linux-proxy* ||
    "${args}" == *"/crd-for-linux/seat/"* ||
    "${args}" == *"/src/proxy/crd-portal"* ||
    "${args}" == *crd-proxy-relay.py* ||
    "${args}" == *"/src/proxy/bus.conf"* ]]
}

read_pid_file() {
  local file="$1"
  local pid
  [[ -f "${file}" ]] || return 1
  pid="$(tr -d '[:space:]' <"${file}")"
  [[ "${pid}" =~ ^[0-9]+$ && "${pid}" -gt 1 ]] || return 1
  printf '%s\n' "${pid}"
}

any_alive() {
  local file pid
  shopt -s nullglob
  for file in "${STATE}"/*.pid; do
    pid="$(read_pid_file "${file}" || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null && pid_is_ours "${pid}"; then
      shopt -u nullglob
      return 0
    fi
  done
  shopt -u nullglob
  return 1
}

stop_one() {
  local pid="$1"
  if [[ "${pid}" -eq $$ ]]; then
    return 0
  fi
  if ! kill -0 "${pid}" 2>/dev/null; then
    return 0
  fi
  local args
  args="$(ps -p "${pid}" -o args= 2>/dev/null || true)"
  if [[ "${args}" == *crd-proxy.sh* ]]; then
    return 0
  fi
  if ! pid_is_ours "${pid}"; then
    echo "crd-proxy: pid ${pid} bizim sürecimiz değil, öldürülmedi" >&2
    return 0
  fi
  kill "${pid}" 2>/dev/null || true
  local _
  for _ in $(seq 1 50); do
    kill -0 "${pid}" 2>/dev/null || return 0
    sleep 0.1
  done
  if pid_is_ours "${pid}"; then
    kill -9 "${pid}" 2>/dev/null || true
  fi
  for _ in $(seq 1 20); do
    kill -0 "${pid}" 2>/dev/null || return 0
    sleep 0.1
  done
  if kill -0 "${pid}" 2>/dev/null; then
    echo "crd-proxy: pid ${pid} durmadı" >&2
    return 1
  fi
}

state_path_ok() {
  case "${STATE}" in
    */crd-for-linux-proxy | /run/crd-for-linux/seat) return 0 ;;
    *) return 1 ;;
  esac
}

stop_bus() {
  state_path_ok || die "durum dizini beklenmeyen yolda: ${STATE}"
  if [[ ! -d "${STATE}" ]]; then
    return 0
  fi
  local file pid
  local -a order=(face.pid relay.pid portal.pid forward.pid dbus.pid starter.pid)
  for file in "${order[@]}"; do
    pid="$(read_pid_file "${STATE}/${file}" || true)"
    if [[ -n "${pid}" ]]; then
      stop_one "${pid}" || true
    fi
  done
  shopt -s nullglob
  for file in "${STATE}"/*.pid; do
    pid="$(read_pid_file "${file}" || true)"
    if [[ -n "${pid}" ]]; then
      stop_one "${pid}" || true
    fi
  done
  shopt -u nullglob
  if [[ "${STATE}" == /run/crd-for-linux/seat ]]; then
    # Üst dizin kökün. Oturum kullanıcısı dizinin kendisini silemez.
    find "${STATE}" -mindepth 1 -delete 2>/dev/null || true
  else
    rm -rf "${STATE}"
    if [[ -e "${STATE}" ]]; then
      die "durum dizini silinemedi: ${STATE}"
    fi
  fi
}

dump_logs() {
  local label file
  for label in portal.log relay.log dbus.log; do
    file="${STATE}/${label}"
    echo "---- ${label} ----" >&2
    if [[ -f "${file}" ]]; then
      cat "${file}" >&2 || true
    else
      echo "(yok)" >&2
    fi
  done
  for label in forward.log face.log; do
    file="${STATE}/${label}"
    echo "---- ${label} (son 40 satır) ----" >&2
    if [[ -f "${file}" ]]; then
      tail -n 40 "${file}" >&2 || true
    else
      echo "(yok)" >&2
    fi
  done
}

fail_start() {
  echo "crd-proxy: $*" >&2
  if [[ -f "${STATE}/secim.txt" ]]; then
    echo "---- secim.txt ----" >&2
    cat "${STATE}/secim.txt" >&2 || true
  fi
  dump_logs
  stop_bus || true
  exit 1
}

name_has_owner() {
  local name="$1"
  local out rc
  set +e
  out="$(dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner "string:${name}" 2>&1)"
  rc=$?
  set -e
  {
    echo "--- NameHasOwner ${name} ---"
    printf '%s\n' "${out}"
    echo "exit=${rc}"
  } >>"${STATE}/secim.txt"
  [[ "${rc}" -eq 0 ]] && grep -q 'boolean true' <<<"${out}"
}

choose_names() {
  : >"${STATE}/secim.txt"
  {
    echo "kaynak: libremoting_core.so C dizgileri ve third_party/chromium-157"
    echo "portal adı köprüde kalır: ${PORTAL_NAME}"
    echo "sistem veriyolu bu parçada iletilmez: org.freedesktop.login1 org.gnome.DisplayManager"
    echo "aday oturum adları (portal.Desktop hariç): ${HOST_CANDIDATES[*]}"
    echo
  } >>"${STATE}/secim.txt"

  FORWARD=()
  local name
  for name in "${HOST_CANDIDATES[@]}"; do
    if name_has_owner "${name}"; then
      FORWARD+=("${name}")
      echo "karar=ilet" >>"${STATE}/secim.txt"
    else
      echo "karar=iletme (bu oturum veriyolunda sahibi yok)" >>"${STATE}/secim.txt"
    fi
    echo >>"${STATE}/secim.txt"
  done

  local -a never=(
    "${PORTAL_NAME}|köprü adı; gerçek portala gitmez"
    "org.freedesktop.login1|sistem veriyolu, oturum vekilinin işi değil"
    "org.gnome.DisplayManager|sistem veriyolu (GDM), oturum vekilinin işi değil"
    "org.gnome.Shell|ikilide birim şablonu (org.gnome.Shell@*.service); ayrı hedef adresi olarak iletilmez"
    "com.google.Chrome|nesne yolu yok; --own kullanılmadı"
  )
  local item reason
  for item in "${never[@]}"; do
    name="${item%%|*}"
    reason="${item#*|}"
    if name_has_owner "${name}"; then
      echo "karar=iletme (${reason}); gerçek veriyolunda sahibi var" >>"${STATE}/secim.txt"
    else
      echo "karar=iletme (${reason})" >>"${STATE}/secim.txt"
    fi
    echo >>"${STATE}/secim.txt"
  done

  if [[ "${#FORWARD[@]}" -eq 0 ]]; then
    echo "konuşma listesi boş: aday oturum adlarının hiçbiri bu oturum veriyolunda değil" >>"${STATE}/secim.txt"
    echo "yes" >"${STATE}/proof-only"
    set +e
    busctl --address="${REAL_BUS}" list --no-legend >"${STATE}/listnames.txt" 2>"${STATE}/listnames.err"
    local list_rc=$?
    set -e
    echo "--- busctl list (konuşma listesi boşken) exit=${list_rc} ---" >>"${STATE}/secim.txt"
    if [[ "${list_rc}" -ne 0 ]]; then
      cat "${STATE}/listnames.err" >>"${STATE}/secim.txt" || true
      fail_start "oturum adları sayılamadı"
    fi
    while read -r name _; do
      [[ "${name}" == *.* ]] || continue
      [[ "${name}" == :* ]] && continue
      case "${name}" in
        org.freedesktop.DBus | org.freedesktop.portal.Desktop | org.freedesktop.login1 | org.gnome.DisplayManager)
          continue
          ;;
      esac
      FORWARD+=("${name}")
      echo "ölçüm için seçilen gerçek oturum adı: ${name}" >>"${STATE}/secim.txt"
      break
    done <"${STATE}/listnames.txt"
  else
    echo "no" >"${STATE}/proof-only"
    echo "konuşma listesi boş değil; sahip olan adaylar iletiliyor" >>"${STATE}/secim.txt"
  fi

  if [[ "${#FORWARD[@]}" -eq 0 ]]; then
    if [[ "${CRD_PROXY_SHARE:-}" == "1" ]]; then
      echo "paylaşılan koltuk: iletilecek oturum adı yok, yalnız portal" >>"${STATE}/secim.txt"
      : >"${STATE}/forward.names"
      return 0
    fi
    fail_start "iletilecek gerçek oturum adı yok"
  fi

  printf '%s\n' "${FORWARD[@]}" >"${STATE}/forward.names"
  echo "iletilen: ${FORWARD[*]}" >>"${STATE}/secim.txt"
  {
    echo
    echo "bitmeyen iletim:"
    echo "- org.gnome.Mutter.RemoteDesktop, org.gnome.Mutter.ScreenCast, org.gnome.Mutter.DisplayConfig ikilide var; sahibi yoksa iletilmez"
    echo "- org.freedesktop.login1 ve org.gnome.DisplayManager sistem veriyolu; iletilmez"
    echo "- org.freedesktop.portal.* arayüzleri (RemoteDesktop, ScreenCast, Clipboard, Session, Request, NetworkMonitor, ProxyResolver, FileTransfer) ayrı veriyolu adı değil"
    echo "- org.gnome.system.proxy bir GSettings şeması; iletilmez"
    echo "- yöntem düzeyinde --call kuralı yok; iletilen adın bütün konuşması --talk ile açık. İkilde görülen yöntemler tam liste değildir"
  } >>"${STATE}/secim.txt"
}

spawn_logged() {
  local pidfile="$1"
  local logfile="$2"
  shift 2
  nohup "$@" </dev/null >"${logfile}" 2>&1 &
  echo $! >"${pidfile}"
}

wait_until() {
  local tries="$1"
  shift
  local _
  for _ in $(seq 1 "${tries}"); do
    if "$@"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

portal_visible_on() {
  local bus="$1"
  dbus-send --bus="${bus}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
    string:"${PORTAL_NAME}" >/dev/null 2>&1
}

copy_portal_to() {
  local dest="$1"
  [[ -x "${PORTAL_SOURCE}" ]] || die "portal ikilisi yok: ${PORTAL_SOURCE}"
  [[ "${dest}" != "${PORTAL_SOURCE}" ]] || die "portal kaynağı ile kopya aynı yol"
  if [[ -d "${dest}" ]]; then
    die "portal kopyası bir dizin"
  fi
  rm -f "${dest}"
  cp -a "${PORTAL_SOURCE}" "${dest}"
  chmod 0755 "${dest}"
  [[ -x "${dest}" ]] || die "crd-portal kopyası çalıştırılamıyor: ${dest}"
  cmp -s "${PORTAL_SOURCE}" "${dest}" || die "portal kopyası kaynakla aynı değil"
}

ensure_portal_copy() {
  [[ "${PORTAL_BIN}" == "${SCRIPT_DIR}/crd-portal" ]] || die "yanlış portal yolu"
  copy_portal_to "${PORTAL_BIN}"
}

start_bus() {
  [[ "${REAL_BUS}" != *" "* && -n "${REAL_BUS}" ]] || die "geçersiz DBUS_SESSION_BUS_ADDRESS"
  case "${REAL_BUS}" in
    *crd-for-linux-proxy*)
      die "DBUS_SESSION_BUS_ADDRESS zaten vekil yolu"
      ;;
  esac
  [[ -f "${BUS_CONF}" ]] || die "bus.conf yok"
  [[ -f "${RELAY}" ]] || die "köprü aktarıcı yok"

  if [[ -d "${STATE}" ]]; then
    if any_alive; then
      local live=""
      live="$(read_pid_file "${STATE}/dbus.pid" || read_pid_file "${STATE}/face.pid" || true)"
      die "zaten çalışıyor (pid ${live:-unknown})"
    fi
    if [[ "${CRD_PROXY_SHARE:-}" == "1" ]]; then
      find "${STATE}" -mindepth 1 -delete
    else
      rm -rf "${STATE}"
    fi
  fi
  local portal_exec="${PORTAL_BIN}"
  if [[ "${CRD_PROXY_SHARE:-}" == "1" ]]; then
    if [[ ! -d "${STATE}" ]]; then
      mkdir -m 0770 "${STATE}"
    fi
    # Karşılama kullanıcısı depo dizinine yazamaz; kopya onun sahip olduğu
    # durum dizinine alınır.
    portal_exec="${STATE}/crd-portal"
    copy_portal_to "${portal_exec}"
  else
    ensure_portal_copy
  fi
  if [[ ! -d "${STATE}" ]]; then
    if [[ "${CRD_PROXY_SHARE:-}" == "1" ]]; then
      mkdir -m 0770 "${STATE}"
    else
      mkdir -m 0700 "${STATE}"
    fi
  fi
  echo $$ >"${STATE}/starter.pid"
  if [[ "${CRD_PROXY_SHARE:-}" == "1" ]]; then
    umask 007
  else
    umask 077
  fi

  choose_names

  local lines address pid
  set +e
  lines="$(dbus-daemon --config-file="${BUS_CONF}" --address="${PRIVATE_ADDR}" \
    --fork --print-pid --print-address --nosyslog 2>"${STATE}/dbus.log")"
  local dbus_rc=$?
  set -e
  if [[ "${dbus_rc}" -ne 0 ]]; then
    fail_start "dbus-daemon çıkış ${dbus_rc}"
  fi
  address="$(printf '%s\n' "${lines}" | sed -n '1p')"
  pid="$(printf '%s\n' "${lines}" | sed -n '2p')"
  if [[ ! "${pid}" =~ ^[0-9]+$ ]]; then
    local swap="${address}"
    address="${pid}"
    pid="${swap}"
  fi
  [[ "${pid}" =~ ^[0-9]+$ ]] || fail_start "dbus-daemon pid okunamadı: ${lines}"
  printf '%s\n' "${pid}" >"${STATE}/dbus.pid"
  printf '%s\n' "${address}" >"${STATE}/private.address"
  kill -0 "${pid}" 2>/dev/null || fail_start "dbus-daemon ayakta değil"

  CRD_CANLI_GIRIS=1 spawn_logged "${STATE}/portal.pid" "${STATE}/portal.log" \
    "${portal_exec}" "${PRIVATE_ADDR}"
  # capture_init ilk kareyi bekledikten sonra ada sahip çıkar.
  if ! wait_until 400 portal_visible_on "${PRIVATE_ADDR}"; then
    fail_start "köprü portal adını alamadı"
  fi

  local name
  if [[ "${#FORWARD[@]}" -gt 0 ]]; then
    local -a forward_cmd=(xdg-dbus-proxy "${REAL_BUS}" "${FORWARD_SOCK}" --filter --log)
    for name in "${FORWARD[@]}"; do
      forward_cmd+=(--talk="${name}")
    done
    printf '%q ' "${forward_cmd[@]}" >"${STATE}/forward.argv"
    printf '\n' >>"${STATE}/forward.argv"
    if grep -F -- '--talk=org.freedesktop.portal.Desktop' "${STATE}/forward.argv" >/dev/null; then
      fail_start "ileri vekil portal adını taşıyor"
    fi
    if grep -F -- 'org.freedesktop.login1' "${STATE}/forward.argv" >/dev/null; then
      fail_start "ileri vekil login1 taşıyor"
    fi
    spawn_logged "${STATE}/forward.pid" "${STATE}/forward.log" "${forward_cmd[@]}"
    if ! wait_until 100 test -S "${FORWARD_SOCK}"; then
      fail_start "ileri vekil soketi açılmadı"
    fi

    nohup python3 -u "${RELAY}" "${PRIVATE_ADDR}" "${FORWARD_ADDR}" "${FORWARD[@]}" \
      </dev/null >"${STATE}/relay.ready" 2>"${STATE}/relay.log" &
    echo $! >"${STATE}/relay.pid"
    if ! wait_until 200 relay_ready; then
      fail_start "aktarıcı hazır olmadı"
    fi
    local forwarded
    for forwarded in "${FORWARD[@]}"; do
      if ! wait_until 100 portal_visible_on_name "${PRIVATE_ADDR}" "${forwarded}"; then
        fail_start "özel veriyolunda ad yok: ${forwarded}"
      fi
    done
  fi

  local -a face_cmd=(
    xdg-dbus-proxy "${PRIVATE_ADDR}" "${FACE_SOCK}" --filter --log
    --talk="${PORTAL_NAME}"
  )
  for name in "${FORWARD[@]}"; do
    face_cmd+=(--talk="${name}")
  done
  printf '%q ' "${face_cmd[@]}" >"${STATE}/face.argv"
  printf '\n' >>"${STATE}/face.argv"
  if grep -F -- "${REAL_BUS}" "${STATE}/face.argv" >/dev/null; then
    fail_start "host vekili gerçek oturum adresine bağlanıyor"
  fi
  spawn_logged "${STATE}/face.pid" "${STATE}/face.log" "${face_cmd[@]}"
  if ! wait_until 100 test -S "${FACE_SOCK}"; then
    fail_start "host soketi açılmadı"
  fi
  if ! wait_until 100 portal_visible_on "${FACE_ADDR}"; then
    fail_start "host ucunda portal adı yok"
  fi
  printf '%s\n' "${FACE_ADDR}"
}

portal_visible_on_name() {
  local bus="$1"
  local name="$2"
  dbus-send --bus="${bus}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
    string:"${name}" >/dev/null 2>&1
}

relay_ready() {
  [[ -f "${STATE}/relay.ready" ]] || return 1
  grep -qx ready "${STATE}/relay.ready" 2>/dev/null || return 1
  local pid
  pid="$(read_pid_file "${STATE}/relay.pid" || true)"
  [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

extract_string() {
  local line
  while IFS= read -r line; do
    if [[ "${line}" =~ string\ \"([^\"]*)\" ]]; then
      printf '%s\n' "${BASH_REMATCH[1]}"
      return 0
    fi
  done
  return 0
}

extract_uint32() {
  local line
  while IFS= read -r line; do
    if [[ "${line}" =~ uint32\ ([0-9]+) ]]; then
      printf '%s\n' "${BASH_REMATCH[1]}"
      return 0
    fi
  done
  return 0
}

payload_line() {
  local line
  while IFS= read -r line; do
    if [[ "${line}" == *'string "'* ]]; then
      printf '%s\n' "${line#"${line%%[![:space:]]*}"}"
      return 0
    fi
  done
  return 0
}

field() {
  local file="$1"
  local key="$2"
  local line
  [[ -f "${file}" ]] || return 0
  while IFS= read -r line || [[ -n "${line}" ]]; do
    if [[ "${line}" == "${key}="* ]]; then
      printf '%s\n' "${line#"${key}="}"
      return 0
    fi
  done <"${file}"
  return 0
}

capture() {
  local dest="$1"
  shift
  set +e
  "$@" >"${dest}.out" 2>"${dest}.err"
  local rc=$?
  set -e
  printf '%s\n' "${rc}" >"${dest}.rc"
}

proof() {
  trap '"${SCRIPT_DIR}/crd-proxy.sh" --stop >/dev/null 2>&1 || true' EXIT
  mkdir -p "${KANIT_DIR}"
  if [[ -d "${STATE}" ]]; then
    "${SCRIPT_DIR}/crd-proxy.sh" --stop || true
  fi
  local work
  work="$(mktemp -d "${KANIT_DIR}/.work.XXXXXX")"
  local fail=0
  local env_before="${DBUS_SESSION_BUS_ADDRESS}"
  local placeholder="/tmp/crd-for-linux-dbus-placeholder"
  local session_dir="${RUNTIME}/crd-for-linux"
  local placeholder_before=absent
  local session_before=absent
  [[ -e "${placeholder}" ]] && placeholder_before=present
  [[ -e "${session_dir}" ]] && session_before=present

  capture "${work}/real-portal-before" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:"${PORTAL_NAME}"
  local real_owner_before
  real_owner_before="$(extract_string <"${work}/real-portal-before.out")"
  capture "${work}/real-portal-pid-before" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetConnectionUnixProcessID \
    string:"${real_owner_before}"
  local real_pid_before
  real_pid_before="$(extract_uint32 <"${work}/real-portal-pid-before.out")"
  ps -p "${real_pid_before}" -o pid=,args= >"${work}/real-portal-ps-before" 2>/dev/null || true

  capture "${work}/real-systemd-before" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:org.freedesktop.systemd1

  local start_rc=0
  set +e
  "${SCRIPT_DIR}/crd-proxy.sh" --start >"${work}/start.out" 2>"${work}/start.err"
  start_rc=$?
  set -e

  local address=""
  address="$(head -n 1 "${work}/start.out" || true)"
  if [[ "${start_rc}" -ne 0 || "${address}" != "${FACE_ADDR}" ]]; then
    fail=1
  fi

  local proxy_owner="" proxy_pid="" portal_file_pid="" portal_ps=""
  local create_rc=1 version_proxy="" version_real="" version_ok=no
  local forward_hidden="" hidden_ok=no
  local measure_name=""
  local pipe_rc=1 pipewire_kind="" pipewire_peer="" pipewire_domain="" pipewire_type=""
  local stream_node="" stream_size="" mapping_id="" chain_failures=""
  if [[ "${fail}" -eq 0 ]]; then
    measure_name="org.freedesktop.systemd1"
    if ! grep -qx "${measure_name}" "${STATE}/forward.names"; then
      measure_name="$(head -n 1 "${STATE}/forward.names")"
    fi
    capture "${work}/proxy-owner" \
      dbus-send --bus="${FACE_ADDR}" --print-reply --dest=org.freedesktop.DBus \
      /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:"${PORTAL_NAME}"
    proxy_owner="$(extract_string <"${work}/proxy-owner.out")"
    capture "${work}/proxy-status" \
      busctl --address="${FACE_ADDR}" status "${PORTAL_NAME}"
    local status_line
    while IFS= read -r status_line; do
      case "${status_line}" in
        PID=*) proxy_pid="${status_line#PID=}" ;;
        CommandLine=*) portal_ps="${status_line#CommandLine=}" ;;
      esac
    done <"${work}/proxy-status.out"
    portal_file_pid="$(read_pid_file "${STATE}/portal.pid" || true)"

    if [[ ! -f "${MEASURE}" ]]; then
      echo "ölçüm aracı yok" >"${work}/pipewire.err"
      echo 1 >"${work}/pipewire.rc"
    else
      capture "${work}/pipewire" \
        python3 -u "${MEASURE}" "${FACE_ADDR}" "${work}/pipewire.facts"
    fi
    if [[ -f "${work}/pipewire.rc" ]]; then
      pipe_rc="$(cat "${work}/pipewire.rc")"
    fi
    pipewire_kind="$(field "${work}/pipewire.facts" pipewire_fd_kind)"
    pipewire_peer="$(field "${work}/pipewire.facts" pipewire_peer_path)"
    pipewire_domain="$(field "${work}/pipewire.facts" pipewire_fd_domain)"
    pipewire_type="$(field "${work}/pipewire.facts" pipewire_fd_type)"
    stream_node="$(field "${work}/pipewire.facts" stream_node)"
    stream_size="$(field "${work}/pipewire.facts" size)"
    mapping_id="$(field "${work}/pipewire.facts" mapping_id)"
    chain_failures="$(field "${work}/pipewire.facts" failures)"
    if [[ "$(field "${work}/pipewire.facts" create_response)" == "0" ]]; then
      create_rc=0
    fi

    if [[ "${measure_name}" == "org.freedesktop.systemd1" ]]; then
      capture "${work}/version-proxy" \
        dbus-send --bus="${FACE_ADDR}" --reply-timeout=5000 --print-reply --type=method_call \
        --dest=org.freedesktop.systemd1 \
        /org/freedesktop/systemd1 \
        org.freedesktop.DBus.Properties.Get \
        string:org.freedesktop.systemd1.Manager string:Version
      capture "${work}/version-real" \
        dbus-send --bus="${REAL_BUS}" --reply-timeout=5000 --print-reply --type=method_call \
        --dest=org.freedesktop.systemd1 \
        /org/freedesktop/systemd1 \
        org.freedesktop.DBus.Properties.Get \
        string:org.freedesktop.systemd1.Manager string:Version
    else
      capture "${work}/version-proxy" \
        dbus-send --bus="${FACE_ADDR}" --reply-timeout=5000 --print-reply --type=method_call \
        --dest="${measure_name}" \
        / org.freedesktop.DBus.Introspectable.Introspect
      capture "${work}/version-real" \
        dbus-send --bus="${REAL_BUS}" --reply-timeout=5000 --print-reply --type=method_call \
        --dest="${measure_name}" \
        / org.freedesktop.DBus.Introspectable.Introspect
    fi
    version_proxy="$(payload_line <"${work}/version-proxy.out")"
    version_real="$(payload_line <"${work}/version-real.out")"
    if [[ -n "${version_proxy}" && "${version_proxy}" == "${version_real}" ]]; then
      version_ok=yes
    fi

    capture "${work}/forward-hidden" \
      dbus-send --bus="${FORWARD_ADDR}" --print-reply --dest=org.freedesktop.DBus \
      /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner string:"${PORTAL_NAME}"
    forward_hidden="$(cat "${work}/forward-hidden.out")"
    if grep -q 'boolean false' "${work}/forward-hidden.out"; then
      hidden_ok=yes
    fi
  fi

  capture "${work}/real-portal-after" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:"${PORTAL_NAME}"
  local real_owner_after
  real_owner_after="$(extract_string <"${work}/real-portal-after.out")"
  capture "${work}/real-portal-pid-after" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetConnectionUnixProcessID \
    string:"${real_owner_after}"
  local real_pid_after
  real_pid_after="$(extract_uint32 <"${work}/real-portal-pid-after.out")"

  capture "${work}/real-systemd-after" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:org.freedesktop.systemd1

  local env_mid="${DBUS_SESSION_BUS_ADDRESS}"
  local second_rc=0
  set +e
  "${SCRIPT_DIR}/crd-proxy.sh" --start >"${work}/second.out" 2>"${work}/second.err"
  second_rc=$?
  set -e

  local saved_pids=""
  if [[ -d "${STATE}" ]]; then
    saved_pids="$(
      shopt -s nullglob
      for file in "${STATE}"/*.pid; do
        echo "$(basename "${file}")=$(cat "${file}" 2>/dev/null || true)"
      done
    )"
    cp -a "${STATE}/secim.txt" "${work}/secim.txt" 2>/dev/null || true
    cp -a "${STATE}/forward.names" "${work}/forward.names" 2>/dev/null || true
    cp -a "${STATE}/forward.argv" "${work}/forward.argv" 2>/dev/null || true
    cp -a "${STATE}/face.argv" "${work}/face.argv" 2>/dev/null || true
    cp -a "${STATE}/portal.log" "${work}/portal.log" 2>/dev/null || true
    cp -a "${STATE}/relay.log" "${work}/relay.log" 2>/dev/null || true
    cp -a "${STATE}/face.log" "${work}/face.log" 2>/dev/null || true
    cp -a "${STATE}/proof-only" "${work}/proof-only" 2>/dev/null || true
  fi

  set +e
  "${SCRIPT_DIR}/crd-proxy.sh" --stop >"${work}/stop.out" 2>"${work}/stop.err"
  local stop_rc=$?
  set -e

  local socket_after=absent state_after=absent
  [[ -e "${FACE_SOCK}" || -e "${PRIVATE}" || -e "${FORWARD_SOCK}" ]] && socket_after=present
  [[ -d "${STATE}" ]] && state_after=present
  local placeholder_after=absent session_after=absent
  [[ -e "${placeholder}" ]] && placeholder_after=present
  [[ -e "${session_dir}" ]] && session_after=present

  local leftover
  leftover="$(ps -eo pid=,args= | grep -E 'crd-for-linux-proxy|/src/proxy/crd-portal|crd-proxy-relay.py|/src/proxy/bus.conf' | grep -v grep || true)"

  local env_after="${DBUS_SESSION_BUS_ADDRESS}"
  capture "${work}/real-portal-stopped" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:"${PORTAL_NAME}"
  local real_owner_stopped
  real_owner_stopped="$(extract_string <"${work}/real-portal-stopped.out")"
  capture "${work}/real-portal-pid-stopped" \
    dbus-send --bus="${REAL_BUS}" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetConnectionUnixProcessID \
    string:"${real_owner_stopped}"
  local real_pid_stopped
  real_pid_stopped="$(extract_uint32 <"${work}/real-portal-pid-stopped.out")"

  local portal_ok=no create_ok=no owner_ok=no env_ok=no second_ok=no clean_ok=no
  local systemd_owner_ok=no hidden_recorded="${hidden_ok}"
  local pipewire_ok=no copy_ok=no capture_log_ok=no
  local expected_peer="${RUNTIME}/pipewire-0"
  local session_handle=""
  session_handle="$(field "${work}/pipewire.facts" session_handle)"
  if [[ -n "${proxy_pid}" && "${proxy_pid}" == "${portal_file_pid}" && "${portal_ps}" == *"/src/proxy/crd-portal"* && "${portal_ps}" != *"/src/portal/crd-portal"* ]]; then
    portal_ok=yes
  fi
  if [[ "${create_rc}" -eq 0 && "${session_handle}" == /org/freedesktop/portal/desktop/session/* ]]; then
    create_ok=yes
  fi
  if [[ "${pipe_rc}" -eq 0 && "${chain_failures}" == "none" && "${pipewire_kind}" == "pipewire" && "${pipewire_peer}" == "${expected_peer}" && "${pipewire_domain}" == "AF_UNIX" && "${pipewire_type}" == "SOCK_STREAM" && -n "${mapping_id}" ]]; then
    if [[ "${stream_node}" =~ ^[1-9][0-9]*$ && "${stream_size}" =~ ^[1-9][0-9]*,[1-9][0-9]*$ ]]; then
      pipewire_ok=yes
    fi
  fi
  if [[ "${pipewire_kind}" == "socketpair" ]]; then
    pipewire_ok=no
  fi
  if [[ -f "${PORTAL_SOURCE}" && -f "${PORTAL_BIN}" ]] && cmp -s "${PORTAL_SOURCE}" "${PORTAL_BIN}"; then
    copy_ok=yes
  fi
  if [[ -n "${stream_node}" && -f "${work}/portal.log" ]] && grep -F -q "capture_stream node_id=${stream_node} " "${work}/portal.log"; then
    capture_log_ok=yes
  fi
  if [[ -n "${real_owner_before}" && "${real_owner_before}" == "${real_owner_after}" && "${real_owner_before}" == "${real_owner_stopped}" && "${real_pid_before}" == "${real_pid_after}" && "${real_pid_before}" == "${real_pid_stopped}" && "${real_pid_before}" != "${portal_file_pid}" ]]; then
    owner_ok=yes
  fi
  local systemd_before systemd_after
  systemd_before="$(extract_string <"${work}/real-systemd-before.out")"
  systemd_after="$(extract_string <"${work}/real-systemd-after.out")"
  if [[ -n "${systemd_before}" && "${systemd_before}" == "${systemd_after}" ]]; then
    systemd_owner_ok=yes
  fi
  if [[ "${env_before}" == "${env_mid}" && "${env_before}" == "${env_after}" && "${env_before}" == "unix:path=/run/user/1000/bus" ]]; then
    env_ok=yes
  fi
  if [[ "${second_rc}" -eq 1 ]] && grep -q 'zaten çalışıyor' "${work}/second.err"; then
    second_ok=yes
  fi
  if [[ "${stop_rc}" -eq 0 && "${socket_after}" == absent && "${state_after}" == absent && -z "${leftover}" && "${placeholder_before}" == "${placeholder_after}" && "${session_before}" == "${session_after}" ]]; then
    clean_ok=yes
  fi
  if [[ "${start_rc}" -ne 0 || "${portal_ok}" != yes || "${copy_ok}" != yes || "${create_ok}" != yes || "${pipewire_ok}" != yes || "${capture_log_ok}" != yes || "${version_ok}" != yes || "${owner_ok}" != yes || "${env_ok}" != yes || "${second_ok}" != yes || "${clean_ok}" != yes || "${hidden_recorded}" != yes || "${systemd_owner_ok}" != yes ]]; then
    fail=1
  fi

  {
    echo "=== CRD proxy proof $(date -Iseconds) ==="
    xdg-dbus-proxy --version
    echo "portal kopyası:"
    echo "portal_copy_matches_source=${copy_ok}"
    if [[ -f "${PORTAL_SOURCE}" ]]; then
      sha256sum "${PORTAL_SOURCE}"
    else
      echo "kaynak yok"
    fi
    if [[ -f "${PORTAL_BIN}" ]]; then
      sha256sum "${PORTAL_BIN}"
    else
      echo "kopya yok"
    fi
    echo "DBUS_SESSION_BUS_ADDRESS before: ${env_before}"
    echo "placeholder ${placeholder} before: ${placeholder_before}"
    echo "session dir ${session_dir} before: ${session_before}"
    echo
    echo "--- real portal owner before ---"
    cat "${work}/real-portal-before.out"
    echo "stderr:"
    cat "${work}/real-portal-before.err"
    echo "exit=$(cat "${work}/real-portal-before.rc")"
    echo "unique=${real_owner_before}"
    cat "${work}/real-portal-pid-before.out"
    echo "pid=${real_pid_before}"
    echo "ps:"
    cat "${work}/real-portal-ps-before" 2>/dev/null || true
    echo
    echo "--- real systemd1 owner before ---"
    cat "${work}/real-systemd-before.out"
    echo "unique=${systemd_before}"
    echo
    echo "--- name selection ---"
    if [[ -f "${work}/secim.txt" ]]; then
      cat "${work}/secim.txt"
    else
      echo "(secim.txt yok)"
      cat "${work}/start.err" || true
    fi
    echo
    echo "--- forwarded names ---"
    if [[ -f "${work}/forward.names" ]]; then
      cat "${work}/forward.names"
    else
      echo "(yok)"
    fi
    if [[ -f "${work}/proof-only" ]]; then
      echo "proof_only=$(cat "${work}/proof-only")"
    fi
    echo
    echo "--- start ---"
    echo "exit=${start_rc}"
    echo "stdout:"
    cat "${work}/start.out"
    echo "stderr:"
    cat "${work}/start.err"
    echo "address=${address}"
    echo "face argv:"
    cat "${work}/face.argv" 2>/dev/null || true
    echo "forward argv:"
    cat "${work}/forward.argv" 2>/dev/null || true
    echo
    echo "--- portal owner through host-facing proxy ---"
    if [[ -f "${work}/proxy-owner.out" ]]; then
      cat "${work}/proxy-owner.out"
      echo "stderr:"
      cat "${work}/proxy-owner.err"
      echo "exit=$(cat "${work}/proxy-owner.rc")"
      echo "unique=${proxy_owner}"
      echo "--- busctl status (host-facing proxy) ---"
      cat "${work}/proxy-status.out"
      echo "stderr:"
      cat "${work}/proxy-status.err"
      echo "exit=$(cat "${work}/proxy-status.rc")"
      echo "pid=${proxy_pid}"
      echo "portal.pid=${portal_file_pid}"
      echo "command=${portal_ps}"
    else
      echo "(ölçülmedi)"
    fi
    echo "portal_owner_is_our_copy=${portal_ok}"
    echo
    echo "--- CreateSession → OpenPipeWireRemote (host socket) ---"
    echo "expected_peer=${expected_peer}"
    if [[ -f "${work}/pipewire.out" ]]; then
      cat "${work}/pipewire.out"
      echo "stderr:"
      cat "${work}/pipewire.err"
      echo "exit=${pipe_rc}"
    else
      echo "(çağrılmadı)"
    fi
    echo "create_ok=${create_ok}"
    echo "pipewire_fd_kind=${pipewire_kind:-absent}"
    echo "pipewire_peer=${pipewire_peer:-absent}"
    echo "pipewire_ok=${pipewire_ok}"
    echo "stream_node=${stream_node:-absent}"
    echo "stream_size=${stream_size:-absent}"
    echo "mapping_id=${mapping_id:-absent}"
    echo "capture_log=${capture_log_ok}"
    echo
    echo "--- forwarded name ${measure_name} through proxy ---"
    if [[ -f "${work}/version-proxy.out" ]]; then
      cat "${work}/version-proxy.out"
      echo "stderr:"
      cat "${work}/version-proxy.err"
      echo "exit=$(cat "${work}/version-proxy.rc")"
      echo "--- same call on real session bus ---"
      cat "${work}/version-real.out"
      echo "stderr:"
      cat "${work}/version-real.err"
      echo "exit=$(cat "${work}/version-real.rc")"
    else
      echo "(çağrılmadı)"
    fi
    echo "payload_proxy=${version_proxy}"
    echo "payload_real=${version_real}"
    echo "payload_match=${version_ok}"
    echo
    echo "--- portal name on forward proxy (must stay hidden) ---"
    if [[ -f "${work}/forward-hidden.out" ]]; then
      cat "${work}/forward-hidden.out"
      echo "stderr:"
      cat "${work}/forward-hidden.err"
      echo "exit=$(cat "${work}/forward-hidden.rc")"
    else
      echo "(ölçülmedi)"
    fi
    echo "hidden=${hidden_recorded}"
    echo
    echo "--- real portal owner after calls ---"
    cat "${work}/real-portal-after.out"
    echo "unique=${real_owner_after} pid=${real_pid_after}"
    echo "--- real systemd1 owner after calls ---"
    cat "${work}/real-systemd-after.out"
    echo "unique=${systemd_after}"
    echo "systemd_owner_unchanged=${systemd_owner_ok}"
    echo "DBUS_SESSION_BUS_ADDRESS after calls: ${env_mid}"
    echo
    echo "--- portal.log ---"
    cat "${work}/portal.log" 2>/dev/null || true
    echo
    echo "--- relay.log ---"
    cat "${work}/relay.log" 2>/dev/null || true
    echo
    echo "--- face.log (son 80 satır) ---"
    if [[ -f "${work}/face.log" ]]; then
      tail -n 80 "${work}/face.log"
    else
      echo "(yok)"
    fi
    echo
    echo "--- second start (must be rejected) ---"
    echo "exit=${second_rc}"
    echo "stdout:"
    cat "${work}/second.out"
    echo "stderr:"
    cat "${work}/second.err"
    echo
    echo "--- pids before stop ---"
    printf '%s\n' "${saved_pids}"
    echo
    echo "--- stop ---"
    echo "exit=${stop_rc}"
    echo "stdout:"
    cat "${work}/stop.out"
    echo "stderr:"
    cat "${work}/stop.err"
    echo "socket after: ${socket_after}"
    echo "state_dir after: ${state_after}"
    echo "placeholder after: ${placeholder_after}"
    echo "session dir after: ${session_after}"
    echo "leftover processes:"
    if [[ -n "${leftover}" ]]; then
      printf '%s\n' "${leftover}"
    else
      echo "(none)"
    fi
    echo
    echo "--- real portal owner after stop ---"
    cat "${work}/real-portal-stopped.out"
    echo "unique=${real_owner_stopped} pid=${real_pid_stopped}"
    echo "DBUS_SESSION_BUS_ADDRESS after stop: ${env_after}"
    echo
    echo "--- summary ---"
    if [[ "${fail}" -eq 0 ]]; then
      echo "all_checks_pass=yes"
    else
      echo "all_checks_pass=no"
    fi
    echo "start=${start_rc} portal_ok=${portal_ok} portal_copy=${copy_ok} create_ok=${create_ok} pipewire_fd_kind=${pipewire_kind:-absent} pipewire_peer=${pipewire_peer:-absent} pipewire_ok=${pipewire_ok} stream_node=${stream_node:-absent} capture_log=${capture_log_ok} payload_match=${version_ok} real_owner_same=${owner_ok} systemd_owner_same=${systemd_owner_ok} env_ok=${env_ok} second_start=${second_rc} clean_ok=${clean_ok} portal_hidden_on_forward=${hidden_recorded}"
  } >"${KANIT}"
  rm -rf "${work}"
  if [[ "${fail}" -ne 0 ]]; then
    return 1
  fi
}

main() {
  case "${1:-}" in
    --start) start_bus ;;
    --stop) stop_bus ;;
    --proof) proof ;;
    *) usage ;;
  esac
}

main "$@"
