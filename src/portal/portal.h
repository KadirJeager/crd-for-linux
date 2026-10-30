#ifndef CRD_PORTAL_H
#define CRD_PORTAL_H

/* crd_portal_run verilen adrese bağlanır, org.freedesktop.portal.Desktop
   adını orada alır ve ana döngüyü çalıştırır.

   Gerçek oturum veriyoluna (DBUS_SESSION_BUS_ADDRESS veya
   XDG_RUNTIME_DIR/bus) bağlanmaz ve o adrese sahip çıkmaz; bu durumda
   3 döner.

   0: ad alınmıştı, döngü durdu.
   4: bağlanılamadı ya da ad alınamadı.
*/
int crd_portal_run(const char *bus_address);

#endif
