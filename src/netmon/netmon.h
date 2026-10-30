#ifndef CRD_NETMON_H
#define CRD_NETMON_H

#include <gio/gio.h>

/* org.freedesktop.portal.NetworkMonitor sürüm 3.
 * Belge: /usr/share/dbus-1/interfaces/org.freedesktop.portal.NetworkMonitor.xml
 *
 * netmon_register, çağıranın açık GDBus bağlantısında aynı arayüzü
 * /org/freedesktop/portal/desktop yoluna kaydeder. Bağlantı açmaz ve
 * org.freedesktop.portal.Desktop adını almaz; adı çağıran süreç alır.
 *
 * Değerler GNetworkMonitorNM ya da GNetworkMonitorNetlink üzerinden okunur.
 * Başka bir kaynaktan çevrimiçi üretilmez; o durumda kayıt yapılmaz.
 *
 * netmon_check veriyoluna dokunmadan belgeyi ve ağ kaynağını doğrular.
 * 0 başarıdır. netmon_register için:
 *   0        kayıt yapıldı
 *   -EBUSY   bu süreçte arayüz zaten kayıtlı
 *   -EINVAL  bağlantı yok ya da belge uyumsuz
 *   -ENOTCONN bağlantı kapalı
 *   -ENODEV  ağ kaynağı gerçek değil
 *   -EIO     nesne yayınlanamadı
 * Son hatanın metni netmon_error() ile okunur; yoksa NULL.
 * Kayıt kalkınca netmon_unregister çağrılır. İkinci çağrı zararsızdır.
 */
int netmon_check(void);
const char *netmon_monitor_name(void);
int netmon_register(GDBusConnection *connection);
void netmon_unregister(void);
const char *netmon_error(void);

#endif
