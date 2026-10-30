#include "olay.h"

#include "giris.h"

#include <libei.h>

#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

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
	fprintf(stderr, "olay-olc: %s\n", why);
	proof_line("hata", why);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000L;
}

struct istemci {
	char seat[128];
	bool seat_added;
	struct ei_device *device;
	bool resumed;
	bool region_ok;
	uint32_t region_x, region_y, region_w, region_h;
	bool keyboard;
	bool absolute;
};

static void note_region(struct istemci *st, struct ei_device *device)
{
	struct ei_region *region;

	if (!device) {
		return;
	}
	if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		st->keyboard = true;
	}
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
		return;
	}
	st->absolute = true;
	region = ei_device_get_region(device, 0);
	if (!region || ei_region_get_width(region) == 0 ||
	    ei_region_get_height(region) == 0) {
		return;
	}
	st->region_x = ei_region_get_x(region);
	st->region_y = ei_region_get_y(region);
	st->region_w = ei_region_get_width(region);
	st->region_h = ei_region_get_height(region);
	st->region_ok = true;
	if (!st->device) {
		st->device = ei_device_ref(device);
	}
}

static void consume_client(struct istemci *st, struct ei *ei)
{
	for (;;) {
		struct ei_event *event = ei_get_event(ei);
		enum ei_event_type type;

		if (!event) {
			break;
		}
		type = ei_event_get_type(event);
		if (type == EI_EVENT_SEAT_ADDED && !st->seat_added) {
			struct ei_seat *seat = ei_event_get_seat(event);
			const char *name = seat ? ei_seat_get_name(seat) : NULL;

			if (seat && name && name[0] != '\0') {
				snprintf(st->seat, sizeof(st->seat), "%s", name);
				ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER,
				                          EI_DEVICE_CAP_KEYBOARD,
				                          EI_DEVICE_CAP_POINTER_ABSOLUTE,
				                          EI_DEVICE_CAP_BUTTON,
				                          EI_DEVICE_CAP_SCROLL, NULL);
				st->seat_added = true;
			}
		} else if (type == EI_EVENT_DEVICE_ADDED ||
		           type == EI_EVENT_DEVICE_RESUMED) {
			note_region(st, ei_event_get_device(event));
			if (type == EI_EVENT_DEVICE_RESUMED) {
				st->resumed = true;
			}
		}
		ei_event_unref(event);
	}
}

static void send_one(struct ei *ei, struct ei_device *device, double x, double y,
                     uint32_t evdev)
{
	ei_device_start_emulating(device, 1);
	ei_device_pointer_motion_absolute(device, x, y);
	ei_device_frame(device, ei_now(ei));
	ei_device_keyboard_key(device, evdev, true);
	ei_device_frame(device, ei_now(ei));
	ei_device_stop_emulating(device);
}

static uint32_t find_evdev(char *key_name, size_t key_name_len, char *sym_name,
                           size_t sym_name_len)
{
	static const uint32_t want[] = {
	    XKB_KEY_gbreve,   XKB_KEY_odiaeresis, XKB_KEY_udiaeresis,
	    XKB_KEY_scedilla, XKB_KEY_idotless,
	};
	uint32_t evdev = 0;

	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (giris_keymap_find_evdev(want[i], &evdev, key_name, key_name_len,
		                            sym_name, sym_name_len) == 0 &&
		    evdev != 0 && evdev <= KEY_MAX) {
			return evdev;
		}
	}
	return 0;
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

int main(void)
{
	struct ei *ei = NULL;
	struct istemci st;
	char key_name[64];
	char sym_name[64];
	uint32_t evdev = 0;
	double ax = 0;
	double ay = 0;
	uint32_t expect_x = 0;
	uint32_t expect_y = 0;
	int client_fd;
	int init_rc;
	int geldi = 0;
	bool sent = false;
	int64_t start;
	int overall = 0;

	if (mkdir("kanit", 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "mkdir kanit: %s\n", strerror(errno));
		return 1;
	}
	proof = fopen("kanit/olay.txt", "w");
	if (!proof) {
		fprintf(stderr, "kanit/olay.txt: %s\n", strerror(errno));
		return 1;
	}

	memset(&st, 0, sizeof(st));
	init_rc = olay_init();
	proof_line("init", init_rc == 0 ? "ok" : "fail");
	if (init_rc != 0) {
		proof_fmt("init_errno", "%d", init_rc);
		proof_line("init_hata", olay_init_error());
		fail("olay_init");
		goto done;
	}
	proof_line("wayland_display", giris_wayland_display());
	if (giris_live_send_enabled() != 0) {
		fail("canlı kapı ölçüm başında açık");
		goto done;
	}

	client_fd = olay_client_fd();
	if (client_fd < 0) {
		fail("eis_client_fd");
		goto done;
	}
	ei = ei_new_sender(NULL);
	if (!ei || ei_setup_backend_fd(ei, client_fd) != 0) {
		fail("ei_setup_backend_fd");
		goto done;
	}

	key_name[0] = sym_name[0] = '\0';
	evdev = find_evdev(key_name, sizeof(key_name), sym_name, sizeof(sym_name));
	if (evdev == 0) {
		fail("oturum haritasından evdev tuş kodu çıkmadı");
		goto done;
	}

	start = now_ms();
	while (now_ms() - start < 10000) {
		struct pollfd pfd = {.fd = ei_get_fd(ei), .events = POLLIN};

		if (sent) {
			geldi = olay_geldi();
			if (geldi != 0) {
				break;
			}
		}
		if (poll(&pfd, 1, 50) > 0) {
			ei_dispatch(ei);
		}
		consume_client(&st, ei);
		if (!sent && st.resumed && st.device && st.region_ok && st.keyboard &&
		    st.absolute && !giris_live_send_enabled()) {
			ax = (double)st.region_x + (double)st.region_w / 2.0;
			ay = (double)st.region_y + (double)st.region_h / 2.0;
			expect_x = (uint32_t)((ax - (double)st.region_x) + 0.5);
			expect_y = (uint32_t)((ay - (double)st.region_y) + 0.5);
			send_one(ei, st.device, ax, ay, evdev);
			sent = true;
		}
	}

	olay_kopru_kapat();
	if (!sent) {
		geldi = olay_geldi();
	} else if (geldi == 0) {
		geldi = olay_geldi();
	}

	proof_line("seat", st.seat[0] ? st.seat : "(yok)");
	proof_line("evdev_key_name", key_name);
	proof_line("evdev_keysym", sym_name);

	{
		const struct giris_rec *motion = first_kind(GIRIS_REC_MOTION_ABSOLUTE);
		const struct giris_rec *key = first_kind(GIRIS_REC_KEY);
		int motion_n = count_kind(GIRIS_REC_MOTION_ABSOLUTE);
		int key_n = count_kind(GIRIS_REC_KEY);

		proof_fmt("kayit_motion_absolute", "%d", motion_n);
		if (motion) {
			proof_fmt("kayit_motion", "x=%u y=%u x_extent=%u y_extent=%u",
			          motion->x, motion->y, motion->x_extent, motion->y_extent);
		} else {
			proof_line("kayit_motion", "(yok)");
		}
		proof_fmt("kayit_key", "%d", key_n);
		if (key) {
			proof_fmt("kayit_key_ayrinti", "evdev=%u state=%u", key->key,
			          key->state);
		} else {
			proof_line("kayit_key_ayrinti", "(yok)");
		}
		proof_fmt("wl_giris_istegi", "%llu",
		          (unsigned long long)giris_wl_input_requests());
		proof_line("canli_kapi", giris_live_send_enabled() ? "acik" : "kapali");

		if (!st.seat_added || st.seat[0] == '\0') {
			fail("EIS koltuğu görülmedi");
		}
		if (!sent) {
			fail("aygıt devam etmedi, hareket ve tuş gönderilmedi");
		}
		if (geldi < 0) {
			proof_fmt("handle_errno", "%d", geldi);
			fail("giris_handle_ei_event hata döndü");
		} else if (geldi == 0) {
			fail("mutlak hareket ve tuş kayda düşmedi");
		}
		if (motion_n != 1 || !motion || motion->x != expect_x ||
		    motion->y != expect_y || motion->x_extent != st.region_w ||
		    motion->y_extent != st.region_h) {
			fail("kayıt edilen mutlak hareket gönderilen bölgeyle tutmuyor");
		}
		if (key_n != 1 || !key || key->key != evdev ||
		    key->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
			fail("kayıt edilen tuş gönderilen evdev basışı değil");
		}
		if (giris_live_send_enabled() != 0 || giris_wl_input_requests() != 0) {
			fail("canlı kapı kapalı değil ya da wl giriş isteği sıfır değil");
		}
	}

done:
	overall = failed ? 0 : 1;
	proof_line("overall", overall ? "pass" : "fail");
	if (st.device) {
		ei_device_unref(st.device);
	}
	if (ei) {
		ei_disconnect(ei);
		ei_unref(ei);
	}
	olay_fini();
	fclose(proof);
	return overall ? 0 : 1;
}
