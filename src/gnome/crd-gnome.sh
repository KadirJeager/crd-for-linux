#!/usr/bin/env bash
# GNOME oturumunda host'u oturum veri yoluyla başlatır. Test edilmedi.
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly HOST_BIN="${ROOT}/third_party/unpacked/opt/google/chrome-remote-desktop/chrome-remote-desktop-host"

if [[ "${XDG_CURRENT_DESKTOP:-}" != *GNOME* ]]; then
  echo "crd-gnome: GNOME oturumu değil (XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-boş})" >&2
  exit 2
fi
if [[ -z "${WAYLAND_DISPLAY:-}" || -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
  echo "crd-gnome: WAYLAND_DISPLAY ya da DBUS_SESSION_BUS_ADDRESS kullanıcı yöneticisinde yok" >&2
  exit 2
fi
shopt -s nullglob
configs=("${HOME}/.config/chrome-remote-desktop/"host#*.json)
shopt -u nullglob
if ((${#configs[@]} == 0)); then
  echo "crd-gnome: host kaydı yok; önce kur.sh'nin yazdığı kayıt adımını yapın" >&2
  exit 2
fi
exec "${HOST_BIN}" --host-config=- <"${configs[0]}"
