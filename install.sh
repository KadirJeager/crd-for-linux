#!/usr/bin/env bash
# CRD for Linux: one-line installer / tek komutluk kurulum.
#
#   curl -fsSL https://raw.githubusercontent.com/KadirJeager/crd-for-linux/main/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/KadirJeager/crd-for-linux/main/install.sh | bash -s -- --experimental
#
# Fetches (or updates) the project into ~/.local/share/crd-for-linux and runs
# "kur.sh install" with the given options. Run it again to update.
# Projeyi ~/.local/share/crd-for-linux altına indirir ya da günceller ve
# "kur.sh install" çalıştırır. Güncellemek için yeniden çalıştırın.
set -euo pipefail

readonly REPO="https://github.com/KadirJeager/crd-for-linux"
readonly DIR="${CRD_FOR_LINUX_DIR:-${XDG_DATA_HOME:-${HOME}/.local/share}/crd-for-linux}"

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in
  tr*) TR=1 ;;
  *) TR=0 ;;
esac
m() { if ((TR)); then printf '%s' "$1"; else printf '%s' "$2"; fi; }
hata() { echo "install.sh: $*" >&2; exit 1; }

((EUID != 0)) || hata "$(m "root olarak değil, ekranı paylaşılacak kullanıcı olarak çalıştırın" "run as the user whose screen is shared, not as root")"

# Bir klonun içinden çalıştırıldıysa o klonu kullan.
kaynak="${BASH_SOURCE[0]:-}"
if [[ -n "${kaynak}" && -f "${kaynak}" && -x "$(dirname "${kaynak}")/kur.sh" ]]; then
  exec "$(dirname "${kaynak}")/kur.sh" install "$@"
fi

if command -v git >/dev/null; then
  if [[ -d "${DIR}/.git" ]]; then
    echo "• $(m güncelleniyor updating): ${DIR}"
    git -C "${DIR}" pull --ff-only --quiet
  else
    [[ ! -e "${DIR}" ]] || hata "$(m "hedef var ama git klonu değil" "target exists but is not a git clone"): ${DIR}"
    echo "• $(m indiriliyor downloading): ${REPO} → ${DIR}"
    mkdir -p "$(dirname "${DIR}")"
    git clone --quiet --depth 1 "${REPO}.git" "${DIR}"
  fi
else
  command -v curl >/dev/null || hata "$(m "git ya da curl gerekli" "git or curl is required")"
  command -v tar >/dev/null || hata "$(m "tar gerekli" "tar is required")"
  echo "• $(m indiriliyor downloading): ${REPO} → ${DIR}"
  gecici="$(mktemp -d)"
  trap 'rm -rf "${gecici}"' EXIT
  curl -fsSL --retry 3 --max-time 120 "${REPO}/archive/refs/heads/main.tar.gz" |
    tar -xz -C "${gecici}" --strip-components=1
  # Önceki kurulumun indirdiği Google paketi ve derleme çıktıları korunur.
  mkdir -p "${DIR}"
  cp -a "${gecici}/." "${DIR}/"
fi

exec "${DIR}/kur.sh" install "$@"
