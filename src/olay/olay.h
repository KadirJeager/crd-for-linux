#ifndef CRD_OLAY_H
#define CRD_OLAY_H

/* EIS sunucusunun giriş olayını giris_handle_ei_event'e bağlar.
 * Canlı kapıyı açmaz. Kapı kapalıyken çevirici olayı yerel kayda yazar.
 */

int olay_init(void);
const char *olay_init_error(void);
int olay_client_fd(void);
/* Her çağrı yeni bir EIS istemci soketi döner. Çağıran kapatır. */
int olay_open_client_fd(void);

/* 1: bir mutlak hareket ve bir tuş işlendi.
 * 0: henüz değil.
 * <0: çevirici hata kodu.
 */
int olay_geldi(void);

/* Yeni olay çeviriciye gitmez. Kayıt okuması bundan sonra güvenlidir. */
void olay_kopru_kapat(void);

void olay_fini(void);

#endif
