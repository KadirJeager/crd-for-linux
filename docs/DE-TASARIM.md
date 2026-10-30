# Diğer masaüstü ortamları: araştırma ve arka uç tasarımı

[English](DE-DESIGN.md) · **Türkçe**

Kaynaklar 30 Ekim 2026'da okundu. Sürüm satırları o günkü resmî depolardan alındı. Bu makinede yalnız Hyprland 0.56 çalışıyor; aşağıdaki ortamların hiçbiri burada denenmedi.

## Özet

| Ortam | Yakalama | Giriş | Pano | Yol |
|---|---|---|---|---|
| Hyprland | `ext-image-copy-capture-v1` | `zwlr_virtual_pointer_v1` + `zwp_virtual_keyboard_v1` | `ext-data-control-v1` / `wlr-data-control` | Bugünkü kod. **Çalışıyor.** |
| Sway ve wlroots ≥ 0.19 (river, labwc, Wayfire…) | `ext-image-copy-capture-v1` | aynı | aynı | Bugünkü kod, değişiklik beklenmez. **Test edilmedi.** |
| wlroots 0.18 ve öncesi | yalnız `wlr-screencopy-unstable-v1` | aynı | `wlr-data-control` | Yakalamaya screencopy yedeği gerekir. **Yazılmadı.** |
| niri | Smithay tabanlı, wlroots değil | doğrulanmadı | doğrulanmadı | Global listesi oturumda okunmalı. **Bilinmiyor.** |
| KDE Plasma 6 (KWin) | `zkde_screencast_unstable_v1` (KWin PipeWire akışını kendi üretir) | `org_kde_kwin_fake_input` | `ext-data-control-v1` | Ayrı arka uç. **Yazıldı, test edilmedi** (`src/capture` KWin dalı, `src/giris` fake_input dalı). Karşılama ve kesintisiz geçiş yok. |
| GNOME (Mutter) | `org.gnome.Mutter.ScreenCast` (D-Bus) | `org.gnome.Mutter.RemoteDesktop` + EIS | Mutter RemoteDesktop panosu | CRD host'unun **kendi GNOME yolu**; bizim portal gerekmez. **Test edilmedi.** |

## Kanıtlar

**wlroots.** `types/` dizini, 0.18.0 etiketinde `wlr_screencopy_v1.c`, `wlr_data_control_v1.c`, `wlr_virtual_pointer_v1.c` ve `wlr_virtual_keyboard_v1.c` içerir. 0.19.0'da bunlara `wlr_ext_image_copy_capture_v1.c` ve `wlr_ext_data_control_v1.c` eklenir. Kaynak: `gitlab.freedesktop.org/wlroots/wlroots`, `types` ağacı, iki etiket. Bir kompozitörün bu global'leri gerçekten duyurup duyurmadığı ayrı bir konudur: wlroots'ta protokol kodu olması, kompozitörün onu açtığı anlamına gelmez. Bu yüzden oturumda `wl_registry` okunur, sürüm tahmin edilmez.

**KWin.** `KDE/kwin` master (son etiket v6.8.2) şu dosyaları içerir: `src/wayland/screencast_v1.cpp`, `src/backends/fakeinput/`, `src/plugins/eis/` (`eisprompter.cpp` dahil) ve `src/wayland/datacontroldevicemanager_v1.cpp`. Pano yöneticisi `ext_data_control_manager_v1`'dir. Ağaçta `ext-image-copy-capture`, `wlr-screencopy` ya da `wlr` sanal fare/klavye yoktur.

`src/wayland_server.cpp` `KWinDisplay::allowInterface` içinde `org_kde_kwin_fake_input`, `zkde_screencast_unstable_v1` ve `ext_data_control_manager_v1` "kısıtlı" listededir. Kısıt yalnız `client->isSandboxed()` doğruysa uygulanır. Yerel, sandbox'sız bir süreç bunlara bağlanabilir; eski `X-KDE-Wayland-Interfaces` masaüstü dosyası koşulu bu sürümde kodda yoktur. Plasma 6.x'in eski sürümlerinde kural farklı olabilir; sürüm aralığı doğrulanmadı.

**Mutter.** `GNOME/mutter` main ağacında `src/backends/meta-remote-desktop*.c`, `meta-screen-cast*.c` ve `meta-eis*.c` vardır. Wayland tarafında `image-copy`, `data-control` ya da sanal fare/klavye protokolü yoktur.

**CRD 157.** `third_party/chromium-157/remoting_host_create_desktop_interaction_strategy_factory.cc` 31-35. satırlar: Wayland altında `GnomeRemoteDesktopSession::IsRunningUnderGnome()` doğruysa `GnomeInteractionStrategyFactory` seçilir, değilse portal yolu (`PortalInteractionStrategyFactory`). GNOME yolu doğrudan `org.gnome.Mutter.RemoteDesktop` ve `org.gnome.Mutter.ScreenCast` arayüzlerini çağırır (`remoting_host_linux_gnome_remote_desktop_session.cc` 44-48). Bu projenin portalı yalnız GNOME dışında devreye girer.

## Arka uç ayrımı

Ortak kalan:

- `src/portal`: D-Bus portal arayüzleri, oturum ve istek nesneleri, EIS sunucusu.
- `src/proxy`: host'a özel veri yolu.
- `src/oturum`: koltuk izleme, karşılama ↔ masaüstü geçişi.
- `src/ses`: PipeWire hoparlör monitörü.
- `src/cikti`: çıktı seçme kuralı. Kural aynı kalır, çıktı listesinin kaynağı arka uca göre değişir.

- `src/clipboard`: her arka uçta `ext-data-control-v1` (ve `wlr-data-control`).

Kompozitöre göre, duyurduğu global'lerden seçilen:

- `src/capture/capture.c`: `ext_image_copy_capture_manager_v1` ve yakalama kaynağı yöneticisi varsa süreç kendi PipeWire düğümünü açar ve kare kopyalar. Yoksa ve `zkde_screencast_unstable_v1` varsa (düğüm kimliği `created` olayıyla gelsin diye sürüm 5 bağlanır) KWin'den seçilen çıktıyı imleç bilgisiyle yayınlaması istenir, KWin'in düğüm kimliği host'a verilir; kare kopyalanmaz.
- `src/giris/giris.c`: `zwlr_virtual_pointer_manager_v1` ve `zwp_virtual_keyboard_manager_v1` varsa onlar kullanılır. Yoksa ve `org_kde_kwin_fake_input` sürüm ≥ 4 varsa bir kez `authenticate` edilir; mutlak hareket (küresel mantıksal koordinat), düğme, eksen ve tuş ona gider. Yarım wlr seti KWin'e düşmez.
- İkisi de tam değilse başlatma başarısız olur ve eksik global'ler adıyla günlüğe yazılır.

KWin arka ucunda yakalama farklı çalışır: `zkde_screencast_unstable_v1.stream_output` ile KWin akışı kendisi yayımlar ve düğüm kimliğini `created` olayıyla verir. Bizim `capture.c` düğüm açmaz, kare kopyalamaz. Koltuk değişince KWin akışı kapanır; host'un bağı kopar. Kesintisiz geçiş için ya ara bir PipeWire düğümü (bugünkü düğüm, kaynağı KWin akışı) gerekir ya da bu ortamda geçiş desteklenmez. Önerilen: bugünkü düğümü koruyup KWin akışından beslemek; doğrulanmadı.

## Ortam algılama

Tahmin `XDG_CURRENT_DESKTOP` ile değil, kompozitörün duyurduğu global'lerle yapılır:

1. `GnomeRemoteDesktopSession::IsRunningUnderGnome()` host içinde zaten karar verir. Kurulum `XDG_CURRENT_DESKTOP` içinde `GNOME` görürse portal ve vekil açılmaz; host oturum veri yoluna bağlanır.
2. `ext_image_copy_capture_manager_v1` ve `zwlr_virtual_pointer_manager_v1` varsa wlroots arka ucu.
3. `zkde_screencast_unstable_v1` ve `org_kde_kwin_fake_input` varsa KWin arka ucu.
4. Hiçbiri yoksa portal açılmaz; eksik global'ler günlüğe yazılır.

## Karşılama ekranı

| Karşılayıcı | Kompozitör | Durum |
|---|---|---|
| greetd + dms-greeter / tuigreet (Hyprland, Sway) | Karşılama kullanıcısı adına wlroots tabanlı kompozitör | Bugünkü ACL yaklaşımı geçerli. Hyprland'de kanıtlandı. |
| SDDM (KDE) | Plasma 6'da `sddm` kullanıcısı adına KWin (Wayland karşılayıcı açıksa) | Soket ve ACL yaklaşımı uygulanabilir görünüyor. Karşılama KWin'inin kısıtlı global'leri sandbox'sız istemciye açtığı doğrulanmadı. |
| GDM (GNOME) | `gdm` kullanıcısı adına gnome-shell | Mutter D-Bus arayüzleri `gdm` oturum veri yolundadır; host başka kullanıcıyla bağlanamaz. Desteklenmez; GNOME'da yalnız açık oturum. |

## İş listesi

1. wlroots: Sway'de dene (kurulum betiği `--deneysel`). Eksik global günlükte görünür.
2. wlroots 0.18: `wlr-screencopy-unstable-v1` ile yakalama yedeği.
3. KWin: arka uç yazıldı (yakalama `zkde_screencast` v5, imleç metadata; giriş `fake_input` v4). Plasma'da denenmeli; koltuk değişiminde KWin akışı kapandığı için kesintisiz geçiş için ara PipeWire düğümü gerekir.
4. GNOME: kurulum, host'u portal olmadan oturum veri yoluyla başlatan yol; karşılama desteklenmez.
5. niri: global listesini oturumda oku, uygun arka ucu seç.

## Doğrulanmadı

- Sway ve diğer wlroots kompozitörlerinin hangi sürümde hangi global'i duyurduğu.
- Plasma 6.0–6.7 arasında `allowInterface` kuralının aynı olduğu.
- SDDM Wayland karşılayıcısında KWin'in kısıtlı global'leri ve soket izinleri.
- KWin akışının koltuk değişiminde kesintisiz sürdürülebileceği.
- niri'nin protokol desteği.
- CRD'nin GNOME yolunun kilit ekranı ve karşılama davranışı.
