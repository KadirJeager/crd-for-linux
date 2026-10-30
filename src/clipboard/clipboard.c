#define _POSIX_C_SOURCE 200809L

#include "clipboard.h"
#include "clipboard_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "gen/ext-data-control-v1-client-protocol.h"
#include "gen/wlr-data-control-unstable-v1-client-protocol.h"

enum clip_proto {
	CLIP_PROTO_NONE = 0,
	CLIP_PROTO_EXT,
	CLIP_PROTO_WLR,
};

enum clip_backend {
	CLIP_BACKEND_EXT,
	CLIP_BACKEND_WLR,
};

struct clip_offer {
	enum clip_backend kind;
	union {
		struct ext_data_control_offer_v1 *ext;
		struct zwlr_data_control_offer_v1 *wlr;
	} obj;
	char **mimes;
	size_t mime_count;
};

struct clip_live_source {
	enum clip_backend kind;
	union {
		struct ext_data_control_source_v1 *ext;
		struct zwlr_data_control_source_v1 *wlr;
	} source;
	char *mime;
	unsigned char *payload;
	size_t payload_len;
};

struct clipboard_state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;

	enum clip_proto proto;
	struct ext_data_control_manager_v1 *ext_manager;
	struct zwlr_data_control_manager_v1 *wlr_manager;
	struct ext_data_control_device_v1 *ext_device;
	struct zwlr_data_control_device_v1 *wlr_device;

	struct clip_offer *selection;
	struct clip_offer *pending_offer;

	struct clip_live_source *live_source;

	uint64_t selection_serial;
	bool initialized;
};

static struct clipboard_state g_clip;

static void note_selection(void)
{
	g_clip.selection_serial++;
}

static bool g_test_inject_get_fail;
static unsigned g_test_set_call_count;

static void clip_offer_destroy(struct clip_offer *offer)
{
	size_t i;

	if (!offer)
		return;
	for (i = 0; i < offer->mime_count; i++)
		free(offer->mimes[i]);
	free(offer->mimes);
	if (offer->kind == CLIP_BACKEND_EXT && offer->obj.ext)
		ext_data_control_offer_v1_destroy(offer->obj.ext);
	if (offer->kind == CLIP_BACKEND_WLR && offer->obj.wlr)
		zwlr_data_control_offer_v1_destroy(offer->obj.wlr);
	free(offer);
}

static struct clip_offer *clip_offer_create_ext(struct ext_data_control_offer_v1 *offer)
{
	struct clip_offer *o;

	o = calloc(1, sizeof(*o));
	if (!o)
		return NULL;
	o->kind = CLIP_BACKEND_EXT;
	o->obj.ext = offer;
	return o;
}

static struct clip_offer *clip_offer_create_wlr(struct zwlr_data_control_offer_v1 *offer)
{
	struct clip_offer *o;

	o = calloc(1, sizeof(*o));
	if (!o)
		return NULL;
	o->kind = CLIP_BACKEND_WLR;
	o->obj.wlr = offer;
	return o;
}

static int clip_offer_add_mime(struct clip_offer *offer, const char *mime)
{
	char **next;
	char *copy;

	copy = strdup(mime);
	if (!copy)
		return -1;
	next = realloc(offer->mimes, (offer->mime_count + 1) * sizeof(char *));
	if (!next) {
		free(copy);
		return -1;
	}
	offer->mimes = next;
	offer->mimes[offer->mime_count++] = copy;
	return 0;
}

static bool clip_offer_has_mime(const struct clip_offer *offer, const char *mime)
{
	size_t i;

	if (!offer || !mime)
		return false;
	for (i = 0; i < offer->mime_count; i++) {
		if (strcmp(offer->mimes[i], mime) == 0)
			return true;
	}
	return false;
}

static int clip_dispatch_timeout(int timeout_ms)
{
	struct pollfd pfd;
	int ret;

	while (wl_display_prepare_read(g_clip.display) != 0) {
		if (wl_display_dispatch_pending(g_clip.display) < 0)
			return -1;
	}

	pfd.fd = wl_display_get_fd(g_clip.display);
	pfd.events = POLLIN;
	pfd.revents = 0;

	if (wl_display_flush(g_clip.display) < 0) {
		wl_display_cancel_read(g_clip.display);
		return -1;
	}

	ret = poll(&pfd, 1, timeout_ms);
	if (ret <= 0) {
		wl_display_cancel_read(g_clip.display);
		return ret == 0 ? 1 : -1;
	}

	if (wl_display_read_events(g_clip.display) < 0)
		return -1;
	if (wl_display_dispatch_pending(g_clip.display) < 0)
		return -1;
	return 0;
}

static void ext_offer_handle_offer(void *data, struct ext_data_control_offer_v1 *offer, const char *mime)
{
	struct clip_offer *o = data;

	(void)offer;
	clip_offer_add_mime(o, mime);
}

static const struct ext_data_control_offer_v1_listener ext_offer_listener = {
	.offer = ext_offer_handle_offer,
};

static void wlr_offer_handle_offer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime)
{
	struct clip_offer *o = data;

	(void)offer;
	clip_offer_add_mime(o, mime);
}

static const struct zwlr_data_control_offer_v1_listener wlr_offer_listener = {
	.offer = wlr_offer_handle_offer,
};

static void ext_device_handle_data_offer(void *data, struct ext_data_control_device_v1 *device,
					  struct ext_data_control_offer_v1 *id)
{
	struct clipboard_state *st = data;

	(void)device;
	if (st->pending_offer)
		clip_offer_destroy(st->pending_offer);
	st->pending_offer = clip_offer_create_ext(id);
	if (st->pending_offer)
		ext_data_control_offer_v1_add_listener(id, &ext_offer_listener, st->pending_offer);
}

static void ext_device_handle_selection(void *data, struct ext_data_control_device_v1 *device,
					 struct ext_data_control_offer_v1 *id)
{
	struct clipboard_state *st = data;

	(void)device;
	if (st->selection)
		clip_offer_destroy(st->selection);
	if (id && st->pending_offer && st->pending_offer->obj.ext == id) {
		st->selection = st->pending_offer;
		st->pending_offer = NULL;
	} else if (id) {
		st->selection = clip_offer_create_ext(id);
		if (st->selection)
			ext_data_control_offer_v1_add_listener(id, &ext_offer_listener, st->selection);
	} else {
		st->selection = NULL;
	}
	if (st->pending_offer) {
		clip_offer_destroy(st->pending_offer);
		st->pending_offer = NULL;
	}
	note_selection();
}

static void ext_device_handle_finished(void *data, struct ext_data_control_device_v1 *device)
{
	struct clipboard_state *st = data;

	(void)device;
	st->ext_device = NULL;
}

static void ext_device_handle_primary_selection(void *data, struct ext_data_control_device_v1 *device,
						struct ext_data_control_offer_v1 *id)
{
	struct clipboard_state *st = data;

	(void)device;
	if (st->pending_offer) {
		clip_offer_destroy(st->pending_offer);
		st->pending_offer = NULL;
	} else if (id) {
		ext_data_control_offer_v1_destroy(id);
	}
}

static const struct ext_data_control_device_v1_listener ext_device_listener = {
	.data_offer = ext_device_handle_data_offer,
	.selection = ext_device_handle_selection,
	.finished = ext_device_handle_finished,
	.primary_selection = ext_device_handle_primary_selection,
};

static void wlr_device_handle_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
					 struct zwlr_data_control_offer_v1 *id)
{
	struct clipboard_state *st = data;

	(void)device;
	if (st->pending_offer)
		clip_offer_destroy(st->pending_offer);
	st->pending_offer = clip_offer_create_wlr(id);
	if (st->pending_offer)
		zwlr_data_control_offer_v1_add_listener(id, &wlr_offer_listener, st->pending_offer);
}

static void wlr_device_handle_selection(void *data, struct zwlr_data_control_device_v1 *device,
					  struct zwlr_data_control_offer_v1 *id)
{
	struct clipboard_state *st = data;

	(void)device;
	if (st->selection)
		clip_offer_destroy(st->selection);
	if (id && st->pending_offer && st->pending_offer->obj.wlr == id) {
		st->selection = st->pending_offer;
		st->pending_offer = NULL;
	} else if (id) {
		st->selection = clip_offer_create_wlr(id);
		if (st->selection)
			zwlr_data_control_offer_v1_add_listener(id, &wlr_offer_listener, st->selection);
	} else {
		st->selection = NULL;
	}
	if (st->pending_offer) {
		clip_offer_destroy(st->pending_offer);
		st->pending_offer = NULL;
	}
	note_selection();
}

static void wlr_device_handle_finished(void *data, struct zwlr_data_control_device_v1 *device)
{
	struct clipboard_state *st = data;

	(void)device;
	st->wlr_device = NULL;
}

static void wlr_device_handle_primary_selection(void *data, struct zwlr_data_control_device_v1 *device,
						struct zwlr_data_control_offer_v1 *id)
{
	struct clipboard_state *st = data;

	(void)device;
	if (st->pending_offer) {
		clip_offer_destroy(st->pending_offer);
		st->pending_offer = NULL;
	} else if (id) {
		zwlr_data_control_offer_v1_destroy(id);
	}
}

static const struct zwlr_data_control_device_v1_listener wlr_device_listener = {
	.data_offer = wlr_device_handle_data_offer,
	.selection = wlr_device_handle_selection,
	.finished = wlr_device_handle_finished,
	.primary_selection = wlr_device_handle_primary_selection,
};

static void clip_live_source_destroy(struct clip_live_source *src)
{
	if (!src)
		return;
	if (src->kind == CLIP_BACKEND_EXT && src->source.ext)
		ext_data_control_source_v1_destroy(src->source.ext);
	if (src->kind == CLIP_BACKEND_WLR && src->source.wlr)
		zwlr_data_control_source_v1_destroy(src->source.wlr);
	free(src->mime);
	free(src->payload);
	free(src);
}

static void clip_live_source_drop(void)
{
	clip_live_source_destroy(g_clip.live_source);
	g_clip.live_source = NULL;
}

static void ext_source_handle_send(void *data, struct ext_data_control_source_v1 *source, const char *mime_type,
				    int32_t fd)
{
	struct clip_live_source *src = data;
	size_t off = 0;

	(void)source;
	if (!src || !src->mime || strcmp(src->mime, mime_type) != 0)
		goto close_fd;

	while (off < src->payload_len) {
		ssize_t w = write(fd, src->payload + off, src->payload_len - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}

close_fd:
	close(fd);
}

static void ext_source_handle_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
	struct clip_live_source *src = data;

	(void)source;
	if (src == g_clip.live_source)
		clip_live_source_drop();
}

static const struct ext_data_control_source_v1_listener ext_source_listener = {
	.send = ext_source_handle_send,
	.cancelled = ext_source_handle_cancelled,
};

static void wlr_source_handle_send(void *data, struct zwlr_data_control_source_v1 *source, const char *mime_type,
				    int32_t fd)
{
	struct clip_live_source *src = data;
	size_t off = 0;

	(void)source;
	if (!src || !src->mime || strcmp(src->mime, mime_type) != 0)
		goto close_fd;

	while (off < src->payload_len) {
		ssize_t w = write(fd, src->payload + off, src->payload_len - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}

close_fd:
	close(fd);
}

static void wlr_source_handle_cancelled(void *data, struct zwlr_data_control_source_v1 *source)
{
	struct clip_live_source *src = data;

	(void)source;
	if (src == g_clip.live_source)
		clip_live_source_drop();
}

static const struct zwlr_data_control_source_v1_listener wlr_source_listener = {
	.send = wlr_source_handle_send,
	.cancelled = wlr_source_handle_cancelled,
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
				   uint32_t version)
{
	struct clipboard_state *st = data;

	(void)version;
	if (strcmp(interface, wl_seat_interface.name) == 0 && !st->seat)
		st->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 && !st->ext_manager)
		st->ext_manager = wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
	else if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0 && !st->wlr_manager)
		st->wlr_manager = wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 2);
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static int clip_device_create(void)
{
	if (g_clip.proto == CLIP_PROTO_EXT) {
		g_clip.ext_device = ext_data_control_manager_v1_get_data_device(g_clip.ext_manager, g_clip.seat);
		if (!g_clip.ext_device)
			return -1;
		ext_data_control_device_v1_add_listener(g_clip.ext_device, &ext_device_listener, &g_clip);
		return 0;
	}
	if (g_clip.proto == CLIP_PROTO_WLR) {
		g_clip.wlr_device = zwlr_data_control_manager_v1_get_data_device(g_clip.wlr_manager, g_clip.seat);
		if (!g_clip.wlr_device)
			return -1;
		zwlr_data_control_device_v1_add_listener(g_clip.wlr_device, &wlr_device_listener, &g_clip);
		return 0;
	}
	return -1;
}

static char clip_bound[108];

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

static void clipboard_drop(void)
{
	if (g_clip.display)
		wl_display_disconnect(g_clip.display);
	memset(&g_clip, 0, sizeof g_clip);
	clip_bound[0] = '\0';
}

int clipboard_init(void)
{
	if (g_clip.initialized)
		return 0;

	memset(&g_clip, 0, sizeof(g_clip));
	g_clip.display = wl_display_connect(NULL);
	if (!g_clip.display)
		return -1;

	g_clip.registry = wl_display_get_registry(g_clip.display);
	if (!g_clip.registry) {
		wl_display_disconnect(g_clip.display);
		memset(&g_clip, 0, sizeof(g_clip));
		return -1;
	}
	wl_registry_add_listener(g_clip.registry, &registry_listener, &g_clip);

	if (wl_display_roundtrip(g_clip.display) < 0)
		goto fail;
	if (!g_clip.seat)
		goto fail;

	if (g_clip.ext_manager)
		g_clip.proto = CLIP_PROTO_EXT;
	else if (g_clip.wlr_manager)
		g_clip.proto = CLIP_PROTO_WLR;
	else
		goto fail;

	if (clip_device_create() < 0)
		goto fail;
	if (wl_display_roundtrip(g_clip.display) < 0)
		goto fail;

	g_clip.initialized = true;
	if (clip_bound[0] == '\0') {
		const char *name = getenv("WAYLAND_DISPLAY");

		if (name)
			snprintf(clip_bound, sizeof clip_bound, "%s", name);
	}
	return 0;

fail:
	if (g_clip.ext_device)
		ext_data_control_device_v1_destroy(g_clip.ext_device);
	if (g_clip.wlr_device)
		zwlr_data_control_device_v1_destroy(g_clip.wlr_device);
	if (g_clip.ext_manager)
		ext_data_control_manager_v1_destroy(g_clip.ext_manager);
	if (g_clip.wlr_manager)
		zwlr_data_control_manager_v1_destroy(g_clip.wlr_manager);
	if (g_clip.seat)
		wl_seat_destroy(g_clip.seat);
	if (g_clip.registry)
		wl_registry_destroy(g_clip.registry);
	if (g_clip.display)
		wl_display_disconnect(g_clip.display);
	memset(&g_clip, 0, sizeof(g_clip));
	return -1;
}

const char *clipboard_protocol_name(void)
{
	switch (g_clip.proto) {
	case CLIP_PROTO_EXT:
		return "ext_data_control_manager_v1";
	case CLIP_PROTO_WLR:
		return "zwlr_data_control_manager_v1";
	default:
		return "none";
	}
}

size_t clipboard_selection_mime_count(void)
{
	if (!g_clip.selection)
		return 0;
	return g_clip.selection->mime_count;
}

const char *clipboard_selection_mime_at(size_t index)
{
	if (!g_clip.selection || index >= g_clip.selection->mime_count)
		return NULL;
	return g_clip.selection->mimes[index];
}

static int clipboard_clear_selection(void)
{
	if (!g_clip.initialized)
		return -1;
	if (g_clip.proto == CLIP_PROTO_EXT) {
		ext_data_control_device_v1_set_selection(g_clip.ext_device, NULL);
		return 0;
	}
	if (g_clip.proto == CLIP_PROTO_WLR) {
		zwlr_data_control_device_v1_set_selection(g_clip.wlr_device, NULL);
		return 0;
	}
	return -1;
}

void clipboard_test_reset(void)
{
	g_test_inject_get_fail = false;
	g_test_set_call_count = 0;
}

void clipboard_test_inject_get_fail(int enable)
{
	g_test_inject_get_fail = enable != 0;
}

unsigned clipboard_test_set_call_count(void)
{
	return g_test_set_call_count;
}

int clipboard_set(const char *mime, const void *data, size_t n)
{
	struct clip_live_source *src;

	if (!g_clip.initialized)
		return -1;

	g_test_set_call_count++;

	clip_live_source_drop();

	if (!mime) {
		if (clipboard_clear_selection() < 0)
			return -1;
		if (wl_display_roundtrip(g_clip.display) < 0)
			return -1;
		return 0;
	}
	if (!data && n > 0)
		return -1;

	src = calloc(1, sizeof(*src));
	if (!src)
		return -1;
	src->mime = strdup(mime);
	if (!src->mime) {
		free(src);
		return -1;
	}
	if (n > 0) {
		src->payload = malloc(n);
		if (!src->payload) {
			free(src->mime);
			free(src);
			return -1;
		}
		memcpy(src->payload, data, n);
		src->payload_len = n;
	}

	if (g_clip.proto == CLIP_PROTO_EXT) {
		src->kind = CLIP_BACKEND_EXT;
		src->source.ext = ext_data_control_manager_v1_create_data_source(g_clip.ext_manager);
		if (!src->source.ext)
			goto fail_src;
		ext_data_control_source_v1_add_listener(src->source.ext, &ext_source_listener, src);
		ext_data_control_source_v1_offer(src->source.ext, mime);
		ext_data_control_device_v1_set_selection(g_clip.ext_device, src->source.ext);
	} else if (g_clip.proto == CLIP_PROTO_WLR) {
		src->kind = CLIP_BACKEND_WLR;
		src->source.wlr = zwlr_data_control_manager_v1_create_data_source(g_clip.wlr_manager);
		if (!src->source.wlr)
			goto fail_src;
		zwlr_data_control_source_v1_add_listener(src->source.wlr, &wlr_source_listener, src);
		zwlr_data_control_source_v1_offer(src->source.wlr, mime);
		zwlr_data_control_device_v1_set_selection(g_clip.wlr_device, src->source.wlr);
	} else {
		goto fail_src;
	}

	g_clip.live_source = src;
	if (wl_display_roundtrip(g_clip.display) < 0) {
		clip_live_source_drop();
		return -1;
	}
	return 0;

fail_src:
	free(src->mime);
	free(src->payload);
	free(src);
	return -1;
}

static int clip_read_offer_pipe(int read_fd, unsigned char **out, size_t *out_len)
{
	unsigned char chunk[4096];
	unsigned char *buf = NULL;
	size_t len = 0;
	size_t cap = 0;
	bool eof = false;

	if (fcntl(read_fd, F_SETFL, O_NONBLOCK) < 0)
		return -1;

	while (!eof) {
		ssize_t r = read(read_fd, chunk, sizeof(chunk));

		if (r > 0) {
			if (len + (size_t)r > cap) {
				size_t new_cap = cap ? cap * 2 : 4096;

				while (new_cap < len + (size_t)r)
					new_cap *= 2;
				unsigned char *nb = realloc(buf, new_cap);

				if (!nb) {
					free(buf);
					return -1;
				}
				buf = nb;
				cap = new_cap;
			}
			memcpy(buf + len, chunk, (size_t)r);
			len += (size_t)r;
			continue;
		}
		if (r == 0) {
			eof = true;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN)
			return -1;
		if (clip_dispatch_timeout(5000) < 0) {
			free(buf);
			return -1;
		}
	}

	*out = buf;
	*out_len = len;
	return 0;
}

int clipboard_pump(void)
{
	struct pollfd pfd;
	int ret;
	char path[108];

	if (read_ekran_path(path, sizeof path) == 0 && strcmp(path, clip_bound) != 0) {
		if (setenv("WAYLAND_DISPLAY", path, 1) == 0) {
			clipboard_drop();
			if (clipboard_init() == 0)
				snprintf(clip_bound, sizeof clip_bound, "%s", path);
		}
	}

	if (!g_clip.initialized || !g_clip.display)
		return -1;

	while (wl_display_prepare_read(g_clip.display) != 0) {
		if (wl_display_dispatch_pending(g_clip.display) < 0) {
			clipboard_drop();
			return -1;
		}
	}
	if (wl_display_flush(g_clip.display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(g_clip.display);
		clipboard_drop();
		return -1;
	}
	pfd.fd = wl_display_get_fd(g_clip.display);
	pfd.events = POLLIN;
	pfd.revents = 0;
	ret = poll(&pfd, 1, 0);
	if (ret < 0) {
		if (errno == EINTR) {
			wl_display_cancel_read(g_clip.display);
			return 0;
		}
		wl_display_cancel_read(g_clip.display);
		clipboard_drop();
		return -1;
	}
	if (ret == 0) {
		wl_display_cancel_read(g_clip.display);
		return 0;
	}
	if (wl_display_read_events(g_clip.display) < 0) {
		clipboard_drop();
		return -1;
	}
	if (wl_display_dispatch_pending(g_clip.display) < 0) {
		clipboard_drop();
		return -1;
	}
	return 0;
}

uint64_t clipboard_selection_serial(void)
{
	return g_clip.selection_serial;
}

int clipboard_has(const char *mime)
{
	if (!g_clip.initialized || !mime)
		return -1;
	if (!g_clip.selection)
		return 0;
	return clip_offer_has_mime(g_clip.selection, mime) ? 1 : 0;
}

int clipboard_get(const char *mime, void **data, size_t *n)
{
	int pipefd[2];
	unsigned char *buf = NULL;
	size_t len = 0;

	if (!g_clip.initialized || !mime || !data || !n)
		return -1;
	*data = NULL;
	*n = 0;

	if (g_test_inject_get_fail) {
		g_test_inject_get_fail = false;
		return -1;
	}

	if (!g_clip.selection || !clip_offer_has_mime(g_clip.selection, mime))
		return -1;

	if (pipe(pipefd) < 0)
		return -1;

	if (g_clip.selection->kind == CLIP_BACKEND_EXT)
		ext_data_control_offer_v1_receive(g_clip.selection->obj.ext, mime, pipefd[1]);
	else
		zwlr_data_control_offer_v1_receive(g_clip.selection->obj.wlr, mime, pipefd[1]);
	close(pipefd[1]);

	if (clip_read_offer_pipe(pipefd[0], &buf, &len) < 0) {
		close(pipefd[0]);
		return -1;
	}
	close(pipefd[0]);

	*data = buf;
	*n = len;
	return 0;
}
