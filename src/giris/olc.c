#include "giris.h"

#include "eis_keymap.h"
#include "eis_server.h"

#include <libei.h>
#include <libeis.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static FILE *proof;
static int failed;

static void proof_line(const char *key, const char *value)
{
	fprintf(proof, "%s=%s\n", key, value);
	fflush(proof);
}

static void proof_fmt(const char *key, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	proof_line(key, buf);
}

static void fail(const char *why)
{
	failed = 1;
	fprintf(stderr, "giris-olc: %s\n", why);
	proof_line("hata", why);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000L;
}

static const struct giris_global *find_global(const char *interface)
{
	for (size_t i = 0; i < giris_global_count(); i++) {
		const struct giris_global *global = giris_global_at(i);

		if (global && strcmp(global->interface, interface) == 0) {
			return global;
		}
	}
	return NULL;
}

static const struct giris_output *find_output(const char *name)
{
	for (size_t i = 0; i < giris_output_count(); i++) {
		const struct giris_output *output = giris_output_at(i);

		if (output && strcmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

static void write_bindings(void)
{
	for (size_t i = 0; i < giris_global_count(); i++) {
		const struct giris_global *global = giris_global_at(i);

		proof_fmt("global", "%s name=%u advertised=%u bound=%u",
		          global->interface, global->name, global->advertised_version,
		          global->bound_version);
	}
	for (size_t i = 0; i < giris_output_count(); i++) {
		const struct giris_output *output = giris_output_at(i);

		proof_fmt("output", "%s x=%d y=%d %dx%d scale=%d refresh=%d",
		          output->name, output->x, output->y, output->width,
		          output->height, output->scale, output->refresh);
	}
}

static int keymap_parses_as_v2(void)
{
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	const char *text = giris_keymap_text();

	if (!text || text[0] == '\0') {
		return 0;
	}
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ctx) {
		return 0;
	}
	keymap = xkb_keymap_new_from_string(ctx, text, XKB_KEYMAP_FORMAT_TEXT_V2,
	                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap) {
		xkb_keymap_unref(keymap);
	}
	xkb_context_unref(ctx);
	return keymap != NULL;
}

struct seen {
	bool seat_added;
	bool keyboard;
	bool absolute;
	size_t keymap_size;
	int keymap_type;
	char mapping_id[128];
	int32_t region_x, region_y;
	uint32_t region_w, region_h;
};

static bool region_matches(struct ei_device *device, struct seen *seen)
{
	for (size_t index = 0;; index++) {
		struct ei_region *region = ei_device_get_region(device, index);
		const char *id;

		if (!region) {
			return false;
		}
		id = ei_region_get_mapping_id(region);
		if (!id) {
			continue;
		}
		snprintf(seen->mapping_id, sizeof(seen->mapping_id), "%s", id);
		seen->region_x = (int32_t)ei_region_get_x(region);
		seen->region_y = (int32_t)ei_region_get_y(region);
		seen->region_w = ei_region_get_width(region);
		seen->region_h = ei_region_get_height(region);
		if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
			seen->absolute = true;
		}
		return true;
	}
}

static void note_device(struct seen *seen, struct ei_device *device)
{
	struct ei_keymap *keymap;

	if (!device) {
		return;
	}
	if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		seen->keyboard = true;
		keymap = ei_device_keyboard_get_keymap(device);
		if (keymap) {
			seen->keymap_size = ei_keymap_get_size(keymap);
			seen->keymap_type = (int)ei_keymap_get_type(keymap);
		}
	}
	if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
		region_matches(device, seen);
	}
}

static bool official_ready(const struct seen *seen)
{
	return seen->seat_added && seen->keyboard && seen->absolute &&
	       seen->keymap_size > 0 && seen->keymap_type == EI_KEYMAP_TYPE_XKB;
}

static int run_official_client(struct seen *seen)
{
	struct ei *ei;
	int client_fd;
	int64_t start;
	int rc = -1;

	client_fd = eis_client_fd();
	if (client_fd < 0) {
		fail("eis_client_fd");
		return -1;
	}
	ei = ei_new_sender(NULL);
	if (!ei || ei_setup_backend_fd(ei, client_fd) != 0) {
		fail("ei_setup_backend_fd ürün sunucusu");
		if (ei) {
			ei_unref(ei);
		}
		return -1;
	}

	start = now_ms();
	while (now_ms() - start < 10000 && !official_ready(seen)) {
		struct pollfd pfd = {.fd = ei_get_fd(ei), .events = POLLIN};

		if (poll(&pfd, 1, 50) > 0) {
			ei_dispatch(ei);
		}
		for (;;) {
			struct ei_event *event = ei_get_event(ei);
			enum ei_event_type type;

			if (!event) {
				break;
			}
			type = ei_event_get_type(event);
			if (type == EI_EVENT_SEAT_ADDED && !seen->seat_added) {
				struct ei_seat *seat = ei_event_get_seat(event);

				if (seat) {
					ei_seat_bind_capabilities(
					    seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_KEYBOARD,
					    EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON,
					    EI_DEVICE_CAP_SCROLL, NULL);
					seen->seat_added = true;
				}
			} else if (type == EI_EVENT_DEVICE_ADDED) {
				note_device(seen, ei_event_get_device(event));
			}
			ei_event_unref(event);
		}
	}

	rc = official_ready(seen) ? 0 : -1;
	ei_disconnect(ei);
	ei_unref(ei);
	return rc;
}

struct feed {
	struct ei_device *device;
	struct eis_device *server_device;
	bool resumed;
	bool sent;
	int motion, button, key, wheel, stop, frames, modifiers;
	int handle_errno;
};

static int keymap_fd_from_giris(void)
{
	size_t size = giris_keymap_size();
	const char *text = giris_keymap_text();
	int fd;
	void *map;

	if (!text || size < 2) {
		return -1;
	}
	fd = memfd_create("giris-test-xkb", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return -1;
	}
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}
	memcpy(map, text, size);
	munmap(map, size);
	return fd;
}

static struct eis_device *make_test_device(struct eis_seat *seat,
                                           const struct giris_output *output)
{
	struct eis_device *device;
	struct eis_region *region;
	struct eis_keymap *keymap;
	int fd;

	device = eis_seat_new_device(seat);
	if (!device) {
		return NULL;
	}
	eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
	eis_device_configure_name(device, "giris-test-input");
	eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
	region = eis_device_new_region(device);
	if (!region) {
		eis_device_unref(device);
		return NULL;
	}
	eis_region_set_size(region, (uint32_t)output->width, (uint32_t)output->height);
	eis_region_set_offset(region, 0, 0);
	eis_region_set_mapping_id(region, output->name);
	eis_region_add(region);
	eis_region_unref(region);

	fd = keymap_fd_from_giris();
	if (fd < 0) {
		eis_device_unref(device);
		return NULL;
	}
	keymap = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd,
	                               giris_keymap_size());
	close(fd);
	if (!keymap) {
		eis_device_unref(device);
		return NULL;
	}
	eis_keymap_add(keymap);
	eis_keymap_unref(keymap);
	eis_device_add(device);
	return device;
}

static void send_sample(struct ei *ei, struct feed *feed, double x, double y,
                        uint32_t evdev)
{
	struct ei_device *device = feed->device;
	uint64_t t = ei_now(ei);

	ei_device_start_emulating(device, 1);
	ei_device_pointer_motion_absolute(device, x, y);
	ei_device_frame(device, t);
	ei_device_button_button(device, BTN_LEFT, true);
	ei_device_frame(device, ei_now(ei));
	ei_device_scroll_discrete(device, 0, -GIRIS_SCROLL_STEP_V120);
	ei_device_frame(device, ei_now(ei));
	ei_device_scroll_stop(device, false, true);
	ei_device_frame(device, ei_now(ei));
	ei_device_keyboard_key(device, evdev, true);
	ei_device_frame(device, ei_now(ei));
	ei_device_stop_emulating(device);
	feed->sent = true;
}

static int forward_eis_event(struct feed *feed, struct eis_event *event)
{
	enum eis_event_type type = eis_event_get_type(event);
	int rc;

	switch (type) {
	case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
	case EIS_EVENT_BUTTON_BUTTON:
	case EIS_EVENT_SCROLL_DISCRETE:
	case EIS_EVENT_SCROLL_STOP:
	case EIS_EVENT_KEYBOARD_KEY:
	case EIS_EVENT_FRAME:
		break;
	default:
		return 0;
	}
	if (giris_live_send_enabled()) {
		fail("canlı kapı olay iletilmeden açık");
		return -EPERM;
	}
	rc = giris_handle_ei_event(event);
	if (rc != 0) {
		feed->handle_errno = rc;
		return rc;
	}
	if (type == EIS_EVENT_POINTER_MOTION_ABSOLUTE) {
		feed->motion++;
	} else if (type == EIS_EVENT_BUTTON_BUTTON) {
		feed->button++;
	} else if (type == EIS_EVENT_SCROLL_DISCRETE) {
		feed->wheel++;
	} else if (type == EIS_EVENT_SCROLL_STOP) {
		feed->stop++;
	} else if (type == EIS_EVENT_KEYBOARD_KEY) {
		feed->key++;
	} else if (type == EIS_EVENT_FRAME) {
		feed->frames++;
	}
	return 0;
}

static void handle_test_eis_event(struct feed *feed, struct eis_event *event)
{
	enum eis_event_type type = eis_event_get_type(event);

	switch (type) {
	case EIS_EVENT_CLIENT_CONNECT: {
		struct eis_client *client = eis_event_get_client(event);
		struct eis_seat *seat;

		if (!client || !eis_client_is_sender(client)) {
			if (client) {
				eis_client_disconnect(client);
			}
			break;
		}
		eis_client_connect(client);
		seat = eis_client_new_seat(client, "giris-test-seat");
		if (!seat) {
			break;
		}
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
		eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
		eis_seat_add(seat);
		eis_seat_unref(seat);
		break;
	}
	case EIS_EVENT_SEAT_BIND:
		if (!feed->server_device) {
			struct eis_seat *seat = eis_event_get_seat(event);
			const struct giris_output *output = find_output("HDMI-A-1");

			if (seat && output) {
				feed->server_device = make_test_device(seat, output);
			}
		}
		break;
	case EIS_EVENT_DEVICE_READY:
		if (feed->server_device) {
			eis_device_resume(feed->server_device);
		}
		break;
	default:
		forward_eis_event(feed, event);
		break;
	}
}

static int run_translator(const struct giris_output *output, uint32_t evdev,
                          struct feed *feed)
{
	struct eis *eis;
	struct ei *ei = NULL;
	int client_fd;
	int64_t start;
	double x = (double)(output->width / 2);
	double y = (double)(output->height / 2);

	eis = eis_new(NULL);
	if (!eis || eis_set_flag(eis, EIS_FLAG_DEVICE_READY) != 0 ||
	    eis_setup_backend_fd(eis) != 0) {
		fail("test libeis alıcısı kurulamadı");
		if (eis) {
			eis_unref(eis);
		}
		return -1;
	}
	client_fd = eis_backend_fd_add_client(eis);
	if (client_fd < 0) {
		fail("test eis_backend_fd_add_client");
		eis_unref(eis);
		return -1;
	}
	ei = ei_new_sender(NULL);
	if (!ei || ei_setup_backend_fd(ei, client_fd) != 0) {
		fail("test ei gönderici kurulamadı");
		if (ei) {
			ei_unref(ei);
		}
		eis_unref(eis);
		return -1;
	}

	start = now_ms();
	while (now_ms() - start < 10000) {
		struct pollfd pfds[2];

		if (feed->motion && feed->button && feed->wheel && feed->stop &&
		    feed->key && feed->handle_errno == 0) {
			break;
		}
		if (feed->handle_errno != 0) {
			break;
		}

		pfds[0].fd = eis_get_fd(eis);
		pfds[0].events = POLLIN;
		pfds[1].fd = ei_get_fd(ei);
		pfds[1].events = POLLIN;
		if (poll(pfds, 2, 50) < 0 && errno != EINTR) {
			break;
		}
		if (pfds[0].revents) {
			eis_dispatch(eis);
		}
		for (;;) {
			struct eis_event *event = eis_get_event(eis);

			if (!event) {
				break;
			}
			handle_test_eis_event(feed, event);
			eis_event_unref(event);
		}
		if (pfds[1].revents) {
			ei_dispatch(ei);
		}
		for (;;) {
			struct ei_event *event = ei_get_event(ei);

			if (!event) {
				break;
			}
			if (ei_event_get_type(event) == EI_EVENT_SEAT_ADDED) {
				struct ei_seat *seat = ei_event_get_seat(event);

				if (seat) {
					ei_seat_bind_capabilities(
					    seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_KEYBOARD,
					    EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON,
					    EI_DEVICE_CAP_SCROLL, NULL);
				}
			} else if (ei_event_get_type(event) == EI_EVENT_DEVICE_RESUMED) {
				struct ei_device *device = ei_event_get_device(event);

				if (device && !feed->device) {
					feed->device = ei_device_ref(device);
				}
				feed->resumed = true;
			}
			ei_event_unref(event);
		}
		if (feed->resumed && feed->device && !feed->sent &&
		    !giris_live_send_enabled()) {
			send_sample(ei, feed, x, y, evdev);
		}
	}

	if (feed->device) {
		ei_device_unref(feed->device);
		feed->device = NULL;
	}
	if (feed->server_device) {
		eis_device_unref(feed->server_device);
		feed->server_device = NULL;
	}
	ei_disconnect(ei);
	ei_unref(ei);
	eis_unref(eis);
	return feed->handle_errno == 0 && feed->motion == 1 && feed->button == 1 &&
	               feed->key == 1 && feed->wheel == 1
	           ? 0
	           : -1;
}

static int count_kind(enum giris_rec_kind kind)
{
	int count = 0;

	for (size_t i = 0; i < giris_record_count(); i++) {
		const struct giris_rec *rec = giris_record_at(i);

		if (rec && rec->kind == kind) {
			count++;
		}
	}
	return count;
}

static const struct giris_rec *first_kind(enum giris_rec_kind kind)
{
	for (size_t i = 0; i < giris_record_count(); i++) {
		const struct giris_rec *rec = giris_record_at(i);

		if (rec && rec->kind == kind) {
			return rec;
		}
	}
	return NULL;
}

static uint32_t find_tr_evdev(char *key_name, size_t key_name_len, char *sym_name,
                              size_t sym_name_len, uint32_t *utf32)
{
	static const xkb_keysym_t want[] = {
	    XKB_KEY_gbreve,     XKB_KEY_odiaeresis, XKB_KEY_udiaeresis,
	    XKB_KEY_scedilla,   XKB_KEY_idotless,
	};
	uint32_t evdev = 0;

	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (giris_keymap_find_evdev(want[i], &evdev, key_name, key_name_len,
		                            sym_name, sym_name_len) == 0) {
			*utf32 = xkb_keysym_to_utf32(want[i]);
			return evdev;
		}
	}
	return 0;
}

int main(void)
{
	const struct giris_global *pointer_mgr;
	const struct giris_global *keyboard_mgr;
	const struct giris_output *output;
	struct seen seen;
	struct feed feed;
	char key_name[64];
	char sym_name[64];
	char eis_source[512];
	uint32_t utf32 = 0;
	uint32_t evdev;
	int init_rc;
	int overall = 0;

	if (mkdir("kanit", 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "mkdir kanit: %s\n", strerror(errno));
		return 1;
	}
	proof = fopen("kanit/giris.txt", "w");
	if (!proof) {
		fprintf(stderr, "kanit/giris.txt: %s\n", strerror(errno));
		return 1;
	}

	init_rc = giris_init();
	proof_fmt("init", "%s", init_rc == 0 ? "ok" : "fail");
	if (init_rc != 0) {
		proof_fmt("init_errno", "%d", init_rc);
		proof_line("init_hata", giris_init_error());
		write_bindings();
		fail("giris_init");
		goto done;
	}
	proof_line("wayland_display", giris_wayland_display());
	write_bindings();

	pointer_mgr = find_global("zwlr_virtual_pointer_manager_v1");
	keyboard_mgr = find_global("zwp_virtual_keyboard_manager_v1");
	output = find_output("HDMI-A-1");
	if (!pointer_mgr || pointer_mgr->bound_version != 2) {
		fail("virtual pointer manager v2 bağlanmadı");
	}
	if (!keyboard_mgr || keyboard_mgr->bound_version < 1) {
		fail("virtual keyboard manager bağlanmadı");
	}
	if (!output || output->width != 2560 || output->height != 1440 ||
	    output->x != 0 || output->y != 0) {
		fail("HDMI-A-1 2560x1440 ofset 0,0 ölçülmedi");
	}

	proof_fmt("keymap_bytes", "%zu", giris_keymap_size());
	proof_line("keymap_source", giris_keymap_source());
	proof_line("keymap_layouts", giris_keymap_layouts());
	proof_line("keymap_empty",
	           giris_keymap_size() > 1 && giris_keymap_text()[0] != '\0' ? "no"
	                                                                     : "yes");
	proof_line("keymap_hyprland_v2_parse", keymap_parses_as_v2() ? "yes" : "no");
	if (giris_keymap_size() < 2 || giris_keymap_text()[0] == '\0' ||
	    giris_keymap_source()[0] == '\0' || !keymap_parses_as_v2()) {
		fail("keymap boş ya da Hyprland V2 ayrıştırmıyor");
	}
	if (!strstr(giris_keymap_layouts(), "tr") &&
	    !strstr(giris_keymap_source(), "layout tr")) {
		fail("keymap kaynağı tr değil");
	}

	if (giris_set_live_send(0) != 0 || giris_live_send_enabled() != 0) {
		fail("canlı kapı baştan kapalı değil");
	}

	memset(&seen, 0, sizeof(seen));
	seen.keymap_type = -1;
	if (eis_server_init() != 0) {
		fail("eis_server_init");
		goto done;
	}
	if (run_official_client(&seen) != 0) {
		fail("EIS istemcisi koltuk/klavye/mutlak işaretçi görmedi");
	}
	snprintf(eis_source, sizeof(eis_source), "%s", eis_xkb_keymap_source());
	proof_line("eis_seat", seen.seat_added ? "yes" : "no");
	proof_line("eis_keyboard", seen.keyboard ? "yes" : "no");
	proof_line("eis_absolute_pointer", seen.absolute ? "yes" : "no");
	proof_fmt("eis_region", "%s %ux%u offset=%d,%d", seen.mapping_id,
	          seen.region_w, seen.region_h, seen.region_x, seen.region_y);
	proof_fmt("eis_keymap_bytes", "%zu", seen.keymap_size);
	proof_fmt("eis_keymap_type", "%s",
	          seen.keymap_type == EI_KEYMAP_TYPE_XKB ? "xkb" : "other");
	proof_line("eis_keymap_source", eis_source[0] ? eis_source : "(yok)");
	if (!seen.seat_added || !seen.keyboard || !seen.absolute ||
	    seen.keymap_size == 0 || seen.keymap_type != EI_KEYMAP_TYPE_XKB ||
	    strcmp(seen.mapping_id, "HDMI-A-1") != 0 || seen.region_w != 2560 ||
	    seen.region_h != 1440 || seen.region_x != 0 || seen.region_y != 0) {
		fail("EIS bölge veya keymap ölçümü tutmadı");
	}

	key_name[0] = sym_name[0] = '\0';
	evdev = output ? find_tr_evdev(key_name, sizeof(key_name), sym_name,
	                                sizeof(sym_name), &utf32)
	               : 0;
	proof_fmt("evdev", "%u", evdev);
	proof_line("evdev_key_name", key_name);
	proof_line("evdev_keysym", sym_name);
	proof_fmt("evdev_utf32", "U+%04X", utf32);
	if (evdev == 0 || evdev > KEY_MAX) {
		fail("oturum haritasından evdev tuş kodu çıkmadı");
		goto done;
	}

	if (giris_live_send_enabled()) {
		fail("çeviriden önce canlı kapı açık");
		goto done;
	}

	memset(&feed, 0, sizeof(feed));
	if (!output || run_translator(output, evdev, &feed) != 0) {
		proof_fmt("handle_errno", "%d", feed.handle_errno);
		proof_fmt("feed", "motion=%d button=%d wheel=%d stop=%d key=%d frames=%d",
		          feed.motion, feed.button, feed.wheel, feed.stop, feed.key,
		          feed.frames);
		fail("çevirici EIS olayını kayda yazamadı");
	}

	{
		const struct giris_rec *motion = first_kind(GIRIS_REC_MOTION_ABSOLUTE);
		const struct giris_rec *button = first_kind(GIRIS_REC_BUTTON);
		const struct giris_rec *wheel = first_kind(GIRIS_REC_AXIS_DISCRETE);
		const struct giris_rec *key = first_kind(GIRIS_REC_KEY);
		int motion_n = count_kind(GIRIS_REC_MOTION_ABSOLUTE);
		int button_n = count_kind(GIRIS_REC_BUTTON);
		int key_n = count_kind(GIRIS_REC_KEY);
		int wheel_n = count_kind(GIRIS_REC_AXIS_DISCRETE);
		int frame_n = count_kind(GIRIS_REC_POINTER_FRAME);

		proof_fmt("kayit_sayisi", "%zu", giris_record_count());
		proof_fmt("kayit_motion_absolute", "%d", motion_n);
		if (motion) {
			proof_fmt("kayit_motion", "x=%u y=%u x_extent=%u y_extent=%u",
			          motion->x, motion->y, motion->x_extent, motion->y_extent);
		}
		proof_fmt("kayit_button", "%d", button_n);
		if (button) {
			proof_fmt("kayit_button_ayrinti", "code=%u state=%u", button->button,
			          button->state);
		}
		proof_fmt("kayit_key", "%d", key_n);
		if (key) {
			proof_fmt("kayit_key_ayrinti", "evdev=%u state=%u", key->key,
			          key->state);
		}
		proof_fmt("kayit_wheel", "%d", wheel_n);
		if (wheel) {
			proof_fmt("kayit_wheel_ayrinti", "axis=%u discrete=%d value=%.3f",
			          wheel->axis, wheel->discrete, wheel->axis_value);
		}
		proof_fmt("kayit_pointer_frame", "%d", frame_n);
		proof_fmt("kayit_modifiers", "%d", count_kind(GIRIS_REC_MODIFIERS));
		proof_fmt("wl_giris_istegi", "%llu",
		          (unsigned long long)giris_wl_input_requests());
		proof_line("canli_kapi", giris_live_send_enabled() ? "acik" : "kapali");
		proof_line("kayit_yerel", "evet");
		proof_line("kayit_kompozitor_gozlemi", "hayir");
		proof_line(
		    "gonderim_yolu",
		    "wl istekleri marshal icinde; marshal yalnizca canli kapi acikken. "
		    "giris_set_live_send(1) cagrilmadi");

		if (motion_n != 1 || !motion || !output || motion->x != (uint32_t)(output->width / 2) ||
		    motion->y != (uint32_t)(output->height / 2) ||
		    motion->x_extent != (uint32_t)output->width ||
		    motion->y_extent != (uint32_t)output->height ||
		    motion->x > motion->x_extent || motion->y > motion->y_extent) {
			fail("mutlak hareket kaydı 2560x1440 içinde değil");
		}
		if (button_n != 1 || !button || button->button != BTN_LEFT ||
		    button->state != WL_POINTER_BUTTON_STATE_PRESSED) {
			fail("düğme kaydı BTN_LEFT değil");
		}
		if (key_n != 1 || !key || key->key != evdev ||
		    key->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
			fail("tuş kaydı oturum evdev kodu değil");
		}
		if (wheel_n != 1 || !wheel ||
		    wheel->axis != WL_POINTER_AXIS_VERTICAL_SCROLL ||
		    wheel->discrete != -1 || wheel->axis_value != -GIRIS_SCROLL_STEP_UNITS) {
			fail("tekerlek kaydı bir tıklama değil");
		}
		if (giris_live_send_enabled() != 0 || giris_wl_input_requests() != 0) {
			fail("canlı kapı kapalı değil ya da wl giriş isteği sayacı sıfır değil");
		}
	}

done:
	overall = failed ? 0 : 1;
	proof_line("overall", overall ? "pass" : "fail");
	giris_fini();
	fclose(proof);
	return overall ? 0 : 1;
}
