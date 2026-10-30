#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "eis_keymap.h"

#include <libeis.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

static char keymap_source[768];

const char *
eis_xkb_keymap_source(void)
{
	return keymap_source;
}

static int
write_keymap_to_memfd(const char *xkb_text, size_t xkb_size, int *out_fd)
{
	int fd;
	void *map;

	if (xkb_size == 0) {
		return -EINVAL;
	}

	fd = memfd_create("crd-xkb-keymap", MFD_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}

	if (ftruncate(fd, (off_t)xkb_size) != 0) {
		int err = errno;
		close(fd);
		return -err;
	}

	map = mmap(NULL, xkb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		int err = errno;
		close(fd);
		return -err;
	}

	memcpy(map, xkb_text, xkb_size);
	munmap(map, xkb_size);

	*out_fd = fd;
	return 0;
}

static void
note_source(char *source, size_t source_len, const char *text)
{
	snprintf(source, source_len, "%s", text);
	snprintf(keymap_source, sizeof keymap_source, "%s", source);
}

struct kbd_src {
	struct wl_seat *seats[8];
	uint32_t caps[8];
	int caps_set[8];
	int nseats;
	int overflow;
	struct wl_keyboard *keyboard;
	char *text;
	uint32_t size;
	int got;
	int bad_format;
};

static int
seat_index(struct kbd_src *src, struct wl_seat *seat)
{
	for (int i = 0; i < src->nseats; i++) {
		if (src->seats[i] == seat) {
			return i;
		}
	}
	return -1;
}

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct kbd_src *src = data;
	int index = seat_index(src, seat);

	if (index < 0) {
		return;
	}
	src->caps[index] = caps;
	src->caps_set[index] = 1;
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void
kbd_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
	   uint32_t size)
{
	struct kbd_src *src = data;
	void *map;

	(void)keyboard;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > 4u * 1024u * 1024u) {
		src->bad_format = 1;
		fprintf(stderr, "crd-xkb: wl_keyboard keymap biçimi %u boyut %u\n", format, size);
		close(fd);
		return;
	}
	/* Sürüm 7 MAP_PRIVATE ister. Daha eski fd'de de geçerlidir. */
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "crd-xkb: wl_keyboard keymap eşlenemedi: %s\n", strerror(errno));
		close(fd);
		return;
	}
	free(src->text);
	src->text = malloc((size_t)size + 1u);
	if (!src->text) {
		munmap(map, size);
		close(fd);
		return;
	}
	memcpy(src->text, map, size);
	src->text[size] = '\0';
	src->size = size;
	src->got = 1;
	munmap(map, size);
	close(fd);
}

static void
kbd_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
	  struct wl_array *keys)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
}

static void
kbd_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
}

static void
kbd_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
	uint32_t state)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)time;
	(void)key;
	(void)state;
}

static void
kbd_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
	      uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;
}

static void
kbd_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
	(void)data;
	(void)keyboard;
	(void)rate;
	(void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kbd_keymap,
	.enter = kbd_enter,
	.leave = kbd_leave,
	.key = kbd_key,
	.modifiers = kbd_modifiers,
	.repeat_info = kbd_repeat_info,
};

static void
kbd_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
	   uint32_t version)
{
	struct kbd_src *src = data;

	(void)version;
	if (strcmp(interface, wl_seat_interface.name) != 0) {
		return;
	}
	if (src->nseats >= (int)(sizeof src->seats / sizeof src->seats[0])) {
		src->overflow = 1;
		return;
	}
	/* keymap olayı sürüm 1'de vardır. Daha yükseği bağlanmaz. */
	src->seats[src->nseats] = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	if (!src->seats[src->nseats]) {
		return;
	}
	wl_seat_add_listener(src->seats[src->nseats], &seat_listener, src);
	src->nseats++;
}

static void
kbd_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener kbd_registry_listener = {
	.global = kbd_global,
	.global_remove = kbd_global_remove,
};

/* Kompozitörün wl_keyboard.keymap olayı. Başarısızsa *out dokunulmaz. */
static int
keymap_from_wl_keyboard(struct xkb_context *ctx, struct xkb_keymap **out, char *source,
			size_t source_len)
{
	const char *wl_name = getenv("WAYLAND_DISPLAY");
	struct wl_display *display = NULL;
	struct wl_registry *registry = NULL;
	struct kbd_src src;
	struct xkb_keymap *keymap = NULL;
	const char *parse = NULL;
	int seat = -1;
	int ret = -ENOENT;
	char note[768];

	if (!wl_name || wl_name[0] == '\0') {
		fprintf(stderr, "crd-xkb: WAYLAND_DISPLAY yok, wl_keyboard keymap okunmadı\n");
		return -ENOENT;
	}
	memset(&src, 0, sizeof src);
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "crd-xkb: wl_display_connect başarısız (%s)\n", wl_name);
		return -ECONNREFUSED;
	}
	registry = wl_display_get_registry(display);
	if (!registry) {
		fprintf(stderr, "crd-xkb: wl_registry yok\n");
		goto out;
	}
	wl_registry_add_listener(registry, &kbd_registry_listener, &src);
	if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "crd-xkb: koltuk turu başarısız\n");
		goto out;
	}
	if (src.overflow) {
		fprintf(stderr, "crd-xkb: koltuk sayısı sınırı aşıldı\n");
		goto out;
	}
	for (int i = 0; i < src.nseats; i++) {
		if (src.caps_set[i] && (src.caps[i] & WL_SEAT_CAPABILITY_KEYBOARD) != 0) {
			seat = i;
			break;
		}
	}
	if (seat < 0) {
		fprintf(stderr, "crd-xkb: klavye yetenekli wl_seat yok\n");
		goto out;
	}
	src.keyboard = wl_seat_get_keyboard(src.seats[seat]);
	if (!src.keyboard) {
		fprintf(stderr, "crd-xkb: wl_seat_get_keyboard başarısız\n");
		goto out;
	}
	wl_keyboard_add_listener(src.keyboard, &keyboard_listener, &src);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "crd-xkb: keymap turu başarısız\n");
		goto out;
	}
	if (!src.got || !src.text) {
		fprintf(stderr, "crd-xkb: wl_keyboard keymap olayı gelmedi\n");
		goto out;
	}
	keymap = xkb_keymap_new_from_string(ctx, src.text, XKB_KEYMAP_FORMAT_TEXT_V1,
					    XKB_KEYMAP_COMPILE_NO_FLAGS);
	parse = "text_v1";
	if (!keymap) {
		keymap = xkb_keymap_new_from_string(ctx, src.text, XKB_KEYMAP_FORMAT_TEXT_V2,
						    XKB_KEYMAP_COMPILE_NO_FLAGS);
		parse = "text_v2";
		if (keymap) {
			fprintf(stderr, "crd-xkb: wl_keyboard metni text_v1 ayrışmadı, text_v2 ayrıştı\n");
		}
	}
	if (!keymap) {
		fprintf(stderr, "crd-xkb: wl_keyboard keymap ayrışmadı\n");
		ret = -EINVAL;
		goto out;
	}
	snprintf(note, sizeof note,
		 "wl_keyboard.keymap(WAYLAND_DISPLAY=%s, format=xkb_v1, parse=%s, bytes=%u)",
		 wl_name, parse, src.size);
	note_source(source, source_len, note);
	*out = keymap;
	keymap = NULL;
	ret = 0;

out:
	if (keymap) {
		xkb_keymap_unref(keymap);
	}
	free(src.text);
	if (src.keyboard) {
		wl_keyboard_destroy(src.keyboard);
	}
	for (int i = 0; i < src.nseats; i++) {
		if (src.seats[i]) {
			wl_seat_destroy(src.seats[i]);
		}
	}
	if (registry) {
		wl_registry_destroy(registry);
	}
	if (display) {
		wl_display_disconnect(display);
	}
	return ret;
}

static const char *
shown(const char *value)
{
	return value && value[0] ? value : "(yok)";
}

static int
keymap_from_env(struct xkb_context *ctx, struct xkb_keymap **out, char *source,
		size_t source_len)
{
	const char *rules = getenv("XKB_DEFAULT_RULES");
	const char *model = getenv("XKB_DEFAULT_MODEL");
	const char *layout = getenv("XKB_DEFAULT_LAYOUT");
	const char *variant = getenv("XKB_DEFAULT_VARIANT");
	const char *options = getenv("XKB_DEFAULT_OPTIONS");
	struct xkb_rule_names names;
	struct xkb_keymap *keymap;
	char note[768];

	if ((!rules || rules[0] == '\0') && (!model || model[0] == '\0') &&
	    (!layout || layout[0] == '\0') && (!variant || variant[0] == '\0') &&
	    (!options || options[0] == '\0')) {
		fprintf(stderr, "crd-xkb: XKB_DEFAULT_* yok\n");
		return -ENOENT;
	}
	memset(&names, 0, sizeof names);
	names.rules = rules && rules[0] ? rules : NULL;
	names.model = model && model[0] ? model : NULL;
	names.layout = layout && layout[0] ? layout : NULL;
	names.variant = variant && variant[0] ? variant : NULL;
	names.options = options && options[0] ? options : NULL;
	keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		fprintf(stderr, "crd-xkb: XKB_DEFAULT_* derlenemedi (layout=%s)\n", shown(layout));
		return -EINVAL;
	}
	snprintf(note, sizeof note,
		 "XKB_DEFAULT_* rules=%s model=%s layout=%s variant=%s options=%s",
		 shown(names.rules), shown(names.model), shown(names.layout), shown(names.variant),
		 shown(names.options));
	note_source(source, source_len, note);
	*out = keymap;
	return 0;
}

struct rmlvo {
	char rules[64];
	char model[64];
	char layout[128];
	char variant[64];
	char options[256];
	int have_rules;
	int have_model;
	int have_layout;
	int have_variant;
	int have_options;
	int from_file;
	int from_localectl;
};

static int
conf_option(const char *line, const char *key, char *dest, size_t dest_len)
{
	char pattern[80];
	const char *p;
	const char *end;
	size_t n;

	snprintf(pattern, sizeof pattern, "\"%s\"", key);
	p = strstr(line, pattern);
	if (!p) {
		return -1;
	}
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '"') {
		return -1;
	}
	p++;
	end = strchr(p, '"');
	if (!end) {
		return -1;
	}
	n = (size_t)(end - p);
	if (n >= dest_len) {
		return -1;
	}
	memcpy(dest, p, n);
	dest[n] = '\0';
	return 0;
}

static void
read_keyboard_conf(struct rmlvo *rm)
{
	static const char path[] = "/etc/X11/xorg.conf.d/00-keyboard.conf";
	FILE *file;
	char line[512];

	file = fopen(path, "r");
	if (!file) {
		fprintf(stderr, "crd-xkb: %s açılamadı: %s\n", path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof line, file)) {
		if (conf_option(line, "XkbRules", rm->rules, sizeof rm->rules) == 0) {
			rm->have_rules = 1;
			rm->from_file = 1;
		} else if (conf_option(line, "XkbModel", rm->model, sizeof rm->model) == 0) {
			rm->have_model = 1;
			rm->from_file = 1;
		} else if (conf_option(line, "XkbLayout", rm->layout, sizeof rm->layout) == 0) {
			rm->have_layout = 1;
			rm->from_file = 1;
		} else if (conf_option(line, "XkbVariant", rm->variant, sizeof rm->variant) == 0) {
			rm->have_variant = 1;
			rm->from_file = 1;
		} else if (conf_option(line, "XkbOptions", rm->options, sizeof rm->options) == 0) {
			rm->have_options = 1;
			rm->from_file = 1;
		}
	}
	fclose(file);
	if (!rm->have_layout || rm->layout[0] == '\0') {
		fprintf(stderr, "crd-xkb: %s içinde XkbLayout yok\n", path);
	}
}

static char *
trim_end(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
			 s[n - 1] == '\t')) {
		s[--n] = '\0';
	}
	return s;
}

static void
take_localectl_field(struct rmlvo *rm, const char *line, const char *label, char *dest,
		     size_t dest_len, int *have)
{
	const char *p = strstr(line, label);
	char copy[256];

	if (!p || *have) {
		return;
	}
	p += strlen(label);
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	snprintf(copy, sizeof copy, "%s", p);
	trim_end(copy);
	if (strlen(copy) >= dest_len) {
		fprintf(stderr, "crd-xkb: localectl alanı sığmadı (%s)\n", label);
		return;
	}
	memcpy(dest, copy, strlen(copy) + 1);
	*have = 1;
	rm->from_localectl = 1;
}

static void
read_localectl(struct rmlvo *rm)
{
	FILE *pipe;
	char line[512];
	int saw_layout_line = 0;

	pipe = popen("localectl status", "r");
	if (!pipe) {
		fprintf(stderr, "crd-xkb: localectl status başlatılamadı: %s\n", strerror(errno));
		return;
	}
	while (fgets(line, sizeof line, pipe)) {
		if (strstr(line, "X11 Layout:")) {
			saw_layout_line = 1;
		}
		take_localectl_field(rm, line, "X11 Layout:", rm->layout, sizeof rm->layout,
				     &rm->have_layout);
		take_localectl_field(rm, line, "X11 Model:", rm->model, sizeof rm->model,
				     &rm->have_model);
		take_localectl_field(rm, line, "X11 Variant:", rm->variant, sizeof rm->variant,
				     &rm->have_variant);
		take_localectl_field(rm, line, "X11 Options:", rm->options, sizeof rm->options,
				     &rm->have_options);
	}
	if (pclose(pipe) != 0) {
		fprintf(stderr, "crd-xkb: localectl status çıkış kodu başarısız\n");
	}
	if (!saw_layout_line) {
		fprintf(stderr, "crd-xkb: localectl status içinde X11 Layout satırı yok\n");
	}
}

static int
keymap_from_system(struct xkb_context *ctx, struct xkb_keymap **out, char *source,
		   size_t source_len)
{
	struct rmlvo rm;
	struct xkb_rule_names names;
	struct xkb_keymap *keymap;
	char note[768];
	const char *origin;

	memset(&rm, 0, sizeof rm);
	read_keyboard_conf(&rm);
	read_localectl(&rm);
	if (!rm.have_layout || rm.layout[0] == '\0') {
		fprintf(stderr, "crd-xkb: sistem düzeninde XkbLayout / X11 Layout yok\n");
		return -ENOENT;
	}
	memset(&names, 0, sizeof names);
	names.rules = rm.have_rules && rm.rules[0] ? rm.rules : NULL;
	names.model = rm.have_model && rm.model[0] ? rm.model : NULL;
	names.layout = rm.layout;
	names.variant = rm.have_variant ? rm.variant : NULL;
	names.options = rm.have_options ? rm.options : NULL;
	keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		fprintf(stderr, "crd-xkb: sistem düzeni derlenemedi (layout=%s)\n", rm.layout);
		return -EINVAL;
	}
	if (rm.from_file && rm.from_localectl) {
		origin = "00-keyboard.conf+localectl";
	} else if (rm.from_file) {
		origin = "00-keyboard.conf";
	} else {
		origin = "localectl";
	}
	snprintf(note, sizeof note,
		 "sistem düzeni kaynak=%s rules=%s model=%s layout=%s variant=%s options=%s",
		 origin, shown(names.rules), shown(names.model), shown(names.layout),
		 rm.have_variant ? shown(rm.variant) : "(yok)",
		 rm.have_options ? shown(rm.options) : "(yok)");
	note_source(source, source_len, note);
	*out = keymap;
	return 0;
}

static int
keymap_from_xkbcommon_default(struct xkb_context *ctx, struct xkb_keymap **out, char *source,
			      size_t source_len)
{
	struct xkb_keymap *keymap;

	fprintf(stderr,
		"crd-xkb: kompozitör haritası, XKB_DEFAULT_* ve sistem düzeni yok; "
		"xkbcommon varsayılanı\n");
	keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		fprintf(stderr, "crd-xkb: xkb_keymap_new_from_names(NULL) başarısız\n");
		return -EINVAL;
	}
	note_source(source, source_len,
		    "xkb_keymap_new_from_names(NULL) xkbcommon varsayılanı");
	*out = keymap;
	return 0;
}

int
crd_xkb_keymap_from_session(struct xkb_context *ctx, struct xkb_keymap **out, char *source,
			    size_t source_len)
{
	int rc;

	if (!ctx || !out || !source || source_len < 8) {
		return -EINVAL;
	}
	*out = NULL;
	source[0] = '\0';
	rc = keymap_from_wl_keyboard(ctx, out, source, source_len);
	if (rc == 0) {
		return 0;
	}
	rc = keymap_from_env(ctx, out, source, source_len);
	if (rc == 0) {
		return 0;
	}
	rc = keymap_from_system(ctx, out, source, source_len);
	if (rc == 0) {
		return 0;
	}
	return keymap_from_xkbcommon_default(ctx, out, source, source_len);
}

int
eis_xkb_attach_to_device(struct eis_device *device)
{
	struct xkb_context *xkb_ctx = NULL;
	struct xkb_keymap *keymap = NULL;
	char *xkb_text = NULL;
	size_t xkb_size;
	int xkb_fd = -1;
	int ret = -EIO;
	struct eis_keymap *eis_km = NULL;

	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_ctx) {
		return -ENOMEM;
	}
	ret = crd_xkb_keymap_from_session(xkb_ctx, &keymap, keymap_source, sizeof keymap_source);
	if (ret != 0 || !keymap) {
		ret = ret != 0 ? ret : -EINVAL;
		goto out;
	}

	xkb_text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!xkb_text || xkb_text[0] == '\0') {
		ret = -EINVAL;
		goto out;
	}

	xkb_size = strlen(xkb_text) + 1;
	ret = write_keymap_to_memfd(xkb_text, xkb_size, &xkb_fd);
	if (ret != 0) {
		goto out;
	}

	eis_km = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, xkb_fd, xkb_size);
	if (!eis_km) {
		ret = -EIO;
		goto out;
	}

	eis_keymap_add(eis_km);
	eis_keymap_unref(eis_km);
	ret = 0;

out:
	if (xkb_fd >= 0) {
		close(xkb_fd);
	}
	free(xkb_text);
	if (keymap) {
		xkb_keymap_unref(keymap);
	}
	if (xkb_ctx) {
		xkb_context_unref(xkb_ctx);
	}
	return ret;
}
