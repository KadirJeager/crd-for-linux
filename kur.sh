#!/usr/bin/env bash
# CRD for Linux kurulumu / installer.
#   ./kur.sh denetle | check      bağımlılık ve oturum denetimi, hiçbir şey değiştirmez
#   ./kur.sh kur     | install    Google paketini indirir ve açar, derler, birimi kurar
#   ./kur.sh kaldir  | uninstall  birimi kaldırır, indirilen paketi siler
# Seçenekler: --deneme | --dry-run (yalnız yazar),
#             --deneysel | --experimental (test edilmemiş ortamda devam).
# Mesajlar sistem dili Türkçeyse Türkçe, değilse İngilizce.
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
EVET=0

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in
  tr*) TR=1 ;;
  *) TR=0 ;;
esac

# m "Türkçe" "English": sistem diline göre birini basar.
m() { if ((TR)); then printf '%s' "$1"; else printf '%s' "$2"; fi; }
hata() { echo "kur.sh: $*" >&2; exit 1; }
bilgi() { echo "• $*"; }

# Deneme kipinde komutu yazar, değilse çalıştırır.
yap() {
  if ((DENEME)); then
    printf '  [%s] %q' "$(m deneme dry-run)" "$1"; shift; printf ' %q' "$@"; echo
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
      echo "$(m "UYARI: ${XDG_CURRENT_DESKTOP:-bilinmeyen ortam} bu projede test edilmedi." "WARNING: ${XDG_CURRENT_DESKTOP:-unknown environment} is untested in this project.")" >&2
      if ((!DENEYSEL)); then
        hata "$(m "devam etmek için --deneysel verin; sonucu bir issue ile bildirin" "pass --experimental to continue, and please report the result in an issue")"
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

# Dağıtımın kurulum komutu, her kelime ayrı satırda. Tanınmazsa boş.
paket_komutu() {
  case "$(dagitim)" in
    arch) printf '%s\n' sudo pacman -S --needed base-devel wayland wayland-protocols pipewire libei libxkbcommon libxkbcommon-x11 libx11 libxcb glib2 xdg-dbus-proxy acl python curl libarchive ;;
    debian) printf '%s\n' sudo apt-get install -y build-essential pkg-config libwayland-dev wayland-protocols libpipewire-0.3-dev libei-dev libeis-dev libxkbcommon-dev libxkbcommon-x11-dev libx11-dev libx11-xcb-dev libxcb1-dev libxcb-xkb-dev libglib2.0-dev xdg-dbus-proxy acl pipewire-bin python3 curl libarchive-tools ;;
    fedora) printf '%s\n' sudo dnf install -y gcc make pkgconf wayland-devel wayland-protocols-devel pipewire-devel libei-devel libxkbcommon-devel libxkbcommon-x11-devel libX11-devel libxcb-devel glib2-devel xdg-dbus-proxy acl pipewire-utils python3 curl bsdtar ;;
  esac
}

paket_onerisi() {
  local komut
  komut="$(paket_komutu | tr '\n' ' ')"
  if [[ -n "${komut}" ]]; then
    echo "${komut}"
  else
    m "dağıtım tanınmadı; eksikleri kendi paket yöneticinizle kurun" "distribution not recognized; install the missing pieces with your package manager"; echo
  fi
}

# /dev/tty gerçekten açılabiliyorsa 0 (boru ve servis içinde açılmayabilir).
terminal_var() { { : </dev/tty; } 2>/dev/null; }

# Terminalden evet/hayır sorar; boru ile çalışırken de /dev/tty'den okur.
# --evet verilmişse sormaz. Terminal yoksa hayır sayılır.
sor() {
  local cevap=""
  ((EVET)) && return 0
  terminal_var || return 1
  printf '%s [e/H] ' "$1" >/dev/tty
  read -r cevap </dev/tty || return 1
  [[ "${cevap,,}" == e* || "${cevap,,}" == y* ]]
}

# Denetim geçmezse dağıtımın paketlerini sorarak kurar, sonra yeniden denetler.
paketleri_hazirla() {
  local -a komut=()
  denetle && return 0
  mapfile -t komut < <(paket_komutu)
  ((${#komut[@]})) || hata "$(m "eksik paketleri kurun ve yeniden çalıştırın" "install the missing packages and run again")"
  if sor "$(m "Eksik paketler şu komutla kurulsun mu?" "Install the missing packages with this command?") ${komut[*]}"; then
    yap "${komut[@]}"
    ((DENEME)) || denetle || hata "$(m "paketler kuruldu ama denetim yine geçmedi" "packages installed but the check still fails")"
  else
    hata "$(m "eksik paketler kurulmadı" "missing packages were not installed")"
  fi
}

denetle() {
  local eksik=0 m k o
  o="$(ortam)"
  bilgi "$(m ortam environment): ${XDG_CURRENT_DESKTOP:-?} → ${o}"
  [[ "${XDG_SESSION_TYPE:-}" == wayland ]] || { echo "  $(m "Wayland oturumu değil" "not a Wayland session") (XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-})"; eksik=1; }
  [[ "$(uname -m)" == x86_64 ]] || { echo "  $(m "Google'ın paketi yalnız x86_64 için var" "Google's package exists only for x86_64") ($(uname -m))"; eksik=1; }
  for k in "${KOMUTLAR[@]}"; do
    command -v "${k}" >/dev/null || { echo "  $(m "komut yok" "missing command"): ${k}"; eksik=1; }
  done
  if command -v pkg-config >/dev/null; then
    for m in "${PC_MODULLER[@]}"; do
      pkg-config --exists "${m}" || { echo "  $(m "geliştirme paketi yok" "missing development package"): ${m}"; eksik=1; }
    done
  fi
  if [[ -x "${HOST_BIN}" ]]; then
    local yok
    yok="$(ldd "${HOST_BIN}" 2>/dev/null | awk '/not found/ {print $1}' | tr '\n' ' ')"
    [[ -z "${yok}" ]] || { echo "  $(m "host'un eksik kütüphaneleri" "libraries missing for the host"): ${yok}"; eksik=1; }
  fi
  if ((eksik)); then
    echo "$(m Öneri Suggestion): $(paket_onerisi)"
    return 1
  fi
  bilgi "$(m "denetim geçti" "check passed")"
}

# --- kurulum ---------------------------------------------------------------

paketi_ac() {
  local deb="${CACHE}/chrome-remote-desktop_current_amd64.deb"
  yap mkdir -p "${CACHE}"
  bilgi "$(m "Google'ın resmî paketi indiriliyor" "downloading Google's official package"): ${DEB_URL}"
  yap curl -fL --retry 3 --max-time 600 -o "${deb}.part" "${DEB_URL}"
  yap mv -f "${deb}.part" "${deb}"
  if ((!DENEME)); then
    local surum
    surum="$(bsdtar -xOf "${deb}" 'control.tar.*' | bsdtar -xOf - ./control | awk -F': ' '/^Version:/ {print $2}')"
    [[ -n "${surum}" ]] || hata "$(m "paketin sürümü okunamadı; dosya bozuk olabilir" "could not read the package version; the file may be corrupt")"
    bilgi "$(m sürüm version) ${surum}, sha256 $(sha256sum "${deb}" | cut -d' ' -f1)"
  fi
  # Paket sisteme kurulmaz; yalnız depo içine açılır.
  yap rm -rf "${UNPACKED}"
  yap mkdir -p "${UNPACKED}"
  if ((DENEME)); then
    echo "  [$(m deneme dry-run)] bsdtar -xOf ${deb} 'data.tar.*' | bsdtar -xf - -C ${UNPACKED}"
  else
    bsdtar -xOf "${deb}" 'data.tar.*' | bsdtar -xf - -C "${UNPACKED}"
    [[ -x "${HOST_BIN}" ]] || hata "$(m "host ikilisi paketten çıkmadı" "host binary not found in the package"): ${HOST_BIN}"
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
  bilgi "$(m "sistem birimi" "system unit"): ${SYS_UNIT}"
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
    bilgi "$(m "birim zaten çalışıyor; yeni ayar bir sonraki açılışta ya da 'sudo systemctl restart crd-koltuk' ile gelir (bağlı istemci düşer)" "unit already running; the new settings apply at next boot or with 'sudo systemctl restart crd-koltuk' (a connected client drops)")"
  else
    yap sudo systemctl start crd-koltuk.service
  fi
}

birim_kur_gnome() {
  bilgi "$(m "kullanıcı birimi" "user unit"): ${USER_UNIT}"
  yap mkdir -p "$(dirname "${USER_UNIT}")"
  if ((DENEME)); then
    birim_uret "${ROOT}/src/gnome/crd-gnome.service.in" | sed 's/^/    /'
  else
    birim_uret "${ROOT}/src/gnome/crd-gnome.service.in" >"${USER_UNIT}"
  fi
  yap systemctl --user daemon-reload
  yap systemctl --user enable --now crd-gnome.service
}

# Google'ın headless sayfasındaki komuttan --code ve --redirect-url alınır;
# start-host bu depodaki ikiliyle ve --no-start ile çalışır, böylece Google'ın
# sanal X açan daemon'ı kurulmaz. PIN'i start-host kendisi sorar. Kod hiçbir
# yere yazılmaz.
kayit_adimi() {
  shopt -s nullglob
  local kayit=("${HOME}/.config/chrome-remote-desktop/"host#*.json)
  shopt -u nullglob
  if ((${#kayit[@]})); then
    bilgi "$(m "host kaydı var" "host is registered"): ${kayit[0]}"
    return
  fi
  if ((DENEME)) || ! terminal_var; then
    kayit_elle
    return
  fi
  local satir="" kod="" yonlendirme="" ad
  if ((TR)); then
    printf '%s\n' "• Host'u Google hesabınıza kaydedin:" \
      "  1. https://remotedesktop.google.com/headless adresini açın: Begin › Next › Authorize." \
      "  2. \"Debian Linux\" komutunu kopyalayıp aşağıya yapıştırın (görünmez), Enter'a basın." >/dev/tty
  else
    printf '%s\n' "• Register the host with your Google account:" \
      "  1. Open https://remotedesktop.google.com/headless: Begin › Next › Authorize." \
      "  2. Copy the \"Debian Linux\" command and paste it below (hidden), then press Enter." >/dev/tty
  fi
  printf '> ' >/dev/tty
  read -rs satir </dev/tty || true
  echo >/dev/tty
  kod="$(sed -n 's/.*--code="\([^"]*\)".*/\1/p' <<<"${satir}")"
  yonlendirme="$(sed -n 's/.*--redirect-url="\([^"]*\)".*/\1/p' <<<"${satir}")"
  satir=""
  if [[ -z "${kod}" || -z "${yonlendirme}" ]]; then
    echo "$(m "komutta --code ya da --redirect-url bulunamadı" "--code or --redirect-url not found in the command")" >&2
    kayit_elle
    return
  fi
  ad="$(hostname)"
  bilgi "$(m "kaydediliyor; PIN'i start-host soracak" "registering; start-host will ask for the PIN")"
  DISPLAY= "${CRD_DIR}/start-host" --code="${kod}" --redirect-url="${yonlendirme}" \
    --name="${ad}" --no-start </dev/tty ||
    hata "$(m "kayıt başarısız (kod tek kullanımlık; sayfadan yenisini alın)" "registration failed (the code is single-use; get a new one from the page)")"
  kod=""
  shopt -s nullglob
  kayit=("${HOME}/.config/chrome-remote-desktop/"host#*.json)
  shopt -u nullglob
  ((${#kayit[@]})) || hata "$(m "start-host bitti ama kayıt dosyası yok" "start-host finished but wrote no registration file")"
  bilgi "$(m "host kaydedildi" "host registered"): ${kayit[0]}"
  if [[ "$(ortam)" == gnome ]]; then
    systemctl --user restart crd-gnome.service
  else
    sudo systemctl restart crd-koltuk.service
  fi
}

# Terminal yoksa ya da yapıştırılan komut okunamazsa elle kayıt adımları.
kayit_elle() {
  if ((TR)); then
    printf '%s\n' "• Host henüz Google hesabınıza kayıtlı değil. Bir kez:" \
      "  1. https://remotedesktop.google.com/headless adresinde \"Debian Linux\" komutunu kopyalayın." \
      "  2. Komuttaki ikili yolunu şununla değiştirip çalıştırın:" \
      "       DISPLAY= ${CRD_DIR}/start-host ... --no-start" \
      "     (--code, --redirect-url ve --name aynı kalır; PIN'i komut sorar.)" \
      "  Sonra: sudo systemctl restart crd-koltuk" \
      "  Kod ve PIN hiçbir günlüğe yazılmaz."
  else
    printf '%s\n' "• The host is not registered with your Google account yet. Once:" \
      "  1. Copy the \"Debian Linux\" command from https://remotedesktop.google.com/headless" \
      "  2. Replace the binary path in it with this one and run it:" \
      "       DISPLAY= ${CRD_DIR}/start-host ... --no-start" \
      "     (--code, --redirect-url and --name stay the same; the command asks for the PIN.)" \
      "  Then: sudo systemctl restart crd-koltuk" \
      "  The code and PIN are never written to any log."
  fi
}

kur() {
  local o
  o="$(ortam)"
  ortam_kapisi "${o}"
  paketleri_hazirla
  paketi_ac
  bilgi "$(m derleniyor building)"
  yap make -C "${ROOT}" -j"$(nproc)"
  if ((!DENEME)); then
    denetle || hata "$(m "derleme sonrası denetim geçmedi" "check after build failed")"
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
  bilgi "$(m "host kaydı (~/.config/chrome-remote-desktop) silinmedi; istemiyorsanız kendiniz silin" "host registration (~/.config/chrome-remote-desktop) was kept; delete it yourself if you want")"
}

main() {
  local komut="${1:-}"
  [[ -n "${komut}" ]] || hata "$(m kullanım usage): $0 check|install|uninstall [--dry-run] [--experimental] [--yes]  (denetle|kur|kaldir [--deneme] [--deneysel] [--evet])"
  shift
  local a
  for a in "$@"; do
    case "${a}" in
      --deneme|--dry-run) DENEME=1 ;;
      --deneysel|--experimental) DENEYSEL=1 ;;
      --evet|--yes) EVET=1 ;;
      *) hata "$(m "bilinmeyen seçenek" "unknown option"): ${a}" ;;
    esac
  done
  ((EUID != 0)) || hata "$(m "root olarak değil, ekranı paylaşılacak kullanıcı olarak çalıştırın; sudo gereken yerde sorar" "run as the user whose screen is shared, not as root; sudo is asked where needed")"
  case "${komut}" in
    denetle|check) denetle ;;
    kur|install) kur ;;
    kaldir|uninstall) kaldir ;;
    *) hata "$(m "bilinmeyen komut" "unknown command"): ${komut}" ;;
  esac
}

main "$@"
