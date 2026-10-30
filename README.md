# CRD for Linux

**Chrome Remote Desktop with your existing Wayland session — no virtual desktop, no Xvfb.**

Google's Chrome Remote Desktop host on Linux normally starts a separate virtual X session. This project runs the unmodified official host against the session you are already using: you see your real desktop, type with your real keyboard layout, share the clipboard and hear your speakers — from the web client or the mobile app, without a permission dialog. It also works on the login screen: the host starts on the greeter, you type your password remotely and the same connection continues into your desktop.

> Bu proje, Chrome Remote Desktop'ı açık olan Wayland oturumuna bağlar. Sanal masaüstü açmaz, Google'ın ikilisine yama yapmaz. Ayrıntılar aşağıda Türkçe.

## Durum

| Ortam | Durum |
|---|---|
| Hyprland 0.56 (CachyOS, greetd + dms-greeter) | **Çalışıyor.** Görüntü, fare, klavye (Türkçe dahil), iki yönlü pano, ses, kilit ekranından giriş ve kesintisiz geçiş doğrulandı. |
| Sway, river, labwc, niri, Wayfire (wlroots) | **Test edilmedi.** Aynı Wayland protokollerini kullandığı için çalışması beklenir. |
| KDE Plasma 6 (KWin) | **Yazıldı, test edilmedi.** Yakalama KWin'in kendi ekran yayınıyla (`zkde_screencast`), giriş `fake_input` ile, pano aynı. Karşılama ekranı (SDDM) ve girişte kesintisiz geçiş yok. |
| GNOME (Mutter) | **Test edilmedi.** CRD host'u GNOME'da Mutter'ın kendi uzak masaüstü arayüzünü kullanır; `kur.sh` onu açık oturumda başlatan bir kullanıcı birimi kurar. Karşılama ekranı (GDM) desteklenmez. |
| Birden çok monitör | Henüz yok: tek çıktı aktarılır (`CRD_OUTPUT` ile seçilir). |
| Dosya aktarımı | Henüz yok. |

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
| `third_party/chromium-157` | Arayüzlerin karşılaştırıldığı Chromium kaynak dosyaları (BSD lisansı) |

## Kurulum

Gereken: Wayland oturumu, PipeWire + WirePlumber, libei, xdg-dbus-proxy, derleyici. Google'ın CRD paketi depoda yok; kurulum betiği resmî paketi Google'dan kendi makinenize indirir ve sisteme kurmadan açar.

```bash
./kur.sh denetle
```

```bash
./kur.sh kur
```

Her komut `--deneme` ile yalnız ne yapacağını yazar. Hyprland dışındaki ortamlarda betik uyarır ve yalnız `--deneysel` ile devam eder.

Host'un Google hesabınıza kaydı ayrı bir adımdır: [remotedesktop.google.com/headless](https://remotedesktop.google.com/headless) sayfasındaki komutu kurulum betiğinin yazdığı yoldaki ikiliyle çalıştırırsınız. PIN ve kod hiçbir günlüğe yazılmaz.

Kaldırmak için:

```bash
./kur.sh kaldir
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

## Lisans

GPL-3.0-or-later; tam metin [LICENSE](LICENSE).

Başka kaynaklardan gelen dosyalar kendi lisanslarıyla kalır:

- `third_party/chromium-157`: Chromium kaynak dosyaları, BSD tarzı lisans (dosya başlıklarında).
- `src/capture/proto/zkde-screencast-unstable-v1.xml`, `src/giris/proto/fake-input.xml`: KDE plasma-wayland-protocols, LGPL-2.1-or-later.
- `src/giris/proto/*.xml` (wlroots ve virtual-keyboard protokolleri): dosya başlıklarındaki lisans.

Chrome Remote Desktop host'u Google'ın ikilisidir ve bu depoda yer almaz; `kur.sh` onu Google'ın resmî adresinden kullanıcının kendi makinesine indirir.
