#include "giris.h"

#include "cikti.h"
#include "eis_keymap.h"
#include "fake-input-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <libeis.h>

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_OUTPUTS 16
#define MAX_GLOBALS 16
#define MAX_HELD 64

struct output_slot {
	struct wl_output *output;
	struct giris_output info;
	int have_mode;
	int have_name;
};

static struct {
	int inited;
	int live;
	char error[256];
	char wl_name[64];

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwlr_virtual_pointer_manager_v1 *pointer_mgr;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_mgr;
	/* KWin yolu: wlr sanal giriş yoksa org_kde_kwin_fake_input (v4+).
	   KWin sandbox'sız istemciye açar; authenticate bir kez gider. */
	struct org_kde_kwin_fake_input *fake;
	int fake_authed;
	struct zwlr_virtual_pointer_v1 *vptr;
	struct zwp_virtual_keyboard_v1 *vkbd;
	int keymap_sent;

	struct output_slot outputs[MAX_OUTPUTS];
	int noutputs;
	int chosen;
	struct giris_global globals[MAX_GLOBALS];
	int nglobals;

	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	char *keymap_text;
	size_t keymap_size;
	char keymap_source[768];
	char layouts[256];

	struct giris_rec *recs;
	size_t nrecs;
	size_t rec_cap;
	uint64_t wl_input_requests;

	int pointer_dirty;
	int mods_known;
	uint32_t last_dep, last_lat, last_lock, last_group;
	uint32_t held_keys[MAX_HELD];
	int nheld_keys;
	uint32_t held_buttons[MAX_HELD];
	int nheld_buttons;
} g;

static void set_error(const char *msg)
{
	snprintf(g.error, sizeof(g.error), "%s", msg);
}

static struct output_slot *selected_output(void)
{
	if (g.chosen < 0 || g.chosen >= g.noutputs) {
		return NULL;
	}
	return &g.outputs[g.chosen];
}

/* Yakalama ile aynı kural: crd_cikti_bul. Mantıksal konum ve boyut
 * wl_output modunun üzerine yazılır; sanal işaretçi o çıktıya bağlanır. */
static int choose_output(void)
{
	struct crd_cikti chosen;

	if (crd_cikti_bul(&chosen) != 0) {
		set_error("çıktı seçilemedi");
		return -ENOENT;
	}
	for (int i = 0; i < g.noutputs; i++) {
		if (!g.outputs[i].have_name || !g.outputs[i].have_mode ||
		    strcmp(g.outputs[i].info.name, chosen.name) != 0) {
			continue;
		}
		g.outputs[i].info.x = chosen.x;
		g.outputs[i].info.y = chosen.y;
		g.outputs[i].info.width = chosen.width;
		g.outputs[i].info.height = chosen.height;
		g.chosen = i;
		return 0;
	}
	set_error("seçilen çıktının wl_output kaydı yok");
	return -ENOENT;
}

static int record_push(const struct giris_rec *rec)
{
	if (g.nrecs == g.rec_cap) {
		size_t cap = g.rec_cap ? g.rec_cap * 2 : 16;
		struct giris_rec *grown = realloc(g.recs, cap * sizeof(*grown));

		if (!grown) {
			return -ENOMEM;
		}
		g.recs = grown;
		g.rec_cap = cap;
	}
	g.recs[g.nrecs++] = *rec;
	return 0;
}

static int keymap_memfd(void)
{
	int fd;
	void *map;

	if (!g.keymap_text || g.keymap_size == 0 || g.keymap_size > UINT32_MAX) {
		return -EINVAL;
	}
	fd = memfd_create("giris-xkb", MFD_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	if (ftruncate(fd, (off_t)g.keymap_size) != 0) {
		int err = errno;
		close(fd);
		return -err;
	}
	map = mmap(NULL, g.keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		int err = errno;
		close(fd);
		return -err;
	}
	memcpy(map, g.keymap_text, g.keymap_size);
	munmap(map, g.keymap_size);
	return fd;
}

static void count_request(void)
{
	g.wl_input_requests++;
}

static int ensure_pointer(void)
{
	struct output_slot *slot;

	if (g.vptr) {
		return 0;
	}
	slot = selected_output();
	if (!g.pointer_mgr || !g.seat || !slot || !slot->output) {
		return -ENODEV;
	}
	count_request();
	g.vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
	    g.pointer_mgr, g.seat, slot->output);
	if (!g.vptr) {
		return -EIO;
	}
	return 0;
}

static int ensure_keyboard(void)
{
	int fd;
	int rc;

	if (g.vkbd && g.keymap_sent) {
		return 0;
	}
	if (!g.keyboard_mgr || !g.seat) {
		return -ENODEV;
	}
	if (!g.vkbd) {
		count_request();
		g.vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		    g.keyboard_mgr, g.seat);
		if (!g.vkbd) {
			return -EIO;
		}
	}
	if (g.keymap_sent) {
		return 0;
	}
	rc = keymap_memfd();
	if (rc < 0) {
		return rc;
	}
	fd = rc;
	count_request();
	zwp_virtual_keyboard_v1_keymap(g.vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
	                               (uint32_t)g.keymap_size);
	close(fd);
	g.keymap_sent = 1;
	return 0;
}

static int flush_display(void)
{
	int rc;

	if (!g.display) {
		return -EINVAL;
	}
	rc = wl_display_flush(g.display);
	if (rc < 0 && errno != EAGAIN) {
		return -EIO;
	}
	return 0;
}

/* KWin dalı. fake_input küresel mantıksal koordinat alır; çıktı kökeni
   eklenir. Eksen kaynağı, durdurma, çerçeve ve değiştiriciler için istek
   yoktur: KWin değiştiricileri tuş olaylarından izler. Test edilmedi. */
static int marshal_fake(const struct giris_rec *rec)
{
	struct output_slot *slot;

	if (!g.fake_authed) {
		count_request();
		org_kde_kwin_fake_input_authenticate(g.fake, "CRD for Linux",
		                                     "uzak masaüstü");
		g.fake_authed = 1;
	}
	switch (rec->kind) {
	case GIRIS_REC_MOTION_ABSOLUTE:
		slot = selected_output();
		if (!slot) {
			return -ENODEV;
		}
		count_request();
		org_kde_kwin_fake_input_pointer_motion_absolute(
		    g.fake, wl_fixed_from_double((double)slot->info.x + rec->x),
		    wl_fixed_from_double((double)slot->info.y + rec->y));
		break;
	case GIRIS_REC_MOTION:
		count_request();
		org_kde_kwin_fake_input_pointer_motion(g.fake,
		                                       wl_fixed_from_double(rec->dx),
		                                       wl_fixed_from_double(rec->dy));
		break;
	case GIRIS_REC_BUTTON:
		count_request();
		org_kde_kwin_fake_input_button(g.fake, rec->button, rec->state);
		break;
	case GIRIS_REC_AXIS_DISCRETE:
	case GIRIS_REC_AXIS:
		count_request();
		org_kde_kwin_fake_input_axis(g.fake, rec->axis, rec->axis_fixed);
		break;
	case GIRIS_REC_AXIS_SOURCE:
	case GIRIS_REC_AXIS_STOP:
	case GIRIS_REC_POINTER_FRAME:
	case GIRIS_REC_MODIFIERS:
		return 0;
	case GIRIS_REC_KEY:
		count_request();
		org_kde_kwin_fake_input_keyboard_key(g.fake, rec->key, rec->state);
		break;
	default:
		return -EINVAL;
	}
	return flush_display();
}

/* Canlı dal. Kapı kapalıyken submit() buraya girmez. */
static int marshal(const struct giris_rec *rec)
{
	int rc;

	if (!g.live) {
		return -EPERM;
	}
	if (!g.pointer_mgr && g.fake) {
		return marshal_fake(rec);
	}

	switch (rec->kind) {
	case GIRIS_REC_MOTION_ABSOLUTE:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_motion_absolute(g.vptr, rec->time_ms, rec->x,
		                                         rec->y, rec->x_extent,
		                                         rec->y_extent);
		break;
	case GIRIS_REC_MOTION:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_motion(g.vptr, rec->time_ms,
		                               wl_fixed_from_double(rec->dx),
		                               wl_fixed_from_double(rec->dy));
		break;
	case GIRIS_REC_BUTTON:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_button(g.vptr, rec->time_ms, rec->button,
		                               rec->state);
		break;
	case GIRIS_REC_AXIS_DISCRETE:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_axis_discrete(g.vptr, rec->time_ms, rec->axis,
		                                      rec->axis_fixed, rec->discrete);
		break;
	case GIRIS_REC_AXIS:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_axis(g.vptr, rec->time_ms, rec->axis,
		                             rec->axis_fixed);
		break;
	case GIRIS_REC_AXIS_SOURCE:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_axis_source(g.vptr, rec->axis_source);
		break;
	case GIRIS_REC_AXIS_STOP:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_axis_stop(g.vptr, rec->time_ms, rec->axis);
		break;
	case GIRIS_REC_POINTER_FRAME:
		rc = ensure_pointer();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwlr_virtual_pointer_v1_frame(g.vptr);
		break;
	case GIRIS_REC_MODIFIERS:
		rc = ensure_keyboard();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwp_virtual_keyboard_v1_modifiers(g.vkbd, rec->mods_depressed,
		                                  rec->mods_latched, rec->mods_locked,
		                                  rec->group);
		break;
	case GIRIS_REC_KEY:
		rc = ensure_keyboard();
		if (rc != 0) {
			return rc;
		}
		count_request();
		zwp_virtual_keyboard_v1_key(g.vkbd, rec->time_ms, rec->key, rec->state);
		break;
	default:
		return -EINVAL;
	}
	return flush_display();
}

static int submit(const struct giris_rec *rec)
{
	if (!g.live) {
		return record_push(rec);
	}
	return marshal(rec);
}

static uint32_t event_time_ms(struct eis_event *event)
{
	return (uint32_t)(eis_event_get_time(event) / 1000u);
}

static int emit_axis_pair(uint32_t time_ms, uint32_t axis, enum giris_rec_kind kind,
                          double value, int32_t discrete, uint32_t source)
{
	struct giris_rec rec;
	int rc;

	memset(&rec, 0, sizeof(rec));
	rec.kind = kind;
	rec.time_ms = time_ms;
	rec.axis = axis;
	rec.axis_value = value;
	rec.axis_fixed = wl_fixed_from_double(value);
	rec.discrete = discrete;
	rc = submit(&rec);
	if (rc != 0) {
		return rc;
	}

	/* Hyprland 0.56.2 axis_source'u son eksene yazar; kaynak eksenden sonra gider. */
	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_AXIS_SOURCE;
	rec.time_ms = time_ms;
	rec.axis = axis;
	rec.axis_source = source;
	rc = submit(&rec);
	if (rc != 0) {
		return rc;
	}
	g.pointer_dirty = 1;
	return 0;
}

static int emit_scroll_component(uint32_t time_ms, uint32_t axis, double amount,
                                 int discrete_event)
{
	double units;
	int32_t clicks;
	int32_t v120;

	if (amount == 0.0) {
		return 0;
	}
	if (discrete_event) {
		v120 = (int32_t)amount;
		clicks = v120 / GIRIS_SCROLL_STEP_V120;
		units = (double)v120 / (double)GIRIS_SCROLL_STEP_V120 *
		        GIRIS_SCROLL_STEP_UNITS;
		return emit_axis_pair(time_ms, axis, GIRIS_REC_AXIS_DISCRETE, units,
		                      clicks, WL_POINTER_AXIS_SOURCE_WHEEL);
	}
	return emit_axis_pair(time_ms, axis, GIRIS_REC_AXIS, amount, 0,
	                      WL_POINTER_AXIS_SOURCE_CONTINUOUS);
}

static int emit_axis_stop(uint32_t time_ms, uint32_t axis, int cancel)
{
	struct giris_rec rec;

	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_AXIS_STOP;
	rec.time_ms = time_ms;
	rec.axis = axis;
	rec.state = cancel ? 1u : 0u;
	g.pointer_dirty = 1;
	return submit(&rec);
}

static int track_button(uint32_t button, int down)
{
	for (int i = 0; i < g.nheld_buttons; i++) {
		if (g.held_buttons[i] != button) {
			continue;
		}
		if (!down) {
			g.held_buttons[i] = g.held_buttons[g.nheld_buttons - 1];
			g.nheld_buttons--;
		}
		return 0;
	}
	if (!down) {
		return 0;
	}
	if (g.nheld_buttons >= MAX_HELD) {
		return -ENOSPC;
	}
	g.held_buttons[g.nheld_buttons++] = button;
	return 0;
}

static int track_key(uint32_t key, int down)
{
	for (int i = 0; i < g.nheld_keys; i++) {
		if (g.held_keys[i] != key) {
			continue;
		}
		if (!down) {
			g.held_keys[i] = g.held_keys[g.nheld_keys - 1];
			g.nheld_keys--;
		}
		return 0;
	}
	if (!down) {
		return 0;
	}
	if (g.nheld_keys >= MAX_HELD) {
		return -ENOSPC;
	}
	g.held_keys[g.nheld_keys++] = key;
	return 0;
}

static int sync_modifiers(uint32_t time_ms)
{
	uint32_t dep;
	uint32_t lat;
	uint32_t lock;
	uint32_t group;
	struct giris_rec rec;

	if (!g.xkb_state) {
		return -EINVAL;
	}
	dep = xkb_state_serialize_mods(g.xkb_state, XKB_STATE_MODS_DEPRESSED);
	lat = xkb_state_serialize_mods(g.xkb_state, XKB_STATE_MODS_LATCHED);
	lock = xkb_state_serialize_mods(g.xkb_state, XKB_STATE_MODS_LOCKED);
	group = xkb_state_serialize_layout(g.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	if (g.mods_known && dep == g.last_dep && lat == g.last_lat &&
	    lock == g.last_lock && group == g.last_group) {
		return 0;
	}
	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_MODIFIERS;
	rec.time_ms = time_ms;
	rec.mods_depressed = dep;
	rec.mods_latched = lat;
	rec.mods_locked = lock;
	rec.group = group;
	g.last_dep = dep;
	g.last_lat = lat;
	g.last_lock = lock;
	g.last_group = group;
	g.mods_known = 1;
	return submit(&rec);
}

static struct eis_region *region_containing(struct eis_device *device, double x,
                                            double y)
{
	for (size_t i = 0;; i++) {
		struct eis_region *region = eis_device_get_region(device, i);

		if (!region) {
			return NULL;
		}
		if (eis_region_contains(region, x, y)) {
			return region;
		}
	}
}

static int handle_absolute(struct eis_event *event)
{
	struct eis_device *device = eis_event_get_device(event);
	struct eis_region *region;
	struct giris_rec rec;
	double x;
	double y;
	double local_x;
	double local_y;
	uint32_t origin_x;
	uint32_t origin_y;
	uint32_t width;
	uint32_t height;

	if (!device) {
		return -EINVAL;
	}
	x = eis_event_pointer_get_absolute_x(event);
	y = eis_event_pointer_get_absolute_y(event);
	region = region_containing(device, x, y);
	if (!region) {
		return -ERANGE;
	}
	origin_x = eis_region_get_x(region);
	origin_y = eis_region_get_y(region);
	width = eis_region_get_width(region);
	height = eis_region_get_height(region);
	if (width == 0 || height == 0) {
		return -ERANGE;
	}
	local_x = x - (double)origin_x;
	local_y = y - (double)origin_y;
	if (local_x < 0 || local_y < 0 || local_x > (double)width ||
	    local_y > (double)height) {
		return -ERANGE;
	}

	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_MOTION_ABSOLUTE;
	rec.time_ms = event_time_ms(event);
	rec.x = (uint32_t)(local_x + 0.5);
	rec.y = (uint32_t)(local_y + 0.5);
	if (rec.x > width) {
		rec.x = width;
	}
	if (rec.y > height) {
		rec.y = height;
	}
	rec.x_extent = width;
	rec.y_extent = height;
	g.pointer_dirty = 1;
	return submit(&rec);
}

static int handle_button(struct eis_event *event)
{
	struct giris_rec rec;
	int press;
	int rc;

	press = eis_event_button_get_is_press(event);
	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_BUTTON;
	rec.time_ms = event_time_ms(event);
	rec.button = eis_event_button_get_button(event);
	rec.state = press ? WL_POINTER_BUTTON_STATE_PRESSED
	                  : WL_POINTER_BUTTON_STATE_RELEASED;
	rc = track_button(rec.button, press);
	if (rc != 0) {
		return rc;
	}
	g.pointer_dirty = 1;
	return submit(&rec);
}

static int submit_key(uint32_t time_ms, uint32_t key, int press)
{
	struct giris_rec rec;

	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_KEY;
	rec.time_ms = time_ms;
	rec.key = key;
	rec.state = press ? WL_KEYBOARD_KEY_STATE_PRESSED
	                  : WL_KEYBOARD_KEY_STATE_RELEASED;
	return submit(&rec);
}

static int key_is_held(uint32_t key)
{
	for (int i = 0; i < g.nheld_keys; i++) {
		if (g.held_keys[i] == key) {
			return 1;
		}
	}
	return 0;
}

static int inject_evdev(uint32_t time_ms, uint32_t key, int press)
{
	int rc;

	if (key == 0 || key > KEY_MAX || !g.xkb_state) {
		return -EINVAL;
	}
	if (!press) {
		/* Hyprland Super kısayolu bırakışta eşleşir ve o anda Super
		   değiştiricisinin hâlâ basılı olmasını ister. Önce değiştiriciyi
		   silmek Cmd/Super dokunuşunu düşürür. */
		rc = track_key(key, 0);
		if (rc != 0) {
			return rc;
		}
		rc = submit_key(time_ms, key, 0);
		if (rc != 0) {
			return rc;
		}
		xkb_state_update_key(g.xkb_state, (xkb_keycode_t)(key + 8), XKB_KEY_UP);
		return sync_modifiers(time_ms);
	}
	xkb_state_update_key(g.xkb_state, (xkb_keycode_t)(key + 8), XKB_KEY_DOWN);
	rc = sync_modifiers(time_ms);
	if (rc != 0) {
		return rc;
	}
	rc = track_key(key, 1);
	if (rc != 0) {
		return rc;
	}
	return submit_key(time_ms, key, 1);
}

static int handle_key(struct eis_event *event)
{
	uint32_t key = eis_event_keyboard_get_key(event);

	if (key == 0 || key > KEY_MAX) {
		return -EINVAL;
	}
	return inject_evdev(event_time_ms(event), key,
	                    eis_event_keyboard_get_key_is_press(event));
}

static uint32_t evdev_for_mod(xkb_mod_index_t index)
{
	const char *name;

	if (!g.keymap) {
		return 0;
	}
	name = xkb_keymap_mod_get_name(g.keymap, index);
	if (!name) {
		return 0;
	}
	if (strcmp(name, XKB_MOD_NAME_SHIFT) == 0) {
		return KEY_LEFTSHIFT;
	}
	if (strcmp(name, XKB_MOD_NAME_CTRL) == 0) {
		return KEY_LEFTCTRL;
	}
	if (strcmp(name, XKB_MOD_NAME_ALT) == 0) {
		return KEY_LEFTALT;
	}
	if (strcmp(name, XKB_MOD_NAME_LOGO) == 0) {
		return KEY_LEFTMETA;
	}
	if (strcmp(name, "Mod5") == 0 || strcmp(name, "LevelThree") == 0) {
		return KEY_RIGHTALT;
	}
	return 0;
}

static int find_sym(xkb_keysym_t sym, uint32_t *evdev, xkb_mod_mask_t *mask)
{
	xkb_keycode_t min;
	xkb_keycode_t max;
	xkb_layout_index_t layout = 0;

	if (!g.keymap || !evdev || !mask) {
		return -EINVAL;
	}
	if (g.xkb_state) {
		layout = xkb_state_serialize_layout(g.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	}
	min = xkb_keymap_min_keycode(g.keymap);
	max = xkb_keymap_max_keycode(g.keymap);
	for (xkb_keycode_t key = min; key <= max; key++) {
		xkb_level_index_t levels =
		    xkb_keymap_num_levels_for_key(g.keymap, key, layout);

		for (xkb_level_index_t level = 0; level < levels; level++) {
			const xkb_keysym_t *syms = NULL;
			int count = xkb_keymap_key_get_syms_by_level(g.keymap, key, layout,
			                                             level, &syms);
			xkb_mod_mask_t masks[1] = {0};

			if (count < 1 || !syms || syms[0] != sym || key < 8) {
				continue;
			}
			if (xkb_keymap_key_get_mods_for_level(g.keymap, key, layout, level,
			                                      masks, 1) < 1) {
				masks[0] = 0;
			}
			*evdev = (uint32_t)key - 8u;
			*mask = masks[0];
			if (*evdev == 0 || *evdev > KEY_MAX) {
				return -EINVAL;
			}
			return 0;
		}
	}
	return -ENOENT;
}

static int press_mask(uint32_t time_ms, xkb_mod_mask_t mask, uint32_t *extra,
                      int *nextra)
{
	xkb_mod_index_t count;
	int rc;

	if (!g.keymap) {
		return -EINVAL;
	}
	count = xkb_keymap_num_mods(g.keymap);
	for (xkb_mod_index_t i = 0; i < count; i++) {
		uint32_t key;

		if ((mask & (1u << i)) == 0) {
			continue;
		}
		key = evdev_for_mod(i);
		if (key == 0 || key_is_held(key)) {
			continue;
		}
		rc = inject_evdev(time_ms, key, 1);
		if (rc != 0) {
			return rc;
		}
		if (*nextra < 8) {
			extra[(*nextra)++] = key;
		}
	}
	return 0;
}

static int release_extra(uint32_t time_ms, const uint32_t *extra, int nextra)
{
	for (int i = nextra - 1; i >= 0; i--) {
		int rc = inject_evdev(time_ms, extra[i], 0);

		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int tap_codepoint(uint32_t time_ms, uint32_t cp)
{
	uint32_t evdev = 0;
	xkb_mod_mask_t mask = 0;
	uint32_t extra[8];
	int nextra = 0;
	xkb_keysym_t sym;
	int rc;

	if (cp == '\n' || cp == '\r') {
		evdev = KEY_ENTER;
	} else if (cp == '\t') {
		evdev = KEY_TAB;
	} else if (cp == '\b' || cp == 0x7fu) {
		evdev = KEY_BACKSPACE;
	} else {
		sym = xkb_utf32_to_keysym(cp);
		if (sym == XKB_KEY_NoSymbol) {
			return 0;
		}
		if (find_sym(sym, &evdev, &mask) != 0) {
			return 0;
		}
	}
	rc = press_mask(time_ms, mask, extra, &nextra);
	if (rc != 0) {
		return rc;
	}
	rc = inject_evdev(time_ms, evdev, 1);
	if (rc != 0) {
		return rc;
	}
	rc = inject_evdev(time_ms, evdev, 0);
	if (rc != 0) {
		return rc;
	}
	return release_extra(time_ms, extra, nextra);
}

static int utf8_next(const char **cursor, uint32_t *cp)
{
	const unsigned char *s = (const unsigned char *)*cursor;
	uint32_t value;
	int need;

	if (*s == 0) {
		return 0;
	}
	if (*s < 0x80) {
		*cp = *s;
		*cursor += 1;
		return 1;
	}
	if ((*s & 0xe0) == 0xc0) {
		value = *s & 0x1f;
		need = 1;
	} else if ((*s & 0xf0) == 0xe0) {
		value = *s & 0x0f;
		need = 2;
	} else if ((*s & 0xf8) == 0xf0) {
		value = *s & 0x07;
		need = 3;
	} else {
		return -EINVAL;
	}
	for (int i = 1; i <= need; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			return -EINVAL;
		}
		value = (value << 6) | (s[i] & 0x3f);
	}
	if ((need == 1 && value < 0x80) || (need == 2 && value < 0x800) ||
	    (need == 3 && value < 0x10000) || value > 0x10ffff ||
	    (value >= 0xd800 && value <= 0xdfff)) {
		return -EINVAL;
	}
	*cp = value;
	*cursor += (size_t)need + 1u;
	return 1;
}

struct sym_down {
	uint32_t keysym;
	uint32_t evdev;
	uint32_t extra[8];
	int nextra;
	int used;
};

static struct sym_down sym_down[16];

static int handle_text_utf8(struct eis_event *event)
{
	const char *cursor = eis_event_text_get_utf8(event);
	uint32_t time_ms;
	int step;

	if (!cursor) {
		return -EINVAL;
	}
	time_ms = event_time_ms(event);
	for (;;) {
		uint32_t cp = 0;
		int rc;

		step = utf8_next(&cursor, &cp);
		if (step == 0) {
			return 0;
		}
		if (step < 0) {
			return step;
		}
		rc = tap_codepoint(time_ms, cp);
		if (rc != 0) {
			return rc;
		}
	}
}

static int handle_text_keysym(struct eis_event *event)
{
	uint32_t keysym = eis_event_text_get_keysym(event);
	int press = eis_event_text_get_keysym_is_press(event);
	uint32_t time_ms = event_time_ms(event);
	struct sym_down *slot = NULL;
	uint32_t evdev = 0;
	xkb_mod_mask_t mask = 0;
	int rc;

	for (int i = 0; i < 16; i++) {
		if (sym_down[i].used && sym_down[i].keysym == keysym) {
			slot = &sym_down[i];
			break;
		}
	}
	if (!press) {
		if (!slot) {
			return 0;
		}
		rc = inject_evdev(time_ms, slot->evdev, 0);
		if (rc != 0) {
			return rc;
		}
		rc = release_extra(time_ms, slot->extra, slot->nextra);
		slot->used = 0;
		return rc;
	}
	if (slot) {
		return 0;
	}
	if (find_sym((xkb_keysym_t)keysym, &evdev, &mask) != 0) {
		return 0;
	}
	for (int i = 0; i < 16; i++) {
		if (!sym_down[i].used) {
			slot = &sym_down[i];
			break;
		}
	}
	if (!slot) {
		return -ENOSPC;
	}
	memset(slot, 0, sizeof(*slot));
	slot->used = 1;
	slot->keysym = keysym;
	slot->evdev = evdev;
	rc = press_mask(time_ms, mask, slot->extra, &slot->nextra);
	if (rc != 0) {
		slot->used = 0;
		return rc;
	}
	rc = inject_evdev(time_ms, evdev, 1);
	if (rc != 0) {
		release_extra(time_ms, slot->extra, slot->nextra);
		slot->used = 0;
	}
	return rc;
}

static int handle_scroll(struct eis_event *event, int discrete_event)
{
	uint32_t time_ms = event_time_ms(event);
	double dx;
	double dy;
	int rc;

	if (discrete_event) {
		dx = (double)eis_event_scroll_get_discrete_dx(event);
		dy = (double)eis_event_scroll_get_discrete_dy(event);
	} else {
		dx = eis_event_scroll_get_dx(event);
		dy = eis_event_scroll_get_dy(event);
	}
	rc = emit_scroll_component(time_ms, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx,
	                           discrete_event);
	if (rc != 0) {
		return rc;
	}
	return emit_scroll_component(time_ms, WL_POINTER_AXIS_VERTICAL_SCROLL, dy,
	                             discrete_event);
}

static int handle_scroll_end(struct eis_event *event, int cancel)
{
	uint32_t time_ms = event_time_ms(event);
	int rc = 0;

	if (eis_event_scroll_get_stop_x(event)) {
		rc = emit_axis_stop(time_ms, WL_POINTER_AXIS_HORIZONTAL_SCROLL, cancel);
		if (rc != 0) {
			return rc;
		}
	}
	if (eis_event_scroll_get_stop_y(event)) {
		rc = emit_axis_stop(time_ms, WL_POINTER_AXIS_VERTICAL_SCROLL, cancel);
	}
	return rc;
}

static int handle_frame(struct eis_event *event)
{
	struct giris_rec rec;
	int rc = 0;

	if (!g.pointer_dirty) {
		return 0;
	}
	memset(&rec, 0, sizeof(rec));
	rec.kind = GIRIS_REC_POINTER_FRAME;
	rec.time_ms = event_time_ms(event);
	rc = submit(&rec);
	g.pointer_dirty = 0;
	return rc;
}

static int connect_wayland(void);

static void giris_drop_wayland(void)
{
	if (g.display)
		wl_display_disconnect(g.display);
	g.display = NULL;
	g.registry = NULL;
	g.seat = NULL;
	g.pointer_mgr = NULL;
	g.keyboard_mgr = NULL;
	g.vptr = NULL;
	g.vkbd = NULL;
	g.keymap_sent = 0;
	g.noutputs = 0;
	g.nglobals = 0;
	memset(g.outputs, 0, sizeof g.outputs);
	g.nheld_keys = 0;
	g.nheld_buttons = 0;
	g.pointer_dirty = 0;
}

static int read_ekran_path(char *path, size_t cap)
{
	FILE *file;
	size_t n;

	file = fopen("/run/crd-for-linux/ekran", "r");
	if (!file)
		return -1;
	if (!fgets(path, (int)cap, file)) {
		fclose(file);
		return -1;
	}
	fclose(file);
	n = strlen(path);
	while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r'))
		path[--n] = '\0';
	if (path[0] != '/')
		return -1;
	return 0;
}

static int giris_follow_ekran(void)
{
	char path[64];
	int dead;

	if (read_ekran_path(path, sizeof path) != 0)
		return 0;
	dead = !g.display || wl_display_get_error(g.display) != 0;
	if (!dead && strcmp(path, g.wl_name) == 0)
		return 0;
	giris_drop_wayland();
	if (setenv("WAYLAND_DISPLAY", path, 1) != 0)
		return -errno;
	if (connect_wayland() != 0) {
		giris_drop_wayland();
		g.wl_name[0] = '\0';
		return -EAGAIN;
	}
	return 0;
}

int giris_handle_ei_event(struct eis_event *event)
{
	struct giris_rec rec;

	if (!g.inited) {
		return -EINVAL;
	}
	if (!event) {
		return -EINVAL;
	}
	if (giris_follow_ekran() != 0 || !g.display) {
		return -EAGAIN;
	}

	switch (eis_event_get_type(event)) {
	case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
		return handle_absolute(event);
	case EIS_EVENT_POINTER_MOTION:
		memset(&rec, 0, sizeof(rec));
		rec.kind = GIRIS_REC_MOTION;
		rec.time_ms = event_time_ms(event);
		rec.dx = eis_event_pointer_get_dx(event);
		rec.dy = eis_event_pointer_get_dy(event);
		g.pointer_dirty = 1;
		return submit(&rec);
	case EIS_EVENT_BUTTON_BUTTON:
		return handle_button(event);
	case EIS_EVENT_SCROLL_DISCRETE:
		return handle_scroll(event, 1);
	case EIS_EVENT_SCROLL_DELTA:
		return handle_scroll(event, 0);
	case EIS_EVENT_SCROLL_STOP:
		return handle_scroll_end(event, 0);
	case EIS_EVENT_SCROLL_CANCEL:
		return handle_scroll_end(event, 1);
	case EIS_EVENT_KEYBOARD_KEY:
		return handle_key(event);
	case EIS_EVENT_TEXT_UTF8:
		return handle_text_utf8(event);
	case EIS_EVENT_TEXT_KEYSYM:
		return handle_text_keysym(event);
	case EIS_EVENT_FRAME:
		return handle_frame(event);
	default:
		return 0;
	}
}

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t phys_w, int32_t phys_h,
                            int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
	struct output_slot *slot = data;

	(void)output;
	(void)phys_w;
	(void)phys_h;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	slot->info.x = x;
	slot->info.y = y;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
	struct output_slot *slot = data;

	(void)output;
	if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
		return;
	}
	slot->info.width = width;
	slot->info.height = height;
	slot->info.refresh = refresh;
	slot->have_mode = 1;
}

static void output_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t scale)
{
	struct output_slot *slot = data;

	(void)output;
	slot->info.scale = scale;
}

static void output_name(void *data, struct wl_output *output, const char *name)
{
	struct output_slot *slot = data;

	(void)output;
	snprintf(slot->info.name, sizeof(slot->info.name), "%s", name ? name : "");
	slot->have_name = 1;
}

static void output_description(void *data, struct wl_output *output,
                               const char *description)
{
	(void)data;
	(void)output;
	(void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	(void)data;
	(void)seat;
	(void)caps;
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void remember_global(const char *interface, uint32_t name,
                            uint32_t advertised, uint32_t bound)
{
	struct giris_global *slot;

	if (g.nglobals >= MAX_GLOBALS) {
		return;
	}
	slot = &g.globals[g.nglobals++];
	snprintf(slot->interface, sizeof(slot->interface), "%s", interface);
	slot->name = name;
	slot->advertised_version = advertised;
	slot->bound_version = bound;
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
	(void)data;

	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		if (g.pointer_mgr || version < 2) {
			return;
		}
		g.pointer_mgr = wl_registry_bind(
		    registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
		if (g.pointer_mgr) {
			remember_global(interface, name, version, 2);
		}
		return;
	}
	if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		if (g.keyboard_mgr || version < 1) {
			return;
		}
		g.keyboard_mgr = wl_registry_bind(
		    registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
		if (g.keyboard_mgr) {
			remember_global(interface, name, version, 1);
		}
		return;
	}
	if (strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
		/* keyboard_key v4 ile gelir; daha eskisi klavyesiz kalırdı. */
		if (g.fake || version < 4) {
			return;
		}
		g.fake = wl_registry_bind(registry, name,
		                          &org_kde_kwin_fake_input_interface, 4);
		if (g.fake) {
			remember_global(interface, name, version, 4);
		}
		return;
	}
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		uint32_t bound = version < 2 ? version : 2;

		if (g.seat || bound < 1) {
			return;
		}
		g.seat = wl_registry_bind(registry, name, &wl_seat_interface, bound);
		if (!g.seat) {
			return;
		}
		wl_seat_add_listener(g.seat, &seat_listener, NULL);
		remember_global(interface, name, version, bound);
		return;
	}
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output_slot *slot;
		uint32_t bound = version < 4 ? version : 4;

		if (bound < 1) {
			return;
		}
		if (g.noutputs >= MAX_OUTPUTS) {
			fprintf(stderr, "giris: wl_output sayısı %d sınırını aştı\n", MAX_OUTPUTS);
			return;
		}
		slot = &g.outputs[g.noutputs];
		memset(slot, 0, sizeof(*slot));
		slot->info.scale = 1;
		slot->output =
		    wl_registry_bind(registry, name, &wl_output_interface, bound);
		if (!slot->output) {
			return;
		}
		wl_output_add_listener(slot->output, &output_listener, slot);
		remember_global(interface, name, version, bound);
		g.noutputs++;
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void fill_layouts(struct xkb_keymap *keymap)
{
	xkb_layout_index_t count = xkb_keymap_num_layouts(keymap);
	size_t used = 0;

	g.layouts[0] = '\0';
	for (xkb_layout_index_t index = 0; index < count; index++) {
		const char *name = xkb_keymap_layout_get_name(keymap, index);
		int wrote;

		if (!name) {
			name = "";
		}
		wrote = snprintf(g.layouts + used, sizeof(g.layouts) - used, "%s%s",
		                 used ? "," : "", name);
		if (wrote < 0 || (size_t)wrote >= sizeof(g.layouts) - used) {
			break;
		}
		used += (size_t)wrote;
	}
}

static int load_session_keymap(void)
{
	struct xkb_context *ctx = NULL;
	struct xkb_keymap *keymap = NULL;
	char *text = NULL;
	int ret = -EIO;

	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ctx) {
		set_error("xkb_context_new başarısız");
		return -ENOMEM;
	}
	ret = crd_xkb_keymap_from_session(ctx, &keymap, g.keymap_source, sizeof g.keymap_source);
	if (ret != 0 || !keymap) {
		set_error("klavye haritası yüklenemedi");
		ret = ret != 0 ? ret : -EINVAL;
		goto out;
	}

	text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!text || text[0] == '\0') {
		set_error("XKB metni boş");
		ret = -EINVAL;
		goto out;
	}
	fill_layouts(keymap);
	g.keymap = keymap;
	keymap = NULL;
	g.keymap_text = text;
	text = NULL;
	g.keymap_size = strlen(g.keymap_text) + 1;
	g.xkb_state = xkb_state_new(g.keymap);
	if (!g.xkb_state) {
		set_error("xkb_state_new başarısız");
		ret = -ENOMEM;
		goto out;
	}
	ret = 0;

out:
	free(text);
	if (keymap) {
		xkb_keymap_unref(keymap);
	}
	if (ret != 0) {
		free(g.keymap_text);
		g.keymap_text = NULL;
		g.keymap_size = 0;
		if (g.keymap) {
			xkb_keymap_unref(g.keymap);
			g.keymap = NULL;
		}
	}
	if (ctx) {
		xkb_context_unref(ctx);
	}
	return ret;
}

static int connect_wayland(void)
{
	const char *name = getenv("WAYLAND_DISPLAY");
	int rc;

	if (!name || name[0] == '\0') {
		set_error("WAYLAND_DISPLAY yok");
		return -ENOENT;
	}
	snprintf(g.wl_name, sizeof(g.wl_name), "%s", name);
	g.display = wl_display_connect(NULL);
	if (!g.display) {
		set_error("wl_display_connect başarısız");
		return -ECONNREFUSED;
	}
	g.registry = wl_display_get_registry(g.display);
	if (!g.registry) {
		set_error("wl_display_get_registry başarısız");
		return -EIO;
	}
	wl_registry_add_listener(g.registry, &registry_listener, NULL);
	for (int i = 0; i < 3; i++) {
		if (wl_display_roundtrip(g.display) < 0) {
			set_error("wl_display_roundtrip başarısız");
			return -EIO;
		}
	}
	if (wl_display_get_error(g.display) != 0) {
		set_error("Wayland protokol hatası");
		return -EPROTO;
	}
	/* İki yoldan biri tam olmalı: wlr sanal fare + klavye ya da KWin
	   fake_input. Yarım wlr yolu KWin'e düşmez. */
	if (g.pointer_mgr || g.keyboard_mgr || !g.fake) {
		if (!g.pointer_mgr) {
			set_error(g.fake ? "zwlr_virtual_pointer_manager_v1 v2 bağlanamadı"
			                 : "zwlr_virtual_pointer_manager_v1 v2 ve org_kde_kwin_fake_input v4 yok");
			return -ENOENT;
		}
		if (!g.keyboard_mgr) {
			set_error("zwp_virtual_keyboard_manager_v1 bağlanamadı");
			return -ENOENT;
		}
	}
	if (!g.seat) {
		set_error("wl_seat bağlanamadı");
		return -ENOENT;
	}
	if (choose_output() != 0) {
		return -ENOENT;
	}
	rc = 0;
	return rc;
}

int giris_init(void)
{
	int rc;

	if (g.display || g.inited) {
		return -EBUSY;
	}
	memset(&g, 0, sizeof(g));
	g.chosen = -1;
	rc = connect_wayland();
	if (rc != 0) {
		return rc;
	}
	rc = load_session_keymap();
	if (rc != 0) {
		return rc;
	}
	if (!g.keymap_text || g.keymap_text[0] == '\0' || g.keymap_size < 2 ||
	    g.keymap_source[0] == '\0') {
		set_error("keymap boş");
		return -EINVAL;
	}
	g.live = 0;
	g.inited = 1;
	g.error[0] = '\0';
	return 0;
}

const char *giris_init_error(void)
{
	return g.error;
}

static void release_held_live(void)
{
	struct giris_rec rec;

	while (g.nheld_keys > 0) {
		uint32_t key = g.held_keys[g.nheld_keys - 1];

		inject_evdev(0, key, 0);
	}
	while (g.nheld_buttons > 0) {
		uint32_t button = g.held_buttons[--g.nheld_buttons];

		memset(&rec, 0, sizeof(rec));
		rec.kind = GIRIS_REC_BUTTON;
		rec.button = button;
		rec.state = WL_POINTER_BUTTON_STATE_RELEASED;
		g.pointer_dirty = 1;
		submit(&rec);
	}
	if (g.pointer_dirty) {
		memset(&rec, 0, sizeof(rec));
		rec.kind = GIRIS_REC_POINTER_FRAME;
		submit(&rec);
		g.pointer_dirty = 0;
	}
}

void giris_fini(void)
{
	if (g.live) {
		release_held_live();
		flush_display();
	}
	g.nheld_keys = 0;
	g.nheld_buttons = 0;
	if (g.vptr) {
		zwlr_virtual_pointer_v1_destroy(g.vptr);
		g.vptr = NULL;
	}
	if (g.vkbd) {
		zwp_virtual_keyboard_v1_destroy(g.vkbd);
		g.vkbd = NULL;
	}
	for (int i = 0; i < g.noutputs; i++) {
		if (g.outputs[i].output) {
			wl_output_destroy(g.outputs[i].output);
			g.outputs[i].output = NULL;
		}
	}
	if (g.seat) {
		wl_seat_destroy(g.seat);
		g.seat = NULL;
	}
	if (g.pointer_mgr) {
		zwlr_virtual_pointer_manager_v1_destroy(g.pointer_mgr);
		g.pointer_mgr = NULL;
	}
	if (g.fake) {
		org_kde_kwin_fake_input_destroy(g.fake);
		g.fake = NULL;
		g.fake_authed = 0;
	}
	if (g.keyboard_mgr) {
		zwp_virtual_keyboard_manager_v1_destroy(g.keyboard_mgr);
		g.keyboard_mgr = NULL;
	}
	if (g.registry) {
		wl_registry_destroy(g.registry);
		g.registry = NULL;
	}
	if (g.display) {
		wl_display_disconnect(g.display);
		g.display = NULL;
	}
	free(g.recs);
	g.recs = NULL;
	g.nrecs = 0;
	g.rec_cap = 0;
	free(g.keymap_text);
	g.keymap_text = NULL;
	g.keymap_size = 0;
	if (g.xkb_state) {
		xkb_state_unref(g.xkb_state);
		g.xkb_state = NULL;
	}
	if (g.keymap) {
		xkb_keymap_unref(g.keymap);
		g.keymap = NULL;
	}
	g.inited = 0;
	g.live = 0;
}

const char *giris_wayland_display(void)
{
	return g.wl_name;
}

size_t giris_global_count(void)
{
	return (size_t)g.nglobals;
}

const struct giris_global *giris_global_at(size_t index)
{
	if (index >= (size_t)g.nglobals) {
		return NULL;
	}
	return &g.globals[index];
}

size_t giris_output_count(void)
{
	return (size_t)g.noutputs;
}

const struct giris_output *giris_output_at(size_t index)
{
	if (index >= (size_t)g.noutputs) {
		return NULL;
	}
	return &g.outputs[index].info;
}

size_t giris_keymap_size(void)
{
	return g.keymap_size;
}

const char *giris_keymap_text(void)
{
	return g.keymap_text ? g.keymap_text : "";
}

const char *giris_keymap_source(void)
{
	return g.keymap_source;
}

const char *giris_keymap_layouts(void)
{
	return g.layouts;
}

int giris_keymap_find_evdev(uint32_t keysym, uint32_t *evdev, char *key_name,
                            size_t key_name_len, char *sym_name, size_t sym_name_len)
{
	xkb_keycode_t min;
	xkb_keycode_t max;

	if (!g.keymap || !evdev) {
		return -EINVAL;
	}
	min = xkb_keymap_min_keycode(g.keymap);
	max = xkb_keymap_max_keycode(g.keymap);
	for (xkb_keycode_t key = min; key <= max; key++) {
		const xkb_keysym_t *syms = NULL;
		int count =
		    xkb_keymap_key_get_syms_by_level(g.keymap, key, 0, 0, &syms);
		const char *name;

		if (count < 1 || !syms || syms[0] != (xkb_keysym_t)keysym) {
			continue;
		}
		if (key < 8) {
			return -EINVAL;
		}
		*evdev = (uint32_t)key - 8u;
		if (*evdev == 0 || *evdev > KEY_MAX) {
			return -EINVAL;
		}
		if (key_name && key_name_len > 0) {
			name = xkb_keymap_key_get_name(g.keymap, key);
			snprintf(key_name, key_name_len, "%s", name ? name : "");
		}
		if (sym_name && sym_name_len > 0) {
			if (xkb_keysym_get_name((xkb_keysym_t)keysym, sym_name,
			                        sym_name_len) < 0) {
				sym_name[0] = '\0';
			}
		}
		return 0;
	}
	return -ENOENT;
}

int giris_set_live_send(int enable)
{
	if (!g.inited) {
		return -EINVAL;
	}
	g.live = enable ? 1 : 0;
	return 0;
}

int giris_live_send_enabled(void)
{
	return g.live;
}

size_t giris_record_count(void)
{
	return g.nrecs;
}

const struct giris_rec *giris_record_at(size_t index)
{
	if (index >= g.nrecs) {
		return NULL;
	}
	return &g.recs[index];
}

uint64_t giris_wl_input_requests(void)
{
	return g.wl_input_requests;
}

#ifdef GIRIS_KEYMAP_PROBE
int main(void)
{
	const char *display = getenv("DISPLAY");
	int rc;

	rc = load_session_keymap();
	if (rc != 0 || !g.keymap_text || g.keymap_text[0] == '\0' ||
	    g.keymap_source[0] == '\0' || g.layouts[0] == '\0') {
		fprintf(stderr, "keymap yedek başarısız: %s (%s)\n",
		        g.error[0] ? g.error : "kaynak yok",
		        g.keymap_source[0] ? g.keymap_source : "");
		return 1;
	}
	printf("DISPLAY=%s\n%s\nlayouts=%s\n",
	       display && display[0] ? display : "(yok)", g.keymap_source, g.layouts);
	return 0;
}
#endif
