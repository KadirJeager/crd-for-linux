#ifndef CRD_CIKTI_H
#define CRD_CIKTI_H

#include <stddef.h>
#include <stdint.h>

/* Kompozitörün bir çıktısı.
 * name: wl_output.name; o olay yoksa xdg_output.name. Uydurma ad yok.
 * description: wl_output.description; yoksa xdg_output.description. Boş olabilir.
 * x, y, width, height: zxdg_output_v1 mantıksal konum ve boyut.
 * mode_width, mode_height: wl_output güncel modu, donanım pikseli. 0 ise gelmedi.
 * scale: wl_output.scale. refresh: güncel mod, mHz. */
struct crd_cikti {
	char name[128];
	char description[256];
	int32_t x, y;
	int32_t width, height;
	int32_t scale;
	int32_t refresh;
	int32_t mode_width, mode_height;
};

/* Varsayılan Wayland görüntüsündeki çıktıyı seçer.
 *
 * Kural:
 * 1. CRD_OUTPUT doluysa yalnız o ad. Listede yoksa hata; başka çıktıya düşülmez.
 * 2. Tam çıktı tekse o. Mantıksal konumu 0,0 olmak zorunda değil.
 * 3. Birden çok tam çıktı varsa mantıksal konumu 0,0 olan. Birden çok 0,0
 *    varsa kayıt sırasındaki ilk (en küçük indeks) seçilir ve bu yazılır.
 *    Hiçbiri 0,0 değilse hata; en büyük ya da isim sırası uydurulmaz.
 *
 * Tam çıktı: adı var, mantıksal konumu ve pozitif mantıksal boyutu gelmiş.
 * zxdg_output_manager_v1 yoksa hata. Birden çok akış burada açılmaz;
 * bütün çıktılar okunur, biri seçilir. 0 veya -1. */
int crd_cikti_bul(struct crd_cikti *out);

/* PipeWire düğüm adı: "crd-capture-" ve çıktı adının küçük harfi.
 * HDMI-A-1 -> crd-capture-hdmi-a-1. Harf, rakam ve tire dışındakiler tire olur. */
int crd_cikti_dugum_adi(const char *output_name, char *buf, size_t buflen);

#endif
