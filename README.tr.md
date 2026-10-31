# CRD for Linux

[English](README.md) · **Türkçe**

**Chrome Remote Desktop'ı açık Wayland oturumunuza bağlar: sanal masaüstü yok, Xvfb yok.**

Google'ın Linux'taki Chrome Remote Desktop host'u normalde ayrı bir sanal X oturumu açar. Bu proje Google'ın değiştirilmemiş host'unu kullandığınız oturuma bağlar: web istemcisinden ya da telefondan, izin penceresi olmadan gerçek masaüstünüzü görür, kendi klavye düzeninizle yazar, panoyu paylaşır ve hoparlörünüzü duyarsınız. Kilit ekranında da çalışır: host karşılama ekranında başlar, şifrenizi uzaktan yazarsınız ve aynı bağlantı masaüstünüze devam eder.

## Durum

| Ortam | Durum |
|---|---|
| Hyprland 0.56 (CachyOS, greetd + dms-greeter) | **Çalışıyor.** Görüntü, fare, klavye (Türkçe dahil), iki yönlü pano, ses, kilit ekranından giriş ve kesintisiz geçiş doğrulandı. |
| Sway, river, labwc, niri, Wayfire (wlroots) | **Test edilmedi.** Aynı Wayland protokollerini kullandığı için çalışması beklenir. |
| KDE Plasma 6 (KWin) | **Yazıldı, test edilmedi.** Yakalama KWin'in kendi ekran yayınıyla (`zkde_screencast`), giriş `fake_input` ile, pano aynı. Karşılama ekranı (SDDM) ve girişte kesintisiz geçiş yok. |
| GNOME (Mutter) | **Test edilmedi.** CRD host'u GNOME'da Mutter'ın kendi uzak masaüstü arayüzünü kullanır; `kur.sh` onu açık oturumda başlatan bir kullanıcı birimi kurar. Karşılama ekranı (GDM) desteklenmez. |
| Birden çok monitör | Henüz yok: tek çıktı aktarılır (`CRD_OUTPUT` ile seçilir). |
| Dosya aktarımı | Henüz yok. |

Ortamlara göre ayrıntılar ve kaynaklar: [docs/DE-TASARIM.md](docs/DE-TASARIM.md).

Test edilmemiş bir ortamda denediyseniz sonucu (çalıştı ya da çalışmadı, günlüklerle) bir issue olarak yazın. En çok ihtiyaç duyulan katkı bu.

## Nasıl çalışır

CRD host'unun Linux sürümü, Wayland'de ekranı ve girişi xdg-desktop-portal üzerinden almayı zaten biliyor (PipeWire + libei/EIS). Eksik olan, izin penceresi açmadan ve karşılama ekranında da çalışan bir portal. Bu proje o portalı yazıyor ve host'a yalnız ona özel bir D-Bus üzerinden veriyor; masaüstünüzün kendi portalı değişmez.

```
chrome-remote-desktop-host (Google'ın değiştirilmemiş ikilisi)
        │  özel D-Bus (xdg-dbus-proxy)
        ▼
crd-portal ── RemoteDesktop / ScreenCast / Clipboard portal arayüzleri
   ├─ yakalama: ext-image-copy-capture → PipeWire düğümü
   ├─ giriş: EIS sunucusu → zwlr_virtual_pointer / zwp_virtual_keyboard
   ├─ pano: ext-data-control / wlr-data-control
   └─ ses: varsayılan hoparlörün monitörü → host'un ses borusu
        ▲
crd-koltuk (systemd sistem birimi)
   karşılama ekranı ↔ masaüstü geçişini izler; geçişte host'u öldürmez,
   yakalama/giriş/pano yeni kompozitöre yeniden bağlanır
```

| Klasör | İş |
|---|---|
| `src/oturum` | Açılış: `crd-koltuk.sh`, systemd birimi, gtk için boş Wayland soketi |
| `src/portal` | Portal sunucusu (`crd-portal`) |
| `src/capture` | Ekran yakalama ve PipeWire düğümü |
| `src/cikti` | Wayland çıktısını bulma ve seçme |
| `src/eis`, `src/giris`, `src/olay` | Uzak fare ve klavye |
| `src/clipboard` | Pano |
| `src/ses` | Ses |
| `src/proxy` | Host'a özel D-Bus |
| `src/netmon` | Portalın ağ izleme arayüzü |
| `src/gnome` | GNOME: host'u açık oturumda başlatır |
| `third_party/chromium-157` | Arayüzlerin karşılaştırıldığı Chromium kaynak dosyaları (BSD lisansı) |

## Kurulum

Tek komut; ekranı paylaşılacak kullanıcıyla çalıştırın (root değil):

```bash
curl -fsSL https://raw.githubusercontent.com/KadirJeager/crd-for-linux/main/install.sh | bash
```

Projeyi `~/.local/share/crd-for-linux` altına indirir, eksik paketleri sorarak kurar (Arch, Debian/Ubuntu, Fedora), Google'ın resmî CRD paketini makinenize indirip sisteme kurmadan açar, derler, servisi kurar ve en sonda host'u Google hesabınıza kaydeder: [remotedesktop.google.com/headless](https://remotedesktop.google.com/headless) sayfasında Begin › Next › Authorize deyin, "Debian Linux" komutunu kopyalayıp sorulduğunda yapıştırın. Komuttan yalnız kod kullanılır; Google'ın sanal masaüstü açan kendi servisi kurulmaz. PIN'i Google'ın aracı sorar ve hiçbir günlüğe yazılmaz.

Güncellemek için aynı komutu yeniden çalıştırın. Seçenekler `bash -s --` sonrasına yazılır:

```bash
curl -fsSL https://raw.githubusercontent.com/KadirJeager/crd-for-linux/main/install.sh | bash -s -- --deneysel
```

| Seçenek | Anlamı |
|---|---|
| `--deneysel` (`--experimental`) | Hyprland dışındaki (test edilmemiş) ortamlarda gerekir. |
| `--deneme` (`--dry-run`) | Yalnız ne yapılacağını yazar. |
| `--evet` (`--yes`) | Paket kurmadan önce sormaz. |

Bir klonun içinden `./kur.sh denetle`, `./kur.sh kur`, `./kur.sh kaldir` de kullanılabilir (İngilizce adlar: `check`, `install`, `uninstall`). Mesajlar sistem dili Türkçeyse Türkçe, değilse İngilizcedir.

Kaldırmak için:

```bash
~/.local/share/crd-for-linux/kur.sh kaldir
```

## Ayarlar

| Değişken | Anlamı |
|---|---|
| `CRD_OUTPUT` | Aktarılacak çıktının adı (ör. `HDMI-A-1`). Verilmezse tek çıktı, birden çoksa mantıksal 0,0'daki çıktı. |

Klavye düzeni kompozitörün etkin haritasından okunur; ayrıca ayar gerekmez.

## Bilinen sınırlar

- Tek monitör aktarılır.
- Host, ekranı paylaşılan kullanıcı adına çalışır; karşılama ekranında görüntü almak için karşılama kompozitörünün soketine ACL ile erişim verilir.
- Ekran boyutu bağlantı sırasında değişirse akış yeniden kurulur; bu yol test edilmedi.
- İstemcinin ekran klavyesi (Esc, F tuşları, Ctrl, Win, Alt olan panel) boş açılır. İstemci tuşları host'un gönderdiği klavye düzeninden çizer; CRD host'unun portal yolu düzen izleyicisini giriş oturumuna hiç bağlamaz (Chromium'da `PortalInteractionStrategy::CreateKeyboardLayoutMonitor`; GNOME yolu `SetKeyboardLayoutMonitor` çağırır), bu yüzden hep boş düzen gider. Sorun Google'ın host'unda, bu projede değil. Telefonun kendi klavyesiyle yazmak çalışır.

## Lisans

GPL-3.0-or-later; tam metin [LICENSE](LICENSE).

Başka kaynaklardan gelen dosyalar kendi lisanslarıyla kalır:

- `third_party/chromium-157`: Chromium kaynak dosyaları, BSD tarzı lisans (dosya başlıklarında).
- `src/capture/proto/zkde-screencast-unstable-v1.xml`, `src/giris/proto/fake-input.xml`: KDE plasma-wayland-protocols, LGPL-2.1-or-later.
- `src/giris/proto/*.xml` (wlroots ve virtual-keyboard protokolleri): dosya başlıklarındaki lisans.

Chrome Remote Desktop host'u Google'ın ikilisidir ve bu depoda yer almaz; `kur.sh` onu Google'ın resmî adresinden kullanıcının kendi makinesine indirir.
