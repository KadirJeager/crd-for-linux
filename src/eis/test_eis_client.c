#define _POSIX_C_SOURCE 200809L

#include "eis_server.h"
#include "eis_keymap.h"
#include "cikti.h"

#include <libei.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *proof;

static void
proof_line(const char *key, const char *value)
{
	fprintf(proof, "%s=%s\n", key, value);
	fprintf(stdout, "%s=%s\n", key, value);
	fflush(proof);
}

static void
proof_bool(const char *key, bool ok)
{
	proof_line(key, ok ? "yes" : "no");
}

struct client_state {
	bool seat_added;
	bool seat_bound;
	bool has_keyboard;
	bool has_abs_pointer;
	bool has_region;
	size_t keymap_size;
	char mapping_id[128];
	uint32_t region_x, region_y, region_w, region_h;
};

static bool
client_ready(const struct client_state *st)
{
	return st->seat_added && st->has_keyboard && st->has_abs_pointer &&
	       st->keymap_size > 0;
}

static void
handle_device_added(struct client_state *st, struct ei_device *device)
{
	struct ei_keymap *keymap;

	if (!device) {
		return;
	}
	if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		st->has_keyboard = true;
		keymap = ei_device_keyboard_get_keymap(device);
		if (keymap) {
			st->keymap_size = ei_keymap_get_size(keymap);
		}
	}
	if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
		st->has_abs_pointer = true;
		for (size_t index = 0;; index++) {
			struct ei_region *region = ei_device_get_region(device, index);
			const char *id;

			if (!region) {
				break;
			}
			id = ei_region_get_mapping_id(region);
			if (!id || id[0] == '\0' || ei_region_get_width(region) == 0 ||
			    ei_region_get_height(region) == 0) {
				continue;
			}
			snprintf(st->mapping_id, sizeof st->mapping_id, "%s", id);
			st->region_x = ei_region_get_x(region);
			st->region_y = ei_region_get_y(region);
			st->region_w = ei_region_get_width(region);
			st->region_h = ei_region_get_height(region);
			st->has_region = true;
			break;
		}
	}
}

static void
process_ei_event(struct client_state *st, struct ei_event *event)
{
	enum ei_event_type type = ei_event_get_type(event);

	switch (type) {
	case EI_EVENT_SEAT_ADDED: {
		struct ei_seat *seat = ei_event_get_seat(event);
		if (seat && !st->seat_bound) {
			ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER,
			                          EI_DEVICE_CAP_KEYBOARD,
			                          EI_DEVICE_CAP_POINTER_ABSOLUTE,
			                          EI_DEVICE_CAP_BUTTON,
			                          EI_DEVICE_CAP_SCROLL, NULL);
			st->seat_bound = true;
			st->seat_added = true;
		}
		break;
	}
	case EI_EVENT_DEVICE_ADDED:
		handle_device_added(st, ei_event_get_device(event));
		break;
	default:
		break;
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
		if (wait > 50) {
			wait = 50;
		}

		if (client_ready(st)) {
			return true;
		}

		if (poll(&pfd, 1, wait) > 0) {
			ei_dispatch(ei);
		}

		while (true) {
			struct ei_event *event = ei_get_event(ei);
			if (!event) {
				break;
			}
			process_ei_event(st, event);
			ei_event_unref(event);
		}

		elapsed += wait;
	}

	return client_ready(st);
}

int
main(void)
{
	struct ei *ei;
	int client_fd;
	int init_ret;
	struct client_state st = {0};
	char size_buf[64];
	bool ok;

	proof = fopen("kanit/eis.txt", "w");
	if (!proof) {
		fprintf(stderr, "failed to open kanit/eis.txt: %s\n",
		        strerror(errno));
		return 1;
	}

	init_ret = eis_server_init();
	if (init_ret != 0) {
		proof_line("init", "fail");
		fprintf(proof, "init_errno=%d\n", init_ret);
		fclose(proof);
		return 1;
	}
	proof_line("init", "ok");

	client_fd = eis_client_fd();
	if (client_fd < 0) {
		proof_line("client_fd", "fail");
		fclose(proof);
		return 1;
	}

	ei = ei_new_sender(NULL);
	if (!ei) {
		proof_line("ei_new_sender", "fail");
		fclose(proof);
		return 1;
	}

	if (ei_setup_backend_fd(ei, client_fd) != 0) {
		proof_line("ei_setup_backend_fd", "fail");
		ei_unref(ei);
		fclose(proof);
		return 1;
	}

	(void)run_client_loop(ei, &st, 10000);

	proof_bool("seat_added", st.seat_added);
	proof_bool("keyboard", st.has_keyboard);
	proof_bool("absolute_pointer", st.has_abs_pointer);
	snprintf(size_buf, sizeof(size_buf), "%zu", st.keymap_size);
	proof_line("keymap_size", size_buf);
	proof_line("keymap_source", eis_xkb_keymap_source());

	{
		struct crd_cikti output;
		char region[160];

		snprintf(region, sizeof region, "%s %ux%u +%u,%u",
			 st.mapping_id[0] ? st.mapping_id : "(yok)", st.region_w, st.region_h,
			 st.region_x, st.region_y);
		proof_line("region", region);
		ok = true;
		if (crd_cikti_bul(&output) != 0) {
			proof_line("wayland_output", "fail");
			ok = false;
		} else {
			char want[160];

			snprintf(want, sizeof want, "%s %dx%d +%d,%d", output.name, output.width,
				 output.height, output.x, output.y);
			proof_line("wayland_output", want);
			if (!st.has_region || strcmp(st.mapping_id, output.name) != 0 ||
			    st.region_w != (uint32_t)output.width ||
			    st.region_h != (uint32_t)output.height ||
			    st.region_x != (uint32_t)output.x ||
			    st.region_y != (uint32_t)output.y) {
				ok = false;
			}
		}
	}

	ok = ok && st.seat_added && st.has_keyboard && st.has_abs_pointer &&
	     st.keymap_size > 0 && st.has_region;
	proof_line("overall", ok ? "pass" : "fail");

	ei_disconnect(ei);
	ei_unref(ei);
	fclose(proof);

	return ok ? 0 : 1;
}
