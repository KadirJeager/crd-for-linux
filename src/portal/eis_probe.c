/* ConnectToEIS ucuna libei sender bağlar.
 * Koltuk ve mutlak bölge görününce bölgenin ortasına bir hareket ve
 * oturum haritasından bir tuş yazar. Canlı kapıyı açmaz.
 */

#include <libei.h>

#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static FILE *proof;

static void
proof_line(const char *key, const char *value)
{
	fprintf(proof, "%s=%s\n", key, value);
	fflush(proof);
}

static void
proof_bool(const char *key, bool ok)
{
	proof_line(key, ok ? "yes" : "no");
}

static bool
note_abs_region(struct ei_device *device, const char *mapping_id, uint32_t width,
                uint32_t height, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
	size_t index;

	for (index = 0;; index++) {
		struct ei_region *region = ei_device_get_region(device, index);
		const char *id;

		if (!region)
			break;
		id = ei_region_get_mapping_id(region);
		if (!id || strcmp(id, mapping_id) != 0)
			continue;
		if (ei_region_get_x(region) != 0 || ei_region_get_y(region) != 0)
			continue;
		if (ei_region_get_width(region) != width || ei_region_get_height(region) != height)
			continue;
		*x = ei_region_get_x(region);
		*y = ei_region_get_y(region);
		*w = ei_region_get_width(region);
		*h = ei_region_get_height(region);
		return true;
	}
	return false;
}

struct client_state {
	bool seat_added;
	bool seat_bound;
	bool has_keyboard;
	bool has_abs_pointer;
	bool resumed;
	bool region_ok;
	uint32_t region_x, region_y, region_w, region_h;
	size_t keymap_size;
	struct ei_device *device;
};

static bool
client_ready(const struct client_state *st)
{
	return st->seat_added && st->has_keyboard && st->has_abs_pointer && st->keymap_size > 0 &&
	       st->resumed && st->region_ok && st->device != NULL;
}

static void
handle_device(struct client_state *st, struct ei_device *device, bool resumed)
{
	struct ei_keymap *keymap;
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t w = 0;
	uint32_t h = 0;

	if (!device)
		return;
	if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		st->has_keyboard = true;
		keymap = ei_device_keyboard_get_keymap(device);
		if (keymap)
			st->keymap_size = ei_keymap_get_size(keymap);
	}
	if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) &&
	    note_abs_region(device, "HDMI-A-1", 2560, 1440, &x, &y, &w, &h)) {
		st->has_abs_pointer = true;
		st->region_ok = true;
		st->region_x = x;
		st->region_y = y;
		st->region_w = w;
		st->region_h = h;
	}
	if (resumed)
		st->resumed = true;
	if (st->device == NULL && st->has_keyboard && st->has_abs_pointer)
		st->device = ei_device_ref(device);
}

static void
process_ei_event(struct client_state *st, struct ei_event *event)
{
	switch (ei_event_get_type(event)) {
	case EI_EVENT_SEAT_ADDED: {
		struct ei_seat *seat = ei_event_get_seat(event);
		if (seat && !st->seat_bound) {
			ei_seat_bind_capabilities(seat,
			                          EI_DEVICE_CAP_POINTER,
			                          EI_DEVICE_CAP_KEYBOARD,
			                          EI_DEVICE_CAP_POINTER_ABSOLUTE,
			                          EI_DEVICE_CAP_BUTTON,
			                          EI_DEVICE_CAP_SCROLL,
			                          NULL);
			st->seat_bound = true;
			st->seat_added = true;
		}
		break;
	}
	case EI_EVENT_DEVICE_ADDED:
		handle_device(st, ei_event_get_device(event), false);
		break;
	case EI_EVENT_DEVICE_RESUMED:
		handle_device(st, ei_event_get_device(event), true);
		break;
	default:
		break;
	}
}

static void
drain_events(struct ei *ei, struct client_state *st)
{
	for (;;) {
		struct ei_event *event = ei_get_event(ei);

		if (!event)
			break;
		process_ei_event(st, event);
		ei_event_unref(event);
	}
}

static bool
run_client_loop(struct ei *ei, struct client_state *st, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = ei_get_fd(ei),
		.events = POLLIN,
	};
	int elapsed = 0;

	while (elapsed < timeout_ms) {
		int wait = timeout_ms - elapsed;

		if (client_ready(st))
			return true;
		if (wait > 50)
			wait = 50;
		if (poll(&pfd, 1, wait) > 0)
			ei_dispatch(ei);
		drain_events(ei, st);
		elapsed += wait;
	}
	return client_ready(st);
}

static uint32_t
evdev_for_keysym(struct xkb_keymap *keymap, xkb_keysym_t keysym)
{
	xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
	xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
	xkb_keycode_t key;

	for (key = min; key <= max; key++) {
		const xkb_keysym_t *syms = NULL;
		int count = xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &syms);
		uint32_t evdev;

		if (count < 1 || syms == NULL || syms[0] != keysym)
			continue;
		if (key < 8)
			return 0;
		evdev = (uint32_t) key - 8u;
		if (evdev == 0 || evdev > KEY_MAX)
			return 0;
		return evdev;
	}
	return 0;
}

static uint32_t
find_evdev(struct ei_device *device)
{
	static const xkb_keysym_t want[] = {
		XKB_KEY_gbreve,   XKB_KEY_odiaeresis, XKB_KEY_udiaeresis,
		XKB_KEY_scedilla, XKB_KEY_idotless,   XKB_KEY_a,
	};
	struct ei_keymap *km;
	struct xkb_context *ctx;
	struct xkb_keymap *keymap = NULL;
	void *buf;
	size_t size;
	size_t length;
	int fd;
	uint32_t evdev = 0;
	size_t index;

	km = ei_device_keyboard_get_keymap(device);
	if (km == NULL)
		return 0;
	fd = ei_keymap_get_fd(km);
	size = ei_keymap_get_size(km);
	if (fd < 0 || size == 0)
		return 0;
	buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (buf == MAP_FAILED)
		return 0;
	length = size;
	if (((const char *) buf)[size - 1] == '\0')
		length = size - 1;
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx != NULL) {
		keymap = xkb_keymap_new_from_buffer(ctx, buf, length, XKB_KEYMAP_FORMAT_TEXT_V1,
		                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (keymap != NULL) {
		for (index = 0; index < sizeof want / sizeof want[0]; index++) {
			evdev = evdev_for_keysym(keymap, want[index]);
			if (evdev != 0)
				break;
		}
		xkb_keymap_unref(keymap);
	}
	if (ctx != NULL)
		xkb_context_unref(ctx);
	munmap(buf, size);
	return evdev;
}

static void
send_one(struct ei *ei, struct ei_device *device, double x, double y, uint32_t evdev)
{
	ei_device_start_emulating(device, 1);
	ei_device_pointer_motion_absolute(device, x, y);
	ei_device_frame(device, ei_now(ei));
	ei_device_keyboard_key(device, evdev, true);
	ei_device_frame(device, ei_now(ei));
	ei_device_stop_emulating(device);
}

static bool
proof_passed(const char *path)
{
	FILE *handle;
	char buf[4096];
	size_t n;

	handle = fopen(path, "r");
	if (handle == NULL)
		return false;
	n = fread(buf, 1, sizeof buf - 1, handle);
	fclose(handle);
	buf[n] = '\0';
	return strstr(buf, "\noverall=pass\n") != NULL || strncmp(buf, "overall=pass\n", 13) == 0;
}

static bool
wait_olay_proof(struct ei *ei, struct client_state *st, const char *path, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = ei_get_fd(ei),
		.events = POLLIN,
	};
	int elapsed = 0;

	while (elapsed < timeout_ms) {
		if (proof_passed(path))
			return true;
		if (poll(&pfd, 1, 50) > 0)
			ei_dispatch(ei);
		drain_events(ei, st);
		elapsed += 50;
	}
	return proof_passed(path);
}

int
main(int argc, char **argv)
{
	struct ei *ei;
	struct client_state st = {0};
	char size_buf[64];
	char *end = NULL;
	long fd_long;
	int client_fd;
	uint32_t evdev = 0;
	bool ready;
	bool sent = false;
	bool recorded = false;
	bool ok;

	if (argc != 4) {
		fprintf(stderr, "kullanım: eis-probe <fd> <eis-kanıt> <olay-kanıt>\n");
		return 2;
	}

	proof = fopen(argv[2], "w");
	if (!proof) {
		fprintf(stderr, "kanıt açılamadı: %s\n", strerror(errno));
		return 1;
	}

	proof_line("source", "ConnectToEIS");
	fd_long = strtol(argv[1], &end, 10);
	if (end == argv[1] || *end != '\0' || fd_long < 0) {
		proof_line("client_fd", "invalid");
		proof_line("overall", "fail");
		fclose(proof);
		return 1;
	}
	client_fd = (int) fd_long;

	ei = ei_new_sender(NULL);
	if (!ei) {
		proof_line("ei_new_sender", "fail");
		proof_line("overall", "fail");
		fclose(proof);
		return 1;
	}
	proof_bool("client_role_sender", ei_is_sender(ei));
	ei_configure_name(ei, "crd-portal-probe");

	if (ei_setup_backend_fd(ei, client_fd) != 0) {
		proof_line("ei_setup_backend_fd", "fail");
		proof_line("injected_events", "0");
		proof_line("seat_added", "no");
		proof_line("keyboard", "no");
		proof_line("absolute_pointer", "no");
		proof_line("overall", "fail");
		ei_unref(ei);
		fclose(proof);
		return 1;
	}

	ready = run_client_loop(ei, &st, 10000);
	if (ready) {
		double ax;
		double ay;

		evdev = find_evdev(st.device);
		if (evdev != 0) {
			ax = (double) st.region_x + (double) st.region_w / 2.0;
			ay = (double) st.region_y + (double) st.region_h / 2.0;
			send_one(ei, st.device, ax, ay, evdev);
			sent = true;
			recorded = wait_olay_proof(ei, &st, argv[3], 5000);
		}
	}

	proof_line("injected_events", sent ? "1" : "0");
	proof_bool("seat_added", st.seat_added);
	proof_bool("keyboard", st.has_keyboard);
	proof_bool("absolute_pointer", st.has_abs_pointer);
	snprintf(size_buf, sizeof size_buf, "%zu", st.keymap_size);
	proof_line("keymap_size", size_buf);
	proof_line("mapping_id", "HDMI-A-1");
	proof_line("region", "2560x1440");

	ok = ei_is_sender(ei) && client_ready(&st) && sent && recorded;
	proof_line("overall", ok ? "pass" : "fail");

	if (st.device != NULL)
		ei_device_unref(st.device);
	ei_disconnect(ei);
	ei_unref(ei);
	fclose(proof);
	return ok ? 0 : 1;
}
