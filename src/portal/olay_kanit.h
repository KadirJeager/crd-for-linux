#ifndef CRD_OLAY_KANIT_H
#define CRD_OLAY_KANIT_H

/* path boşsa kanıt yazılmaz ve ölçüm için köprü kapatılmaz.
 * Doluysa ana döngü, mutlak hareket ve tuş kayda düşünce dosyayı bir kez yazar.
 * Canlı kapıyı açmaz.
 * 0: kol kuruldu ya da kanıt istenmedi.
 * -1: zamanlayıcı kurulamadı.
 */
int olay_kanit_arm(const char *path);

/* Henüz yazılmadıysa son kaydı yazar. Kanıt istenmediyse dokunmaz. */
void olay_kanit_flush(void);

#endif
