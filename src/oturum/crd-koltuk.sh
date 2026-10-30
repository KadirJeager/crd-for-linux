#!/usr/bin/env bash
# Host her zaman CRD_HOST_USER'dır ve hiç kapanmayan boş bir Wayland
# ekranına bağlanır. Karşılama ekranı kapanınca host ölmez. Görüntü,
# fare ve klavye portalın oturum soketinden gelir; koltuk değişince
# portal aynı süreçte yeni sokete geçer.
set -u

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly PROXY="${ROOT}/src/proxy/crd-proxy.sh"
readonly SES="${ROOT}/src/ses/crd-ses.sh"
readonly HOST_BIN="${ROOT}/third_party/unpacked/opt/google/chrome-remote-desktop/chrome-remote-desktop-host"
# Ekranı paylaşılan kullanıcı birimden gelir (kur.sh yazar). Uid, ev dizini ve
# çalışma dizini ondan türetilir; makineye özgü değer betikte durmaz.
if [[ -z "${CRD_HOST_USER:-}" ]]; then
  echo "crd-koltuk: CRD_HOST_USER verilmedi" >&2
  exit 2
fi
readonly HOST_USER="${CRD_HOST_USER}"
if ! HOST_UID="$(id -u "${HOST_USER}" 2>/dev/null)"; then
  echo "crd-koltuk: kullanıcı yok: ${HOST_USER}" >&2
  exit 2
fi
readonly HOST_UID
HOST_HOME="$(getent passwd "${HOST_USER}" | cut -d: -f6)"
readonly HOST_HOME
readonly HOST_RUNTIME="/run/user/${HOST_UID}"
readonly STATE="/run/crd-for-linux/seat"
readonly LOG="/run/crd-for-linux/koltuk.log"

host_pid=""
pipewire_pid=""
pipewire_owned=0
gtk_pid=""
stack_user=""
stack_runtime=""
stack_dbus=""
current_key=""
host_born=0

log() {
  printf '%s %s\n' "$(date -Iseconds)" "$*" | tee -a "${LOG}" >&2
}

host_json() {
  local -a files=()
  shopt -s nullglob
  files=("${HOST_HOME}/.config/chrome-remote-desktop/"host#*.json)
  shopt -u nullglob
  if ((${#files[@]} == 0)); then
    return 1
  fi
  printf '%s\n' "${files[0]}"
}

stop_foreign_host() {
  local runtime="${HOST_RUNTIME}"
  if [[ -S "${runtime}/bus" && -x "${PROXY}" ]]; then
    runuser -u "${HOST_USER}" -- env \
      XDG_RUNTIME_DIR="${runtime}" \
      DBUS_SESSION_BUS_ADDRESS="unix:path=${runtime}/bus" \
      "${PROXY}" --stop >/dev/null 2>&1 || true
  fi
  local pid
  # Süreç adı 15 karakterde kesilir; -x bu ikiliyi görmez.
  for pid in $(pgrep -f '/chrome-remote-desktop-host --host-config=-' || true); do
    kill "${pid}" 2>/dev/null || true
  done
  sleep 0.5
  for pid in $(pgrep -f '/chrome-remote-desktop-host --host-config=-' || true); do
    kill -9 "${pid}" 2>/dev/null || true
  done
}

kill_tree() {
  local pid="$1"
  local child
  for child in $(ps -o pid= --ppid "${pid}" 2>/dev/null || true); do
    kill_tree "${child}"
  done
  kill "${pid}" 2>/dev/null || true
}

stop_stack() {
  if [[ -n "${stack_user}" && -n "${stack_runtime}" && -n "${stack_dbus}" ]]; then
    runuser -u "${stack_user}" -- env \
      XDG_RUNTIME_DIR="${stack_runtime}" \
      DBUS_SESSION_BUS_ADDRESS="${stack_dbus}" \
      CRD_PROXY_STATE="${STATE}" \
      CRD_PROXY_SHARE=1 \
      "${PROXY}" --stop >/dev/null 2>&1 || true
  fi
  if [[ -n "${host_pid}" ]] && kill -0 "${host_pid}" 2>/dev/null; then
    # host_pid sarmalayıcıdır; runuser ve ikili onun altındadır.
    kill_tree "${host_pid}"
    local _
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "${host_pid}" 2>/dev/null || break
      sleep 0.2
    done
    kill_tree "${host_pid}"
    kill -9 "${host_pid}" 2>/dev/null || true
  fi
  host_pid=""
  if [[ "${pipewire_owned}" == "1" && -n "${pipewire_pid}" ]] && kill -0 "${pipewire_pid}" 2>/dev/null; then
    kill "${pipewire_pid}" 2>/dev/null || true
  fi
  pipewire_pid=""
  pipewire_owned=0
  stack_user=""
  stack_runtime=""
  stack_dbus=""
}

wayland_name() {
  local uid="$1"
  local pid="$2"
  local runtime="/run/user/${uid}"
  local sock base inode fd target
  shopt -s nullglob
  for sock in "${runtime}"/wayland-*; do
    base="${sock##*/}"
    [[ "${base}" =~ ^wayland-[0-9]+$ && -S "${sock}" ]] || continue
    # Dinleyen soketin readlink'i yol değil socket:[inode] döner.
    inode="$(awk -v p="${sock}" '$NF == p && $4 == "00010000" { print $(NF-1); exit }' /proc/net/unix)"
    [[ -n "${inode}" ]] || continue
    for fd in /proc/"${pid}"/fd/*; do
      target="$(readlink "${fd}" 2>/dev/null || true)"
      if [[ "${target}" == "socket:[${inode}]" ]]; then
        printf '%s\n' "${base}"
        shopt -u nullglob
        return 0
      fi
    done
  done
  shopt -u nullglob
  return 1
}

# Kompozitör süreci adıyla bulunur; soketi sonra /proc/net/unix ile eşlenir.
# Hyprland dışındakiler test edilmedi. GNOME (gnome-shell) bu yoldan geçmez:
# orada host Mutter'ı kendisi kullanır (src/gnome).
compositor_pid() {
  local uid="$1"
  # dms-greeter --command hyprland argv0'ı "hyprland" bırakabilir.
  ps -u "${uid}" -o pid=,comm= | awk '
    $2=="Hyprland" || $2=="hyprland" || $2=="sway" || $2=="kwin_wayland" ||
    $2=="river" || $2=="labwc" || $2=="wayfire" || $2=="niri" {print $1; exit}'
}

ensure_pipewire() {
  local user="$1"
  local runtime="$2"
  pipewire_pid=""
  pipewire_owned=0
  if [[ -S "${runtime}/pipewire-0" ]]; then
    return 0
  fi
  if ! command -v pipewire >/dev/null; then
    log "pipewire yok, görüntü soketi açılmayabilir"
    return 0
  fi
  runuser -u "${user}" -- env XDG_RUNTIME_DIR="${runtime}" pipewire \
    >>"${LOG}" 2>&1 &
  pipewire_pid=$!
  pipewire_owned=1
  local _
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ -S "${runtime}/pipewire-0" ]] && return 0
    sleep 0.2
  done
  log "pipewire soketi gelmedi (${user})"
  return 1
}

open_share() {
  local user="$1"
  install -d -m 0755 /run/crd-for-linux
  if [[ -d "${STATE}" ]]; then
    find "${STATE}" -mindepth 1 -delete 2>/dev/null || true
  fi
  install -d -m 0770 -o "${user}" -g "${user}" "${STATE}"
}

relax_sockets() {
  local dir
  find "${STATE}" -type d -exec chmod 0770 {} +
  find "${STATE}" -type s -exec chmod 0660 {} +
  # Alt dizinler oluşturan süreç kendi grubunda kalır; host CRD_HOST_USER
  # greeter grubunda olduğu için greeter soketine erişir.
  dir="$(stat -c '%U' "${STATE}")"
  chown -R "${dir}:${dir}" "${STATE}"
}

display_of() {
  local user="$1"
  local sock name
  shopt -s nullglob
  for sock in /tmp/.X11-unix/X[0-9]*; do
    [[ -S "${sock}" ]] || continue
    name="${sock##*/X}"
    [[ "${name}" =~ ^[0-9]+$ ]] || continue
    if [[ "$(stat -c '%U' "${sock}")" == "${user}" ]]; then
      printf ':%s\n' "${name}"
      shopt -u nullglob
      return 0
    fi
  done
  shopt -u nullglob
  return 1
}

ensure_gtk() {
  local _
  if [[ -n "${gtk_pid}" ]] && kill -0 "${gtk_pid}" 2>/dev/null && [[ -S /run/crd-for-linux/gtk.sock ]]; then
    return 0
  fi
  rm -f /run/crd-for-linux/gtk.sock
  "${ROOT}/src/oturum/gtk-ekran" /run/crd-for-linux/gtk.sock >>"${LOG}" 2>&1 &
  gtk_pid=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    if [[ -S /run/crd-for-linux/gtk.sock ]]; then
      return 0
    fi
    sleep 0.1
  done
  log "gtk ekranı kalkmadı"
  return 1
}

compositor_socket() {
  local runtime="$1" wl="$2"
  local path="${runtime}/${wl}"
  [[ -S "${path}" ]] || return 1
  if [[ "${runtime}" != "${HOST_RUNTIME}" ]]; then
    setfacl -m "u:${HOST_USER}:x" "${runtime}" || log "dizin izni verilemedi"
    setfacl -m "u:${HOST_USER}:rw" "${path}" || log "soket izni verilemedi"
    if [[ -S "${runtime}/bus" ]]; then
      setfacl -m "u:${HOST_USER}:rw" "${runtime}/bus" || log "veri yolu izni verilemedi"
    fi
  fi
  printf '%s\n' "${path}"
}

publish_ekran() {
  printf '%s\n' "$1" > /run/crd-for-linux/ekran
  chmod 0644 /run/crd-for-linux/ekran
}

start_stack() {
  local user="$1" uid="$2" runtime="$3" dbus="$4" wl="$5" sid="$6"
  local json pulse="" pw_dir="" abs_wl="" proxy_dbus="" display=""

  json="$(host_json)" || {
    log "host json yok"
    return 1
  }
  abs_wl="$(compositor_socket "${runtime}" "${wl}")" || {
    log "wayland soketi yok (${user})"
    return 1
  }
  publish_ekran "${abs_wl}"
  if [[ ! -d ${HOST_RUNTIME} ]]; then
    install -d -m 0700 -o "${HOST_USER}" -g "${HOST_USER}" ${HOST_RUNTIME}
  fi
  open_share "${HOST_USER}"
  ensure_pipewire "${HOST_USER}" "${HOST_RUNTIME}" || true
  ensure_gtk || return 1

  if [[ -S ${HOST_RUNTIME}/bus ]]; then
    proxy_dbus="unix:path=${HOST_RUNTIME}/bus"
  else
    proxy_dbus="${dbus}"
  fi

  local -a proxy_env=(
    "XDG_RUNTIME_DIR=${HOST_RUNTIME}"
    "DBUS_SESSION_BUS_ADDRESS=${proxy_dbus}"
    "WAYLAND_DISPLAY=${abs_wl}"
    "XDG_SESSION_TYPE=wayland"
    "XDG_CURRENT_DESKTOP=Hyprland"
    "XDG_SEAT=seat0"
    "HOME=${HOST_HOME}"
    "USER=${HOST_USER}"
    "LOGNAME=${HOST_USER}"
    "CRD_PROXY_STATE=${STATE}"
    "CRD_PROXY_SHARE=1"
    "PIPEWIRE_RUNTIME_DIR=${HOST_RUNTIME}"
    "PIPEWIRE_REMOTE=pipewire-0"
  )
  # Karşılama soketi portal kullanıcısına ait değildir. XAUTHORITY
  # yoktur; o DISPLAY giris_init'i düşürür. Yalnız masaüstü kullanıcısının
  # kendi soketi verilir. Boş değer, devralınan DISPLAY'i de kapatır.
  if [[ "${user}" == "${HOST_USER}" ]]; then
    display="$(display_of "${HOST_USER}" || true)"
  fi
  if [[ -n "${display}" ]]; then
    proxy_env+=("DISPLAY=${display}")
  else
    proxy_env+=("DISPLAY=")
  fi
  if [[ "${user}" == "${HOST_USER}" ]]; then
    proxy_env+=("XDG_SESSION_ID=${sid}")
  fi
  if ! runuser -u "${HOST_USER}" -- env "${proxy_env[@]}" \
    "${PROXY}" --start >>"${LOG}" 2>&1; then
    log "portal kalkmadı (${user})"
    return 1
  fi
  relax_sockets
  if [[ ! -S "${STATE}/bus" ]]; then
    log "yüz soketi yok"
    return 1
  fi

  if [[ -S ${HOST_RUNTIME}/pipewire-0 ]]; then
    pw_dir="${HOST_RUNTIME}"
  fi
  if [[ -n "${pw_dir}" && -S "${pw_dir}/pulse/native" ]]; then
    pulse="${pw_dir}/pulse"
  fi

  # gtk_init gerçek oturum soketine bağlanırsa o soket kapanınca host ölür
  # ve tabletteki oturum düşer. Boş ekran süreç boyunca açık kalır.
  local -a env_args=(
    "HOME=${HOST_HOME}"
    "USER=${HOST_USER}"
    "LOGNAME=${HOST_USER}"
    "PATH=/usr/bin:/bin"
    "LANG=C.UTF-8"
    "XDG_RUNTIME_DIR=${HOST_RUNTIME}"
    "XDG_SESSION_TYPE=wayland"
    "XDG_CURRENT_DESKTOP=Hyprland"
    "XDG_SEAT=seat0"
    "DBUS_SESSION_BUS_ADDRESS=unix:path=${STATE}/bus"
    "CHROME_REMOTE_DESKTOP_SESSION=1"
    "PIPEWIRE_REMOTE=pipewire-0"
    "WAYLAND_DISPLAY=/run/crd-for-linux/gtk.sock"
  )
  if [[ "${user}" == "${HOST_USER}" ]]; then
    env_args+=("XDG_SESSION_ID=${sid}")
  fi
  if [[ -n "${pw_dir}" && -x "${SES}" ]]; then
    runuser -u "${HOST_USER}" -- env XDG_RUNTIME_DIR="${pw_dir}" \
      "${SES}" --start >>"${LOG}" 2>&1 || true
    env_args+=("CHROME_REMOTE_DESKTOP_AUDIO_PIPE=${pw_dir}/crd-for-linux-audio/fifo")
  fi
  if [[ -n "${pw_dir}" ]]; then
    env_args+=("PIPEWIRE_RUNTIME_DIR=${pw_dir}")
  fi
  if [[ -n "${pulse}" ]]; then
    env_args+=("PULSE_RUNTIME_PATH=${pulse}")
  fi

  # stdbuf satır satır yazar; süreç düşünce çıkış kodu koltuk günlüğüne düşer.
  (
    stdbuf -oL -eL runuser -u "${HOST_USER}" -- env "${env_args[@]}" \
      "${HOST_BIN}" --host-config=- \
      <"${json}" >>/run/crd-for-linux/host.log 2>&1
    status=$?
    printf '%s host çıktı %s\n' "$(date -Iseconds)" "${status}" >>"${LOG}"
  ) &
  host_pid=$!
  stack_user="${HOST_USER}"
  stack_runtime="${HOST_RUNTIME}"
  stack_dbus="${proxy_dbus}"
  log "oturum ${sid} kullanıcı ${user} wayland ${wl} host ${host_pid}"
  return 0
}

read_seat() {
  local sid uid pid wl
  sid="$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)"
  [[ -n "${sid}" ]] || return 1
  uid="$(loginctl show-session "${sid}" -p User --value 2>/dev/null || true)"
  seat_user="$(loginctl show-session "${sid}" -p Name --value 2>/dev/null || true)"
  [[ -n "${uid}" && -n "${seat_user}" ]] || return 1
  seat_runtime="/run/user/${uid}"
  [[ -d "${seat_runtime}" ]] || return 1
  pid="$(compositor_pid "${uid}")"
  [[ -n "${pid}" ]] || return 1
  wl="$(wayland_name "${uid}" "${pid}")" || return 1
  if [[ ! -S "${seat_runtime}/bus" ]]; then
    runuser -u "${seat_user}" -- env XDG_RUNTIME_DIR="${seat_runtime}" \
      dbus-daemon --session --fork --nosyslog >>"${LOG}" 2>&1 || true
    sleep 0.3
  fi
  [[ -S "${seat_runtime}/bus" ]] || return 1
  seat_sid="${sid}"
  seat_uid="${uid}"
  seat_wl="${wl}"
  seat_dbus="unix:path=${seat_runtime}/bus"
  return 0
}

on_exit() {
  stop_stack
  if [[ -n "${gtk_pid}" ]] && kill -0 "${gtk_pid}" 2>/dev/null; then
    kill "${gtk_pid}" 2>/dev/null || true
  fi
  rm -f /run/crd-for-linux/gtk.sock
}
trap on_exit EXIT

mkdir -p /run/crd-for-linux
: >"${LOG}"
log "koltuk izleyici başladı"
stop_foreign_host

seat_user="" seat_uid="" seat_runtime="" seat_wl="" seat_sid="" seat_dbus=""

while true; do
  if read_seat; then
    key="${seat_uid}:${seat_wl}:${seat_sid}"
    host_up=0
    if [[ -n "${host_pid}" ]] && kill -0 "${host_pid}" 2>/dev/null; then
      host_up=1
    fi
    if [[ "${key}" != "${current_key}" && "${host_up}" == "1" ]]; then
      abs="$(compositor_socket "${seat_runtime}" "${seat_wl}" || true)"
      if [[ -n "${abs}" ]]; then
        publish_ekran "${abs}"
        log "kesintisiz ${current_key} -> ${key}"
        current_key="${key}"
        if [[ ! -p ${HOST_RUNTIME}/crd-for-linux-audio/fifo && -x "${SES}" ]]; then
          runuser -u "${HOST_USER}" -- env XDG_RUNTIME_DIR=${HOST_RUNTIME} \
            "${SES}" --start >>"${LOG}" 2>&1 || true
        fi
      fi
    elif [[ "${host_up}" == "0" ]]; then
      # Aynı koltukta host düşünce iki saniyede bir yeniden doğmasın.
      if [[ "${key}" == "${current_key}" && "${host_born}" != "0" ]]; then
        now="$(date +%s)"
        wait_s="$(( 8 - (now - host_born) ))"
        if (( wait_s > 0 )); then
          sleep "${wait_s}"
        fi
      fi
      log "geçiş ${current_key:-yok} -> ${key}"
      stop_stack
      if start_stack "${seat_user}" "${seat_uid}" "${seat_runtime}" "${seat_dbus}" "${seat_wl}" "${seat_sid}"; then
        current_key="${key}"
        host_born="$(date +%s)"
      else
        current_key=""
        stop_stack
      fi
    fi
  fi
  sleep 2
done
