#include "cikti.h"

#include "xdg-output-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#define MAX_OUTPUTS 16

struct output {
	struct wl_output *wl;
	struct zxdg_output_v1 *xdg;
	struct crd_cikti info;
	char xdg_name[128];
	char xdg_desc[256];
	int have_wl_name;
	int have_xdg_name;
	int have_wl_desc;
	int have_logical_pos;
	int have_logical_size;
	int have_mode;
};

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct zxdg_output_manager_v1 *manager;
	struct output outs[MAX_OUTPUTS];
	int n;
	int overflow;
};

static void on_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
			int32_t phys_w, int32_t phys_h, int32_t subpixel,
			const char *make, const char *model, int32_t transform)
{
	(void)data;
	(void)output;
	(void)x;
	(void)y;
	(void)phys_w;
	(void)phys_h;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	/* Konum xdg_output mantıksal konumundan gelir. wl_output.geometry
	   masaüstü koordinatı olarak kullanılmaz. */
}

static void on_mode(void *data, struct wl_output *output, uint32_t flags,
		    int32_t width, int32_t height, int32_t refresh)
{
	struct output *o = data;

	(void)output;
	if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
		return;
	o->info.mode_width = width;
	o->info.mode_height = height;
	o->info.refresh = refresh;
	o->have_mode = 1;
}

static void on_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void on_scale(void *data, struct wl_output *output, int32_t scale)
{
	struct output *o = data;

	(void)output;
	o->info.scale = scale;
}

static void on_name(void *data, struct wl_output *output, const char *name)
{
	struct output *o = data;

	(void)output;
	snprintf(o->info.name, sizeof o->info.name, "%s", name ? name : "");
	o->have_wl_name = o->info.name[0] != '\0';
}

static void on_description(void *data, struct wl_output *output, const char *description)
{
	struct output *o = data;

	(void)output;
	snprintf(o->info.description, sizeof o->info.description, "%s",
		 description ? description : "");
	o->have_wl_desc = 1;
}

static const struct wl_output_listener output_listener = {
	.geometry = on_geometry,
	.mode = on_mode,
	.done = on_done,
	.scale = on_scale,
	.name = on_name,
	.description = on_description,
};

static void on_logical_position(void *data, struct zxdg_output_v1 *xdg, int32_t x, int32_t y)
{
	struct output *o = data;

	(void)xdg;
	o->info.x = x;
	o->info.y = y;
	o->have_logical_pos = 1;
}

static void on_logical_size(void *data, struct zxdg_output_v1 *xdg, int32_t width,
			    int32_t height)
{
	struct output *o = data;

	(void)xdg;
	o->info.width = width;
	o->info.height = height;
	o->have_logical_size = 1;
}

static void on_xdg_done(void *data, struct zxdg_output_v1 *xdg)
{
	(void)data;
	(void)xdg;
}

static void on_xdg_name(void *data, struct zxdg_output_v1 *xdg, const char *name)
{
	struct output *o = data;

	(void)xdg;
	snprintf(o->xdg_name, sizeof o->xdg_name, "%s", name ? name : "");
	o->have_xdg_name = o->xdg_name[0] != '\0';
}

static void on_xdg_description(void *data, struct zxdg_output_v1 *xdg, const char *description)
{
	struct output *o = data;

	(void)xdg;
	snprintf(o->xdg_desc, sizeof o->xdg_desc, "%s", description ? description : "");
}

static const struct zxdg_output_v1_listener xdg_listener = {
	.logical_position = on_logical_position,
	.logical_size = on_logical_size,
	.done = on_xdg_done,
	.name = on_xdg_name,
	.description = on_xdg_description,
};

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
		      const char *interface, uint32_t version)
{
	struct state *st = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *o;
		uint32_t bound = version > 4 ? 4 : version;

		if (st->n >= MAX_OUTPUTS) {
			st->overflow = 1;
			return;
		}
		if (bound < 1)
			return;
		o = &st->outs[st->n];
		memset(o, 0, sizeof *o);
		o->info.scale = 1;
		o->wl = wl_registry_bind(registry, name, &wl_output_interface, bound);
		if (!o->wl)
			return;
		wl_output_add_listener(o->wl, &output_listener, o);
		st->n++;
		return;
	}
	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0 && !st->manager) {
		uint32_t bound = version > 3 ? 3 : version;

		if (bound < 1)
			return;
		st->manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
					       bound);
	}
}

static void on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = on_global,
	.global_remove = on_global_remove,
};

static void finish_identity(struct output *o)
{
	if (!o->have_wl_name && o->have_xdg_name) {
		snprintf(o->info.name, sizeof o->info.name, "%s", o->xdg_name);
		fprintf(stderr, "crd-cikti: wl_output.name boş, xdg_output.name=%s\n",
			o->xdg_name);
	} else if (o->have_wl_name && o->have_xdg_name &&
		   strcmp(o->info.name, o->xdg_name) != 0) {
		fprintf(stderr, "crd-cikti: wl_output.name=%s xdg_output.name=%s; wl adı\n",
			o->info.name, o->xdg_name);
	}
	if (!o->have_wl_desc && o->xdg_desc[0] != '\0')
		snprintf(o->info.description, sizeof o->info.description, "%s", o->xdg_desc);
}

static int eligible(const struct output *o)
{
	return o->info.name[0] != '\0' && o->have_logical_pos && o->have_logical_size &&
	       o->info.width > 0 && o->info.height > 0;
}

static void log_seen(const struct state *st)
{
	for (int i = 0; i < st->n; i++) {
		const struct output *o = &st->outs[i];

		fprintf(stderr,
			"crd-cikti: görülen [%d] ad=%s açıklama=%s konum=%d,%d mantıksal=%dx%d mod=%dx%d\n",
			i, o->info.name[0] ? o->info.name : "(adsız)",
			o->info.description[0] ? o->info.description : "(yok)", o->info.x,
			o->info.y, o->info.width, o->info.height, o->info.mode_width,
			o->info.mode_height);
	}
}

static void destroy_state(struct state *st)
{
	for (int i = 0; i < st->n; i++) {
		if (st->outs[i].xdg)
			zxdg_output_v1_destroy(st->outs[i].xdg);
		if (st->outs[i].wl)
			wl_output_destroy(st->outs[i].wl);
	}
	if (st->manager)
		zxdg_output_manager_v1_destroy(st->manager);
	if (st->registry)
		wl_registry_destroy(st->registry);
	if (st->display)
		wl_display_disconnect(st->display);
}

int crd_cikti_dugum_adi(const char *output_name, char *buf, size_t buflen)
{
	static const char prefix[] = "crd-capture-";
	size_t prefix_len = sizeof prefix - 1;
	size_t n = 0;
	int prev_dash = 0;

	if (!output_name || output_name[0] == '\0' || !buf || buflen <= prefix_len)
		return -1;
	memcpy(buf, prefix, prefix_len);
	n = prefix_len;
	for (const char *p = output_name; *p != '\0'; p++) {
		unsigned char c = (unsigned char)*p;
		char out;

		if (c >= 'A' && c <= 'Z')
			out = (char)(c - 'A' + 'a');
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			out = (char)c;
		else
			out = '-';
		if (out == '-' && prev_dash)
			continue;
		if (n + 1 >= buflen)
			return -1;
		buf[n++] = out;
		prev_dash = out == '-';
	}
	while (n > prefix_len && buf[n - 1] == '-')
		n--;
	if (n == prefix_len)
		return -1;
	buf[n] = '\0';
	return 0;
}

int crd_cikti_bul(struct crd_cikti *out)
{
	struct state st;
	const char *env;
	const char *wl_name;
	int chosen = -1;
	const char *rule = NULL;

	if (!out)
		return -1;
	memset(&st, 0, sizeof st);
	memset(out, 0, sizeof *out);
	wl_name = getenv("WAYLAND_DISPLAY");
	st.display = wl_display_connect(NULL);
	if (!st.display) {
		fprintf(stderr, "crd-cikti: Wayland görüntüsüne bağlanılamadı (WAYLAND_DISPLAY=%s)\n",
			wl_name && wl_name[0] ? wl_name : "(yok)");
		return -1;
	}
	st.registry = wl_display_get_registry(st.display);
	if (!st.registry) {
		fprintf(stderr, "crd-cikti: wl_registry yok\n");
		destroy_state(&st);
		return -1;
	}
	wl_registry_add_listener(st.registry, &registry_listener, &st);
	if (wl_display_roundtrip(st.display) < 0) {
		fprintf(stderr, "crd-cikti: kayıt turu başarısız\n");
		destroy_state(&st);
		return -1;
	}
	if (st.overflow) {
		fprintf(stderr, "crd-cikti: %d çıktıdan fazlası okunmadı\n", MAX_OUTPUTS);
		log_seen(&st);
		destroy_state(&st);
		return -1;
	}
	if (!st.manager) {
		fprintf(stderr, "crd-cikti: zxdg_output_manager_v1 yok\n");
		log_seen(&st);
		destroy_state(&st);
		return -1;
	}
	for (int i = 0; i < st.n; i++) {
		st.outs[i].xdg = zxdg_output_manager_v1_get_xdg_output(st.manager, st.outs[i].wl);
		if (!st.outs[i].xdg) {
			fprintf(stderr, "crd-cikti: xdg_output kurulamadı\n");
			destroy_state(&st);
			return -1;
		}
		zxdg_output_v1_add_listener(st.outs[i].xdg, &xdg_listener, &st.outs[i]);
	}
	if (wl_display_roundtrip(st.display) < 0 || wl_display_roundtrip(st.display) < 0) {
		fprintf(stderr, "crd-cikti: xdg_output turu başarısız\n");
		destroy_state(&st);
		return -1;
	}
	for (int i = 0; i < st.n; i++)
		finish_identity(&st.outs[i]);

	env = getenv("CRD_OUTPUT");
	if (env && env[0] != '\0') {
		rule = "CRD_OUTPUT";
		for (int i = 0; i < st.n; i++) {
			if (eligible(&st.outs[i]) && strcmp(st.outs[i].info.name, env) == 0) {
				chosen = i;
				break;
			}
		}
		if (chosen < 0) {
			fprintf(stderr, "crd-cikti: CRD_OUTPUT=%s bulunamadı\n", env);
			log_seen(&st);
			destroy_state(&st);
			return -1;
		}
	} else {
		int nfull = 0;
		int at_origin = 0;

		for (int i = 0; i < st.n; i++) {
			if (!eligible(&st.outs[i]))
				continue;
			nfull++;
			if (st.outs[i].info.x == 0 && st.outs[i].info.y == 0)
				at_origin++;
		}
		if (nfull == 0) {
			fprintf(stderr, "crd-cikti: adı ve mantıksal boyutu olan çıktı yok\n");
			log_seen(&st);
			destroy_state(&st);
			return -1;
		}
		if (nfull == 1) {
			rule = "tek çıktı";
			for (int i = 0; i < st.n; i++) {
				if (eligible(&st.outs[i])) {
					chosen = i;
					break;
				}
			}
		} else if (at_origin == 0) {
			fprintf(stderr,
				"crd-cikti: %d çıktı var, mantıksal 0,0 yok; CRD_OUTPUT verilmedi\n",
				nfull);
			log_seen(&st);
			destroy_state(&st);
			return -1;
		} else {
			rule = at_origin > 1 ? "mantıksal 0,0, kayıt sırasındaki ilk" : "mantıksal 0,0";
			for (int i = 0; i < st.n; i++) {
				if (eligible(&st.outs[i]) && st.outs[i].info.x == 0 &&
				    st.outs[i].info.y == 0) {
					chosen = i;
					break;
				}
			}
		}
	}
	if (chosen < 0 || !rule) {
		fprintf(stderr, "crd-cikti: çıktı seçilemedi\n");
		log_seen(&st);
		destroy_state(&st);
		return -1;
	}
	*out = st.outs[chosen].info;
	fprintf(stderr,
		"crd-cikti: seçildi %s \"%s\" konum %d,%d boyut %dx%d mod %dx%d kural=%s\n",
		out->name, out->description[0] ? out->description : "(açıklama yok)", out->x,
		out->y, out->width, out->height, out->mode_width, out->mode_height, rule);
	destroy_state(&st);
	return 0;
}
