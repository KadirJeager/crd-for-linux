#ifndef CRD_CURSOR_THEME_H
#define CRD_CURSOR_THEME_H

#include <stddef.h>
#include <stdint.h>

/* Hyprland tema imlecinin ARGB'si. Başarıda dst, B,G,R,A satırlarıdır.
 * Eşleşen gerçek görüntü yoksa -1; dst'ye yazılmaz. */
int cursor_theme_copy(int width, int height, int hotspot_x, int hotspot_y,
		      uint8_t *dst, int dst_stride);

#endif
