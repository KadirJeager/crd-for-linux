#include "cursor_theme.h"

#include <hyprcursor/hyprcursor.h>
#include <cairo/cairo.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THEME_MAX 48

/* wp_cursor_shape adları. Hyprland masaüstü imleci "default" / left_ptr. */
static const char *const shape_names[] = {
	"default",	 "left_ptr",	   "context-menu", "help",
	"pointer",	 "progress",	   "wait",	   "cell",
	"crosshair",	 "text",	   "vertical-text", "alias",
	"copy",		 "move",	   "no-drop",	   "not-allowed",
	"grab",		 "grabbing",	   "e-resize",	   "n-resize",
	"ne-resize",	 "nw-resize",	   "s-resize",	   "se-resize",
	"sw-resize",	 "w-resize",	   "ew-resize",	   "ns-resize",
	"nesw-resize",	 "nwse-resize",	   "col-resize",   "row-resize",
	"all-scroll",	 "zoom-in",	   "zoom-out",	   "dnd-ask",
};

struct shape_px {
	char name[32];
	int w, h, hx, hy;
	uint8_t *px;
};

struct theme_lib {
	void *hc;
	void *cairo;
	struct hyprcursor_manager_t *(*create)(const char *);
	int (*valid)(struct hyprcursor_manager_t *);
	int (*load_style)(struct hyprcursor_manager_t *, struct hyprcursor_cursor_style_info);
	hyprcursor_cursor_image_data **(*get_images)(struct hyprcursor_manager_t *, const char *,
						     struct hyprcursor_cursor_style_info, int *);
	void (*free_images)(hyprcursor_cursor_image_data **, int);
	void (*style_done)(struct hyprcursor_manager_t *, struct hyprcursor_cursor_style_info);
	void (*flush)(cairo_surface_t *);
	int (*surf_w)(cairo_surface_t *);
	int (*surf_h)(cairo_surface_t *);
	int (*surf_stride)(cairo_surface_t *);
	unsigned char *(*surf_data)(cairo_surface_t *);
	struct hyprcursor_manager_t *mgr;
	unsigned int style_size;
	int style_ready;
	struct shape_px shapes[THEME_MAX];
	int nshapes;
	int ready;
	int failed;
};

static struct theme_lib T;

static int bind_sym(void *handle, const char *name, void **out)
{
	void *sym = dlsym(handle, name);
	if (!sym)
		return -1;
	*out = sym;
	return 0;
}

static int open_libs(void)
{
	const char *theme;

	if (T.ready)
		return 0;
	if (T.failed)
		return -1;
	T.hc = dlopen("libhyprcursor.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (!T.hc)
		T.hc = dlopen("libhyprcursor.so", RTLD_LAZY | RTLD_LOCAL);
	T.cairo = dlopen("libcairo.so.2", RTLD_LAZY | RTLD_LOCAL);
	if (!T.cairo)
		T.cairo = dlopen("libcairo.so", RTLD_LAZY | RTLD_LOCAL);
	if (!T.hc || !T.cairo)
		goto fail;
	if (bind_sym(T.hc, "hyprcursor_manager_create", (void **)&T.create) ||
	    bind_sym(T.hc, "hyprcursor_manager_valid", (void **)&T.valid) ||
	    bind_sym(T.hc, "hyprcursor_load_theme_style", (void **)&T.load_style) ||
	    bind_sym(T.hc, "hyprcursor_get_cursor_image_data", (void **)&T.get_images) ||
	    bind_sym(T.hc, "hyprcursor_cursor_image_data_free", (void **)&T.free_images) ||
	    bind_sym(T.hc, "hyprcursor_style_done", (void **)&T.style_done) ||
	    bind_sym(T.cairo, "cairo_surface_flush", (void **)&T.flush) ||
	    bind_sym(T.cairo, "cairo_image_surface_get_width", (void **)&T.surf_w) ||
	    bind_sym(T.cairo, "cairo_image_surface_get_height", (void **)&T.surf_h) ||
	    bind_sym(T.cairo, "cairo_image_surface_get_stride", (void **)&T.surf_stride) ||
	    bind_sym(T.cairo, "cairo_image_surface_get_data", (void **)&T.surf_data))
		goto fail;
	theme = getenv("HYPRCURSOR_THEME");
	if (!theme || !theme[0])
		theme = getenv("XCURSOR_THEME");
	T.mgr = T.create(theme && theme[0] ? theme : NULL);
	if (!T.mgr || !T.valid(T.mgr))
		goto fail;
	T.ready = 1;
	return 0;
fail:
	T.failed = 1;
	return -1;
}

static void clear_shapes(void)
{
	int i;

	for (i = 0; i < T.nshapes; i++)
		free(T.shapes[i].px);
	T.nshapes = 0;
	if (T.style_ready && T.mgr) {
		struct hyprcursor_cursor_style_info info = { .size = T.style_size };
		T.style_done(T.mgr, info);
		T.style_ready = 0;
	}
}

static int shape_nonzero(const uint8_t *px, int w, int h)
{
	int i, n = w * h * 4;

	for (i = 0; i < n; i++) {
		if (px[i])
			return 1;
	}
	return 0;
}

static int load_style(unsigned int size)
{
	struct hyprcursor_cursor_style_info info = { .size = size };
	size_t i;

	if (T.style_ready && T.style_size == size && T.nshapes > 0)
		return 0;
	clear_shapes();
	if (!T.load_style(T.mgr, info))
		return -1;
	T.style_size = size;
	T.style_ready = 1;
	for (i = 0; i < sizeof shape_names / sizeof shape_names[0]; i++) {
		hyprcursor_cursor_image_data **imgs;
		int n = 0;
		int hx, hy, w, h, stride, y;
		cairo_surface_t *surf;
		uint8_t *px;
		struct shape_px *s;

		if (T.nshapes >= THEME_MAX)
			break;
		imgs = T.get_images(T.mgr, shape_names[i], info, &n);
		if (!imgs || n < 1 || !imgs[0] || !imgs[0]->surface) {
			if (imgs)
				T.free_images(imgs, n);
			continue;
		}
		hx = imgs[0]->hotspotX;
		hy = imgs[0]->hotspotY;
		surf = imgs[0]->surface;
		T.flush(surf);
		w = T.surf_w(surf);
		h = T.surf_h(surf);
		stride = T.surf_stride(surf);
		if (w <= 0 || h <= 0 || w > 256 || h > 256 || stride < w * 4) {
			T.free_images(imgs, n);
			continue;
		}
		px = malloc((size_t)w * (size_t)h * 4);
		if (!px) {
			T.free_images(imgs, n);
			return -1;
		}
		for (y = 0; y < h; y++) {
			memcpy(px + (size_t)y * (size_t)w * 4,
			       T.surf_data(surf) + (size_t)y * (size_t)stride,
			       (size_t)w * 4);
		}
		T.free_images(imgs, n);
		if (!shape_nonzero(px, w, h)) {
			free(px);
			continue;
		}
		s = &T.shapes[T.nshapes++];
		memset(s, 0, sizeof *s);
		snprintf(s->name, sizeof s->name, "%s", shape_names[i]);
		s->w = w;
		s->h = h;
		s->hx = hx;
		s->hy = hy;
		s->px = px;
	}
	return T.nshapes > 0 ? 0 : -1;
}

static int same_pixels(const struct shape_px *a, const struct shape_px *b)
{
	if (a->w != b->w || a->h != b->h)
		return 0;
	return memcmp(a->px, b->px, (size_t)a->w * (size_t)a->h * 4) == 0;
}

static int is_default_name(const char *name)
{
	return strcmp(name, "default") == 0 || strcmp(name, "left_ptr") == 0;
}

int cursor_theme_copy(int width, int height, int hotspot_x, int hotspot_y,
		      uint8_t *dst, int dst_stride)
{
	unsigned int size;
	int i, chosen = -1, distinct = 0;
	int match[THEME_MAX];
	int nmatch = 0;

	if (!dst || width <= 0 || height <= 0 || width > 256 || height > 256 ||
	    dst_stride < width * 4)
		return -1;
	if (open_libs() < 0)
		return -1;
	size = (unsigned int)(width > height ? width : height);
	if (load_style(size) < 0)
		return -1;

	for (i = 0; i < T.nshapes; i++) {
		if (T.shapes[i].w == width && T.shapes[i].h == height &&
		    T.shapes[i].hx == hotspot_x && T.shapes[i].hy == hotspot_y)
			match[nmatch++] = i;
	}
	if (nmatch == 0)
		return -1;

	for (i = 0; i < nmatch; i++) {
		int j, seen = 0;
		for (j = 0; j < i; j++) {
			if (same_pixels(&T.shapes[match[i]], &T.shapes[match[j]])) {
				seen = 1;
				break;
			}
		}
		if (!seen)
			distinct++;
	}

	if (distinct == 1) {
		chosen = match[0];
	} else {
		/* Aynı sıcak noktayı paylaşan farklı şekiller var.
		 * Hyprland masaüstü imleci default/left_ptr. Başka bir ada
		 * tahmin yürütülmez. */
		for (i = 0; i < nmatch; i++) {
			if (is_default_name(T.shapes[match[i]].name)) {
				chosen = match[i];
				break;
			}
		}
	}
	if (chosen < 0)
		return -1;

	for (i = 0; i < height; i++) {
		memcpy(dst + (size_t)i * (size_t)dst_stride,
		       T.shapes[chosen].px + (size_t)i * (size_t)width * 4,
		       (size_t)width * 4);
	}
	return 0;
}
