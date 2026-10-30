#ifndef CRD_GIRIS_H
#define CRD_GIRIS_H

#include <stddef.h>
#include <stdint.h>

struct eis_event;

/* Hyprland 0.56.2 zwlr_virtual_pointer_v1.axis_discrete:
 * deltaDiscrete = discrete * 120. Bir tekerlek tıklaması EIS'te 120 birim,
 * sürekli delta ise 15 (InputManager: 15 smooth unit / 120-unit detent). */
#define GIRIS_SCROLL_STEP_V120 120
#define GIRIS_SCROLL_STEP_UNITS 15.0

enum giris_rec_kind {
	GIRIS_REC_MOTION_ABSOLUTE = 1,
	GIRIS_REC_MOTION,
	GIRIS_REC_BUTTON,
	GIRIS_REC_AXIS_DISCRETE,
	GIRIS_REC_AXIS,
	GIRIS_REC_AXIS_SOURCE,
	GIRIS_REC_AXIS_STOP,
	GIRIS_REC_POINTER_FRAME,
	GIRIS_REC_MODIFIERS,
	GIRIS_REC_KEY,
};

/* Yerel kayıt. Kompozitörün olayı gördüğü anlamına gelmez.
 * Canlı kapı kapalıyken sanal aygıt çağrısı buraya yazılır. */
struct giris_rec {
	enum giris_rec_kind kind;
	uint32_t time_ms;
	uint32_t x, y, x_extent, y_extent;
	double dx, dy;
	uint32_t button;
	uint32_t state;
	uint32_t axis;
	uint32_t axis_source;
	int32_t discrete;
	double axis_value;
	int32_t axis_fixed;
	uint32_t key;
	uint32_t mods_depressed;
	uint32_t mods_latched;
	uint32_t mods_locked;
	uint32_t group;
};

struct giris_global {
	char interface[96];
	uint32_t name;
	uint32_t advertised_version;
	uint32_t bound_version;
};

struct giris_output {
	char name[128];
	int32_t x, y;
	int32_t width, height;
	int32_t scale;
	int32_t refresh;
};

/* Wayland soketinde yöneticileri bağlar, oturum XKB haritasını yükler.
 * Sanal işaretçiye veya klavyeye olay göndermez. 0 veya -errno.
 * Başarısız çağrıdan sonra bağlanan globaller fini'ye kadar okunabilir. */
int giris_init(void);
const char *giris_init_error(void);
void giris_fini(void);

const char *giris_wayland_display(void);

size_t giris_global_count(void);
const struct giris_global *giris_global_at(size_t index);

size_t giris_output_count(void);
const struct giris_output *giris_output_at(size_t index);

/* keymap bayt sayısı, sondaki NUL dahil. Boş harita 0. */
size_t giris_keymap_size(void);
const char *giris_keymap_text(void);
const char *giris_keymap_source(void);
const char *giris_keymap_layouts(void);

/* Oturum haritasında keysym'in evdev kodu. 0 veya -errno. */
int giris_keymap_find_evdev(uint32_t keysym, uint32_t *evdev, char *key_name,
                            size_t key_name_len, char *sym_name,
                            size_t sym_name_len);

/* EIS mutlak hareket, düğme, tekerlek ve tuşu sanal aygıt çağrısına çevirir.
 * Canlı kapı kapalıyken çağrı Wayland'a gitmez, giris_record_at ile okunur.
 * 0 veya -errno. Bilinmeyen olay türü 0 döner ve kayıt yazmaz. */
int giris_handle_ei_event(struct eis_event *event);

/* 0 kapalı. enable != 0 kapıyı açar; bu sürümün kanıtı açmaz. */
int giris_set_live_send(int enable);
int giris_live_send_enabled(void);

size_t giris_record_count(void);
const struct giris_rec *giris_record_at(size_t index);

/* Yalnızca canlı daldaki wl giriş istekleri. Kapı kapalıyken 0 kalır. */
uint64_t giris_wl_input_requests(void);

#endif
