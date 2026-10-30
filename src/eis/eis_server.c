#define _POSIX_C_SOURCE 200809L

#include "eis_server.h"

#include "capture.h"
#include "cikti.h"
#include "eis_keymap.h"

#include <libeis.h>

#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CRD_EIS_SEAT_NAME "crd-seat"
#define CRD_EIS_DEVICE_NAME "crd-virtual-input"

static int (*g_stream_info)(struct crd_stream_info *out);

void
eis_server_set_capture_info(int (*stream_info)(struct crd_stream_info *out))
{
	g_stream_info = stream_info;
}

/* Portal yakalamayı bağlamışsa onun ölçüsü. Yoksa aynı Wayland seçimi.
 * libeis ofseti işaretsizdir; negatif mantıksal konum kabul edilmez. */
static int
load_region(char *mapping, size_t mapping_n, uint32_t *w, uint32_t *h, uint32_t *x,
	    uint32_t *y)
{
	if (g_stream_info) {
		struct crd_stream_info info;

		if (g_stream_info(&info) == 0) {
			if (info.mapping_id[0] != '\0' && info.width > 0 && info.height > 0 &&
			    info.x >= 0 && info.y >= 0) {
				snprintf(mapping, mapping_n, "%s", info.mapping_id);
				*w = (uint32_t)info.width;
				*h = (uint32_t)info.height;
				*x = (uint32_t)info.x;
				*y = (uint32_t)info.y;
				fprintf(stderr, "crd-eis: bölge yakalamadan %s %ux%u +%u,%u\n",
					mapping, *w, *h, *x, *y);
				return 0;
			}
			fprintf(stderr,
				"crd-eis: yakalama bilgisi geçersiz (%s %dx%d +%d,%d)\n",
				info.mapping_id, info.width, info.height, info.x, info.y);
			return -1;
		}
		fprintf(stderr, "crd-eis: capture_stream_info başarısız, Wayland çıktısına bakılıyor\n");
	}

	{
		struct crd_cikti output;

		if (crd_cikti_bul(&output) != 0) {
			return -1;
		}
		if (output.name[0] == '\0' || output.width <= 0 || output.height <= 0 ||
		    output.x < 0 || output.y < 0) {
			fprintf(stderr, "crd-eis: çıktı EIS ofsetine sığmıyor (%s %dx%d +%d,%d)\n",
				output.name, output.width, output.height, output.x, output.y);
			return -1;
		}
		snprintf(mapping, mapping_n, "%s", output.name);
		*w = (uint32_t)output.width;
		*h = (uint32_t)output.height;
		*x = (uint32_t)output.x;
		*y = (uint32_t)output.y;
		fprintf(stderr, "crd-eis: bölge Wayland %s %ux%u +%u,%u\n", mapping, *w, *h, *x,
			*y);
		return 0;
	}
}

static struct eis *g_eis;
static int g_client_fd = -1;
static pthread_t g_dispatch_thread;
static pthread_mutex_t g_eis_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_dispatch_running;
static atomic_bool g_initialized;

static bool
seat_bind_wants_device(struct eis_event *event)
{
	static const enum eis_device_capability caps[] = {
	    EIS_DEVICE_CAP_KEYBOARD,
	    EIS_DEVICE_CAP_POINTER,
	    EIS_DEVICE_CAP_POINTER_ABSOLUTE,
	    EIS_DEVICE_CAP_BUTTON,
	    EIS_DEVICE_CAP_SCROLL,
	    EIS_DEVICE_CAP_TEXT,
	};

	for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		if (eis_event_seat_has_capability(event, caps[i])) {
			return true;
		}
	}
	return false;
}

static struct eis_device *
create_bound_device(struct eis_seat *seat, bool with_text)
{
	struct eis_device *device;
	struct eis_region *region;
	int km_ret;

	device = eis_seat_new_device(seat);
	if (!device) {
		return NULL;
	}

	eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
	eis_device_configure_name(device, CRD_EIS_DEVICE_NAME);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
	eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
	/* CRD 157 metin tuşunu klavye olayına çevirir ve TEXT bağlamaz.
	   Bağlamayan istemciye bu yeteneği koymak aygıtı reddettirebilir. */
	if (with_text) {
		eis_device_configure_capability(device, EIS_DEVICE_CAP_TEXT);
	}

	region = eis_device_new_region(device);
	if (!region) {
		eis_device_unref(device);
		return NULL;
	}
	{
		char mapping[128];
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t x = 0;
		uint32_t y = 0;

		if (load_region(mapping, sizeof mapping, &width, &height, &x, &y) != 0) {
			fprintf(stderr, "crd-eis: bölge yok, aygıt eklenmedi\n");
			eis_region_unref(region);
			eis_device_unref(device);
			return NULL;
		}
		eis_region_set_size(region, width, height);
		eis_region_set_offset(region, x, y);
		eis_region_set_mapping_id(region, mapping);
	}
	eis_region_add(region);
	eis_region_unref(region);

	km_ret = eis_xkb_attach_to_device(device);
	if (km_ret != 0) {
		fprintf(stderr, "crd-eis: klavye haritası eklenemedi: %d\n", km_ret);
		eis_device_unref(device);
		return NULL;
	}

	eis_device_add(device);
	return device;
}

static void
provision_seat_for_client(struct eis_client *client)
{
	struct eis_seat *seat;

	seat = eis_client_new_seat(client, CRD_EIS_SEAT_NAME);
	if (!seat) {
		return;
	}

	eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
	eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
	eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
	eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
	eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
	eis_seat_configure_capability(seat, EIS_DEVICE_CAP_TEXT);
	eis_seat_add(seat);
	eis_seat_unref(seat);
}

static void
handle_seat_bind(struct eis_event *event)
{
	struct eis_client *client;
	struct eis_seat *seat;
	struct eis_device *existing;
	struct eis_device *device;
	bool wants_device;

	client = eis_event_get_client(event);
	seat = eis_event_get_seat(event);
	if (!client || !seat) {
		return;
	}

	wants_device = seat_bind_wants_device(event);
	existing = eis_client_get_user_data(client);

	if (wants_device && !existing) {
		device = create_bound_device(
		    seat, eis_event_seat_has_capability(event, EIS_DEVICE_CAP_TEXT));
		if (device) {
			eis_client_set_user_data(client, device);
		}
		return;
	}

	if (!wants_device && existing) {
		eis_device_remove(existing);
		eis_device_unref(existing);
		eis_client_set_user_data(client, NULL);
	}
}

static void
handle_seat_device_requested(struct eis_event *event)
{
	struct eis_client *client;
	struct eis_seat *seat;

	client = eis_event_get_client(event);
	seat = eis_event_get_seat(event);
	if (!client || !seat) {
		return;
	}
	if (eis_client_get_user_data(client)) {
		return;
	}
	if (!seat_bind_wants_device(event)) {
		return;
	}

	handle_seat_bind(event);
}

static void
process_eis_event(struct eis_event *event)
{
	enum eis_event_type type;

	type = eis_event_get_type(event);
	switch (type) {
	case EIS_EVENT_CLIENT_CONNECT: {
		struct eis_client *client = eis_event_get_client(event);
		if (!client) {
			break;
		}
		if (!eis_client_is_sender(client)) {
			eis_client_disconnect(client);
			break;
		}
		eis_client_connect(client);
		provision_seat_for_client(client);
		break;
	}
	case EIS_EVENT_SEAT_BIND:
		handle_seat_bind(event);
		break;
	case EIS_EVENT_SEAT_DEVICE_REQUESTED:
		handle_seat_device_requested(event);
		break;
	case EIS_EVENT_DEVICE_READY: {
		struct eis_device *device = eis_event_get_device(event);
		if (device) {
			eis_device_resume(device);
		}
		break;
	}
	case EIS_EVENT_CLIENT_DISCONNECT: {
		struct eis_client *client = eis_event_get_client(event);
		struct eis_device *device;
		if (!client) {
			break;
		}
		device = eis_client_get_user_data(client);
		if (device) {
			eis_device_remove(device);
			eis_device_unref(device);
			eis_client_set_user_data(client, NULL);
		}
		break;
	}
	default:
		break;
	}
}

static void
drain_eis_events(void)
{
	struct eis_event *event;

	for (;;) {
		event = eis_get_event(g_eis);
		if (!event) {
			break;
		}
		process_eis_event(event);
		eis_event_unref(event);
	}
}

static void *
dispatch_main(void *arg)
{
	struct pollfd pfd;

	(void)arg;

	pfd.fd = eis_get_fd(g_eis);
	pfd.events = POLLIN;

	while (atomic_load(&g_dispatch_running)) {
		int ready = poll(&pfd, 1, 100);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (ready == 0) {
			continue;
		}
		pthread_mutex_lock(&g_eis_mu);
		eis_dispatch(g_eis);
		drain_eis_events();
		pthread_mutex_unlock(&g_eis_mu);
	}

	return NULL;
}

int
eis_server_init(void)
{
	int ret;

	if (atomic_load(&g_initialized)) {
		return -EBUSY;
	}

	g_eis = eis_new(NULL);
	if (!g_eis) {
		return -ENOMEM;
	}

	ret = eis_set_flag(g_eis, EIS_FLAG_DEVICE_READY);
	if (ret != 0) {
		eis_unref(g_eis);
		g_eis = NULL;
		return ret;
	}

	ret = eis_setup_backend_fd(g_eis);
	if (ret != 0) {
		eis_unref(g_eis);
		g_eis = NULL;
		return ret;
	}

	g_client_fd = eis_backend_fd_add_client(g_eis);
	if (g_client_fd < 0) {
		ret = g_client_fd;
		eis_unref(g_eis);
		g_eis = NULL;
		g_client_fd = -1;
		return ret;
	}

	atomic_store(&g_dispatch_running, true);
	ret = pthread_create(&g_dispatch_thread, NULL, dispatch_main, NULL);
	if (ret != 0) {
		close(g_client_fd);
		g_client_fd = -1;
		eis_unref(g_eis);
		g_eis = NULL;
		return -ret;
	}

	atomic_store(&g_initialized, true);
	return 0;
}

int
eis_client_fd(void)
{
	if (!atomic_load(&g_initialized) || g_client_fd < 0) {
		return -EINVAL;
	}
	return g_client_fd;
}

int
eis_open_client_fd(void)
{
	int fd;

	if (!atomic_load(&g_initialized) || g_eis == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&g_eis_mu);
	fd = eis_backend_fd_add_client(g_eis);
	pthread_mutex_unlock(&g_eis_mu);
	return fd;
}
