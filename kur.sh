#!/usr/bin/env bash
# CRD for Linux kurulumu.
#   ./kur.sh denetle   bağımlılık ve oturum denetimi, hiçbir şey değiştirmez
#   ./kur.sh kur       Google paketini indirir ve açar, derler, birimi kurar
#   ./kur.sh kaldir    birimi kaldırır, indirilen paketi siler
# Seçenekler: --deneme (yalnız yazar), --deneysel (test edilmemiş ortamda devam).
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly UNPACKED="${ROOT}/third_party/unpacked"
readonly CRD_DIR="${UNPACKED}/opt/google/chrome-remote-desktop"
readonly HOST_BIN="${CRD_DIR}/chrome-remote-desktop-host"
readonly DEB_URL="https://dl.google.com/linux/direct/chrome-remote-desktop_current_amd64.deb"
readonly CACHE="${XDG_CACHE_HOME:-${HOME}/.cache}/crd-for-linux"
readonly SYS_UNIT="/etc/systemd/system/crd-koltuk.service"
readonly USER_UNIT="${XDG_CONFIG_HOME:-${HOME}/.config}/systemd/user/crd-gnome.service"

DENEME=0
DENEYSEL=0

hata() { echo "kur.sh: $*" >&2; exit 1; }
bilgi() { echo "• $*"; }

# Deneme kipinde komutu yazar, değilse çalıştırır.
yap() {
  if ((DENEME)); then
    printf '  [deneme] %q' "$1"; shift; printf ' %q' "$@"; echo
  else
    "$@"
  fi
}

# --- ortam -----------------------------------------------------------------

# Çıktı: hyprland | wlroots | kde | gnome | bilinmiyor
ortam() {
  local d="${XDG_CURRENT_DESKTOP:-}"
  case "${d,,}" in
    *hyprland*) echo hyprland ;;
    *gnome*) echo gnome ;;
    *kde*) echo kde ;;
    *sway*|*river*|*labwc*|*wayfire*|*niri*) echo wlroots ;;
    *) echo bilinmiyor ;;
  esac
}

ortam_kapisi() {
  local o="$1"
  case "${o}" in
    hyprland) return 0 ;;
    wlroots|kde|gnome|bilinmiyor)
      echo "UYARI: ${XDG_CURRENT_DESKTOP:-bilinmeyen ortam} bu projede test edilmedi." >&2
      if ((!DENEYSEL)); then
        hata "devam etmek için --deneysel verin; sonucu bir issue ile bildirin"
      fi ;;
  esac
}

# --- denetim ---------------------------------------------------------------

readonly PC_MODULLER=(wayland-client wayland-server wayland-protocols libpipewire-0.3
  libeis-1.0 libei-1.0 xkbcommon xkbcommon-x11 x11 x11-xcb xcb xcb-xkb gio-2.0)
readonly KOMUTLAR=(cc make pkg-config wayland-scanner xdg-dbus-proxy setfacl pw-cat
  python3 curl bsdtar runuser stdbuf)

dagitim() {
  local id="" like=""
  if [[ -r /etc/os-release ]]; then
    id="$(. /etc/os-release; echo "${ID:-}")"
    like="$(. /etc/os-release; echo "${ID_LIKE:-}")"
  fi
  case " ${id} ${like} " in
    *" arch "*) echo arch ;;
    *" debian "*|*" ubuntu "*) echo debian ;;
    *" fedora "*|*" rhel "*) echo fedora ;;
    *) echo bilinmiyor ;;
  esac
}

paket_onerisi() {
  case "$(dagitim)" in
    arch) echo "sudo pacman -S --needed base-devel wayland wayland-protocols pipewire libei libxkbcommon libxkbcommon-x11 libx11 libxcb glib2 xdg-dbus-proxy acl python curl libarchive" ;;
    debian) echo "sudo apt install build-essential pkg-config libwayland-dev wayland-protocols libpipewire-0.3-dev libei-dev libeis-dev libxkbcommon-dev libxkbcommon-x11-dev libx11-dev libx11-xcb-dev libxcb1-dev libxcb-xkb-dev libglib2.0-dev xdg-dbus-proxy acl pipewire-bin python3 curl libarchive-tools" ;;
    fedora) echo "sudo dnf install gcc make pkgconf wayland-devel wayland-protocols-devel pipewire-devel libei-devel libxkbcommon-devel libxkbcommon-x11-devel libX11-devel libxcb-devel glib2-devel xdg-dbus-proxy acl pipewire-utils python3 curl bsdtar" ;;
    *) echo "dağıtım tanınmadı; eksikleri kendi paket yöneticinizle kurun" ;;
  esac
}

denetle() {
  local eksik=0 m k o
  o="$(ortam)"
  bilgi "ortam: ${XDG_CURRENT_DESKTOP:-boş} → ${o}"
  [[ "${XDG_SESSION_TYPE:-}" == wayland ]] || { echo "  Wayland oturumu değil (XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-boş})"; eksik=1; }
  [[ "$(uname -m)" == x86_64 ]] || { echo "  Google'ın paketi yalnız x86_64 için var (bu makine $(uname -m))"; eksik=1; }
  for k in "${KOMUTLAR[@]}"; do
    command -v "${k}" >/dev/null || { echo "  komut yok: ${k}"; eksik=1; }
  done
  if command -v pkg-config >/dev/null; then
    for m in "${PC_MODULLER[@]}"; do
      pkg-config --exists "${m}" || { echo "  geliştirme paketi yok: ${m}"; eksik=1; }
    done
  fi
  if [[ -x "${HOST_BIN}" ]]; then
    local yok
    yok="$(ldd "${HOST_BIN}" 2>/dev/null | awk '/not found/ {print $1}' | tr '\n' ' ')"
    [[ -z "${yok}" ]] || { echo "  host'un eksik kütüphaneleri: ${yok}"; eksik=1; }
  fi
  if ((eksik)); then
    echo "Öneri: $(paket_onerisi)"
    return 1
  fi
  bilgi "denetim geçti"
}

# --- kurulum ---------------------------------------------------------------

paketi_ac() {
  local deb="${CACHE}/chrome-remote-desktop_current_amd64.deb"
  yap mkdir -p "${CACHE}"
  bilgi "Google'ın resmî paketi indiriliyor: ${DEB_URL}"
  yap curl -fL --retry 3 --max-time 600 -o "${deb}.part" "${DEB_URL}"
  yap mv -f "${deb}.part" "${deb}"
  if ((!DENEME)); then
    local surum
    surum="$(bsdtar -xOf "${deb}" 'control.tar.*' | bsdtar -xOf - ./control | awk -F': ' '/^Version:/ {print $2}')"
    [[ -n "${surum}" ]] || hata "paketin sürümü okunamadı; dosya bozuk olabilir"
    bilgi "sürüm ${surum}, sha256 $(sha256sum "${deb}" | cut -d' ' -f1)"
  fi
  # Paket sisteme kurulmaz; yalnız depo içine açılır.
  yap rm -rf "${UNPACKED}"
  yap mkdir -p "${UNPACKED}"
  if ((DENEME)); then
    echo "  [deneme] bsdtar -xOf ${deb} 'data.tar.*' | bsdtar -xf - -C ${UNPACKED}"
  else
    bsdtar -xOf "${deb}" 'data.tar.*' | bsdtar -xf - -C "${UNPACKED}"
    [[ -x "${HOST_BIN}" ]] || hata "host ikilisi paketten çıkmadı: ${HOST_BIN}"
  fi
}

birim_uret() {
  local sablon="$1"
  sed -e "s|@ROOT@|${ROOT}|g" -e "s|@USER@|$(id -un)|g" -e "s|@UID@|$(id -u)|g" "${sablon}"
}

birim_kur_sistem() {
  local gecici
  gecici="$(mktemp)"
  birim_uret "${ROOT}/src/oturum/crd-koltuk.service.in" >"${gecici}"
  bilgi "sistem birimi: ${SYS_UNIT}"
  ((DENEME)) && sed 's/^/    /' "${gecici}"
  yap sudo install -m 0644 "${gecici}" "${SYS_UNIT}"
  rm -f "${gecici}"
  # Eski elle kurulumdan kalan kullanıcı drop-in'i birime taşındı.
  if [[ -e /etc/systemd/system/crd-koltuk.service.d/kullanici.conf ]]; then
    yap sudo rm -f /etc/systemd/system/crd-koltuk.service.d/kullanici.conf
    yap sudo rmdir --ignore-fail-on-non-empty /etc/systemd/system/crd-koltuk.service.d
  fi
  yap sudo systemctl daemon-reload
  yap sudo systemctl enable crd-koltuk.service
  if systemctl is-active --quiet crd-koltuk.service; then
    bilgi "birim zaten çalışıyor; yeni ayar bir sonraki açılışta ya da 'sudo systemctl restart crd-koltuk' ile gelir (bağlı istemci düşer)"
  else
    yap sudo systemctl start crd-koltuk.service
  fi
}

birim_kur_gnome() {
  bilgi "kullanıcı birimi: ${USER_UNIT}"
  yap mkdir -p "$(dirname "${USER_UNIT}")"
  if ((DENEME)); then
    birim_uret "${ROOT}/src/gnome/crd-gnome.service.in" | sed 's/^/    /'
  else
    birim_uret "${ROOT}/src/gnome/crd-gnome.service.in" >"${USER_UNIT}"
  fi
  yap systemctl --user daemon-reload
  yap systemctl --user enable --now crd-gnome.service
}

kayit_adimi() {
  shopt -s nullglob
  local kayit=("${HOME}/.config/chrome-remote-desktop/"host#*.json)
  shopt -u nullglob
  if ((${#kayit[@]})); then
    bilgi "host kaydı var: ${kayit[0]}"
    return
  fi
  cat <<EOF
• Host henüz Google hesabınıza kayıtlı değil. Bir kez:
  1. https://remotedesktop.google.com/headless adresinde "Debian Linux" komutunu kopyalayın.
  2. Komuttaki ikili yolunu şununla değiştirip çalıştırın:
       ${CRD_DIR}/start-host
     (--code, --redirect-url ve --name kısmı aynı kalır; PIN'i komut sorar.)
  Kod ve PIN hiçbir günlüğe yazılmaz.
EOF
}

kur() {
  local o
  o="$(ortam)"
  ortam_kapisi "${o}"
  paketi_ac
  bilgi "derleniyor"
  yap make -C "${ROOT}" -j"$(nproc)"
  if ((!DENEME)); then
    denetle || hata "derleme sonrası denetim geçmedi"
  fi
  if [[ "${o}" == gnome ]]; then
    birim_kur_gnome
  else
    birim_kur_sistem
  fi
  kayit_adimi
}

kaldir() {
  if [[ -e "${SYS_UNIT}" ]]; then
    yap sudo systemctl disable --now crd-koltuk.service
    yap sudo rm -f "${SYS_UNIT}"
    yap sudo rm -rf /etc/systemd/system/crd-koltuk.service.d
    yap sudo systemctl daemon-reload
  fi
  if [[ -e "${USER_UNIT}" ]]; then
    yap systemctl --user disable --now crd-gnome.service
    yap rm -f "${USER_UNIT}"
    yap systemctl --user daemon-reload
  fi
  yap rm -rf "${CACHE}" "${UNPACKED}"
  yap make -C "${ROOT}" clean
  bilgi "host kaydı (~/.config/chrome-remote-desktop) silinmedi; istemiyorsanız kendiniz silin"
}

main() {
  local komut="${1:-}"
  [[ -n "${komut}" ]] || hata "kullanım: $0 denetle|kur|kaldir [--deneme] [--deneysel]"
  shift
  local a
  for a in "$@"; do
    case "${a}" in
      --deneme) DENEME=1 ;;
      --deneysel) DENEYSEL=1 ;;
      *) hata "bilinmeyen seçenek: ${a}" ;;
    esac
  done
  ((EUID != 0)) || hata "root olarak değil, ekranı paylaşılacak kullanıcı olarak çalıştırın; sudo gereken yerde sorar"
  case "${komut}" in
    denetle) denetle ;;
    kur) kur ;;
    kaldir) kaldir ;;
    *) hata "bilinmeyen komut: ${komut}" ;;
  esac
}

main "$@"
