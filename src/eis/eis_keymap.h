#ifndef CRD_EIS_KEYMAP_H
#define CRD_EIS_KEYMAP_H

#include <stddef.h>

struct eis_device;
struct xkb_context;
struct xkb_keymap;

/* Kompozitörün wl_keyboard haritası, yoksa XKB_DEFAULT_*, yoksa sistem
 * düzeni, hiçbiri yoksa xkbcommon varsayılanı. Düzen adı kodda sabit değildir. */
int crd_xkb_keymap_from_session(struct xkb_context *ctx, struct xkb_keymap **out,
				char *source, size_t source_len);

/* Aynı haritayı aygıta ekler. */
int eis_xkb_attach_to_device(struct eis_device *device);

/* Human-readable keymap source for test reports. Valid after successful attach. */
const char *eis_xkb_keymap_source(void);

#endif
