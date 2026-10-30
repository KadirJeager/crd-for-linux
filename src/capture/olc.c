#include "capture.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CURSOR_MAX 256
#define CRD_CURSOR_FLAG_HIDDEN (1u << 0)

struct consumer {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	uint32_t node_id;
	int found_node;
	char source_format[64];
	char format_name[32];
	int format_ok;
	int width, height, stride;
	uint8_t *frame;
	size_t frame_bytes;
	uint64_t frames;
	struct timespec t0;
	int started;
	double fps;
	int cursor_seen;
	int cursor_bitmap;
	int cursor_nonzero;
	int cursor_w, cursor_h;
	int cursor_x, cursor_y, cursor_hx, cursor_hy;
	int cursor_bgr;
	uint8_t cursor_px[CURSOR_MAX * CURSOR_MAX * 4];
	int done;
	int paused;
	uint32_t self_node;
	uint32_t src_port;
	uint32_t dst_port;
	const char *error;
};

static uint64_t nsec_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *format_name(enum spa_video_format fmt)
{
	switch (fmt) {
	case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
	case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
	case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
	case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
	default: return NULL;
	}
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct consumer *c = data;
	if (id == PW_ID_CORE)
		pw_thread_loop_signal(c->loop, false);
	(void)seq;
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
};

static void on_global(void *data, uint32_t id, uint32_t permissions,
		      const char *type, uint32_t version, const struct spa_dict *props)
{
	struct consumer *c = data;
	const char *src;
	(void)permissions;
	(void)version;
	if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0 && props) {
		const char *nid = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char *dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
		char *end = NULL;
		unsigned long node;

		if (nid && dir) {
			node = strtoul(nid, &end, 10);
			if (end != nid && node == c->node_id && strcmp(dir, "out") == 0 && c->src_port == 0)
				c->src_port = id;
			if (end != nid && node == c->self_node && strcmp(dir, "in") == 0 && c->dst_port == 0)
				c->dst_port = id;
		}
	}
	if (id != c->node_id || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;
	c->found_node = 1;
	if (!props)
		return;
	src = spa_dict_lookup(props, "crd.capture.source-format");
	if (src)
		snprintf(c->source_format, sizeof c->source_format, "%s", src);
}

static void on_node_info(void *data, const struct pw_node_info *info)
{
	struct consumer *c = data;
	const char *src;

	if (!info || !info->props)
		return;
	src = spa_dict_lookup(info->props, "crd.capture.source-format");
	if (src)
		snprintf(c->source_format, sizeof c->source_format, "%s", src);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
};

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
};

static int connect_sees_node(int fd, struct consumer *c)
{
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_node *node = NULL;
	struct spa_hook core_hook;
	struct spa_hook reg_hook;
	struct spa_hook node_hook;
	int rc = -1;

	pw_thread_loop_lock(c->loop);
	ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
	core = ctx ? pw_context_connect_fd(ctx, fd, NULL, 0) : NULL;
	if (!core) {
		if (ctx)
			pw_context_destroy(ctx);
		pw_thread_loop_unlock(c->loop);
		return -1;
	}
	memset(&core_hook, 0, sizeof core_hook);
	memset(&reg_hook, 0, sizeof reg_hook);
	pw_core_add_listener(core, &core_hook, &core_events, c);
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(registry, &reg_hook, &registry_events, c);
	pw_core_sync(core, PW_ID_CORE, 0);
	pw_thread_loop_timed_wait(c->loop, 2);
	if (c->found_node) {
		memset(&node_hook, 0, sizeof node_hook);
		node = pw_registry_bind(registry, c->node_id, PW_TYPE_INTERFACE_Node,
					PW_VERSION_NODE, 0);
		if (node)
			pw_node_add_listener(node, &node_hook, &node_events, c);
		pw_core_sync(core, PW_ID_CORE, 0);
		pw_thread_loop_timed_wait(c->loop, 2);
	}
	rc = c->found_node ? 0 : -1;
	if (node)
		pw_proxy_destroy((struct pw_proxy *)node);
	pw_proxy_destroy((struct pw_proxy *)registry);
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_thread_loop_unlock(c->loop);
	return rc;
}

static void on_param(void *data, uint32_t id, const struct spa_pod *param)
{
	struct consumer *c = data;
	struct spa_video_info_raw info;
	const char *name;

	if (id != SPA_PARAM_Format || !param)
		return;
	memset(&info, 0, sizeof info);
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;
	name = format_name(info.format);
	if (!name)
		return;
	snprintf(c->format_name, sizeof c->format_name, "%s", name);
	c->width = (int)info.size.width;
	c->height = (int)info.size.height;
	c->format_ok = 1;
}

static void on_process(void *data)
{
	struct consumer *c = data;
	struct pw_buffer *b;
	struct spa_data *d;
	struct spa_meta_cursor *cursor;
	uint64_t now;
	size_t nbytes;

	b = pw_stream_dequeue_buffer(c->stream);
	if (!b)
		return;
	d = &b->buffer->datas[0];
	if (c->format_ok && d->data && d->chunk && d->chunk->size > 0) {
		nbytes = (size_t)c->stride * (size_t)c->height;
		if (c->frame && d->chunk->size >= nbytes && d->maxsize >= nbytes)
			memcpy(c->frame, d->data, nbytes);
		if (!c->started) {
			clock_gettime(CLOCK_MONOTONIC, &c->t0);
			c->started = 1;
			c->frames = 0;
		}
		c->frames++;
		now = nsec_now();
		if (c->started) {
			uint64_t t0 = (uint64_t)c->t0.tv_sec * 1000000000ull + (uint64_t)c->t0.tv_nsec;
			if (now - t0 >= 1000000000ull) {
				c->fps = (double)(c->frames - 1) * 1e9 / (double)(now - t0);
				c->done = 1;
				pw_thread_loop_signal(c->loop, false);
			}
		}
	}
	cursor = spa_buffer_find_meta_data(b->buffer, SPA_META_Cursor, sizeof *cursor);
	if (cursor && spa_meta_cursor_is_valid(cursor)) {
		c->cursor_seen = 1;
		c->cursor_x = (int)cursor->position.x;
		c->cursor_y = (int)cursor->position.y;
		c->cursor_hx = (int)cursor->hotspot.x;
		c->cursor_hy = (int)cursor->hotspot.y;
		if (cursor->bitmap_offset >= sizeof(struct spa_meta_cursor)) {
			struct spa_meta_bitmap *bm = SPA_PTROFF(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);
			if (bm->format != 0 && bm->size.width > 0 && bm->size.height > 0 &&
			    bm->offset >= sizeof(struct spa_meta_bitmap) &&
			    bm->size.width <= CURSOR_MAX && bm->size.height <= CURSOR_MAX &&
			    bm->stride >= (int)bm->size.width * 4) {
				const uint8_t *px = SPA_PTROFF(bm, bm->offset, uint8_t);
				size_t row = (size_t)bm->size.width * 4;
				size_t y, k;
				int nonzero = 0;
				c->cursor_bitmap = 1;
				c->cursor_w = (int)bm->size.width;
				c->cursor_h = (int)bm->size.height;
				for (y = 0; y < bm->size.height && !nonzero; y++) {
					const uint8_t *src = px + y * (size_t)bm->stride;
					for (k = 0; k < row; k++) {
						if (src[k]) {
							nonzero = 1;
							break;
						}
					}
				}
				if (nonzero) {
					c->cursor_nonzero = 1;
					c->cursor_bgr = (bm->format == SPA_VIDEO_FORMAT_BGRA ||
							 bm->format == SPA_VIDEO_FORMAT_BGRx);
					for (y = 0; y < bm->size.height; y++) {
						memcpy(c->cursor_px + y * row,
						       px + y * (size_t)bm->stride, row);
					}
				}
			}
		}
		(void)CRD_CURSOR_FLAG_HIDDEN;
	}
	pw_stream_queue_buffer(c->stream, b);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	struct consumer *c = data;
	(void)old;
	if (state == PW_STREAM_STATE_PAUSED && !c->paused) {
		c->paused = 1;
		pw_thread_loop_signal(c->loop, false);
	}
	if (state == PW_STREAM_STATE_ERROR) {
		c->error = error ? error : "pipewire hata";
		c->done = 1;
		pw_thread_loop_signal(c->loop, false);
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.param_changed = on_param,
	.process = on_process,
};

static int consume(int fd, struct consumer *c)
{
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_properties *props;
	struct pw_properties *link_props;
	struct pw_proxy *link = NULL;
	struct spa_hook hook;
	struct spa_hook core_hook;
	struct spa_hook reg_hook;
	uint8_t podmem[8192];
	struct spa_pod_builder b;
	const struct spa_pod *params[8];
	struct spa_rectangle size;
	struct spa_fraction rate;
	char out_node[16], out_port[16], in_node[16], in_port[16];
	int n = 0;
	int meta_size;
	int rc;

	pw_thread_loop_lock(c->loop);
	ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
	core = ctx ? pw_context_connect_fd(ctx, fd, NULL, 0) : NULL;
	if (!core) {
		c->error = "tüketici pipewire fd bağlanamadı";
		if (ctx)
			pw_context_destroy(ctx);
		pw_thread_loop_unlock(c->loop);
		return -1;
	}
	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Screen",
		PW_KEY_NODE_AUTOCONNECT, "false",
		NULL);
	c->stream = pw_stream_new(core, "crd-olc", props);
	memset(&hook, 0, sizeof hook);
	pw_stream_add_listener(c->stream, &hook, &stream_events, c);
	size.width = (uint32_t)c->width;
	size.height = (uint32_t)c->height;
	rate.num = 30;
	rate.denom = 1;
	spa_pod_builder_init(&b, podmem, sizeof podmem);
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate));
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_RGBx),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate));
	meta_size = (int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) +
			  (size_t)CURSOR_MAX * CURSOR_MAX * 4);
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_Int(meta_size));
	{
		int32_t stride = c->width * 4;
		int32_t bytes = stride * c->height;
		params[n++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(bytes, bytes, bytes),
			SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(stride, stride, stride),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	}
	if (pw_stream_connect(c->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			      PW_STREAM_FLAG_MAP_BUFFERS,
			      params, (uint32_t)n) < 0) {
		c->error = "akış bağlanamadı";
		pw_thread_loop_unlock(c->loop);
		return -1;
	}
	while (!c->paused && !c->error) {
		if (pw_thread_loop_timed_wait(c->loop, 2) != 0)
			break;
	}
	c->self_node = pw_stream_get_node_id(c->stream);
	c->src_port = 0;
	c->dst_port = 0;
	memset(&core_hook, 0, sizeof core_hook);
	memset(&reg_hook, 0, sizeof reg_hook);
	pw_core_add_listener(core, &core_hook, &core_events, c);
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(registry, &reg_hook, &registry_events, c);
	pw_core_sync(core, PW_ID_CORE, 0);
	pw_thread_loop_timed_wait(c->loop, 2);
	if (!c->error && c->src_port && c->dst_port && c->self_node) {
		snprintf(out_node, sizeof out_node, "%u", c->node_id);
		snprintf(out_port, sizeof out_port, "%u", c->src_port);
		snprintf(in_node, sizeof in_node, "%u", c->self_node);
		snprintf(in_port, sizeof in_port, "%u", c->dst_port);
		link_props = pw_properties_new(
			PW_KEY_LINK_OUTPUT_NODE, out_node,
			PW_KEY_LINK_OUTPUT_PORT, out_port,
			PW_KEY_LINK_INPUT_NODE, in_node,
			PW_KEY_LINK_INPUT_PORT, in_port,
			NULL);
		link = pw_core_create_object(core, "link-factory",
					     PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
					     link_props ? &link_props->dict : NULL, 0);
		if (link_props)
			pw_properties_free(link_props);
		if (!link)
			c->error = "bağ kurulamadı";
	} else if (!c->error) {
		c->error = "kapı bulunamadı";
	}
	while (!c->done && !c->error) {
		if (pw_thread_loop_timed_wait(c->loop, 4) != 0)
			break;
	}
	rc = (c->done && c->frames > 1 && !c->error) ? 0 : -1;
	if (link)
		pw_proxy_destroy(link);
	pw_proxy_destroy((struct pw_proxy *)registry);
	pw_stream_destroy(c->stream);
	c->stream = NULL;
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_thread_loop_unlock(c->loop);
	return rc;
}

static int write_ppm(const char *path, const uint8_t *packed, int width, int height,
		     int stride, int bgr)
{
	FILE *f;
	int y, x;

	f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", width, height);
	for (y = 0; y < height; y++) {
		const uint8_t *row = packed + (size_t)y * (size_t)stride;
		for (x = 0; x < width; x++) {
			const uint8_t *p = row + (size_t)x * 4;
			uint8_t rgb[3];
			if (bgr) {
				rgb[0] = p[2];
				rgb[1] = p[1];
				rgb[2] = p[0];
			} else {
				rgb[0] = p[0];
				rgb[1] = p[1];
				rgb[2] = p[2];
			}
			if (fwrite(rgb, 1, 3, f) != 3) {
				fclose(f);
				return -1;
			}
		}
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

static int file_sha256(const char *path, char out[65])
{
	int pipefd[2];
	pid_t pid;
	char buf[128];
	ssize_t n;
	int status;

	if (pipe(pipefd) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execlp("sha256sum", "sha256sum", path, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	n = read(pipefd[0], buf, sizeof buf - 1);
	close(pipefd[0]);
	waitpid(pid, &status, 0);
	if (n < 64 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	memcpy(out, buf, 64);
	out[64] = 0;
	return 0;
}

static int cursor_ppm_path(const char *olcum, char *out, size_t n)
{
	const char *slash = strrchr(olcum, '/');
	int rc;

	if (!slash)
		rc = snprintf(out, n, "cursor.ppm");
	else
		rc = snprintf(out, n, "%.*s/cursor.ppm", (int)(slash - olcum), olcum);
	return (rc < 0 || (size_t)rc >= n) ? -1 : 0;
}

/* Bütün pikseller aynı RGB ise tek renktir. */
static int cursor_single_color(const uint8_t *px, int width, int height, int bgr)
{
	int y, x;
	int have = 0;
	uint8_t r0 = 0, g0 = 0, b0 = 0;

	for (y = 0; y < height; y++) {
		const uint8_t *row = px + (size_t)y * (size_t)width * 4;
		for (x = 0; x < width; x++) {
			const uint8_t *p = row + (size_t)x * 4;
			uint8_t r = bgr ? p[2] : p[0];
			uint8_t g = p[1];
			uint8_t b = bgr ? p[0] : p[2];
			if (!have) {
				r0 = r;
				g0 = g;
				b0 = b;
				have = 1;
			} else if (r != r0 || g != g0 || b != b0) {
				return 0;
			}
		}
	}
	return 1;
}

static uint64_t count_nonzero(const uint8_t *rgbx, int width, int height, int stride)
{
	uint64_t n = 0;
	int y, x;

	for (y = 0; y < height; y++) {
		const uint8_t *row = rgbx + (size_t)y * (size_t)stride;
		for (x = 0; x < width; x++) {
			const uint8_t *p = row + (size_t)x * 4;
			if (p[0] | p[1] | p[2])
				n++;
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	struct crd_stream_info info;
	struct consumer c;
	int fd_a, fd_b;
	char sha[65];
	uint64_t nonzero;
	FILE *out;
	const char *ppm;
	const char *olcum;

	if (argc != 3) {
		fprintf(stderr, "kullanım: olc frame.ppm olcum.txt\n");
		return 2;
	}
	ppm = argv[1];
	olcum = argv[2];
	if (capture_init() != 0) {
		fprintf(stderr, "capture_init başarısız\n");
		return 1;
	}
	if (capture_stream_info(&info) != 0) {
		fprintf(stderr, "capture_stream_info başarısız\n");
		return 1;
	}
	fd_a = capture_open_pipewire_remote();
	fd_b = capture_open_pipewire_remote();
	if (fd_a < 0 || fd_b < 0 || fd_a == fd_b) {
		fprintf(stderr, "pipewire fd geçersiz\n");
		return 1;
	}

	memset(&c, 0, sizeof c);
	c.node_id = info.node_id;
	c.width = info.width;
	c.height = info.height;
	c.stride = info.width * 4;
	c.frame_bytes = (size_t)c.stride * (size_t)c.height;
	c.frame = calloc(1, c.frame_bytes);
	if (!c.frame)
		return 1;
	pw_init(NULL, NULL);
	c.loop = pw_thread_loop_new("crd-olc", NULL);
	if (!c.loop)
		return 1;
	pw_thread_loop_start(c.loop);
	if (connect_sees_node(fd_b, &c) != 0) {
		fprintf(stderr, "ikinci fd düğümü görmüyor: %u\n", info.node_id);
		return 1;
	}
	if (consume(fd_a, &c) != 0) {
		fprintf(stderr, "kare alınamadı: %s fps=%.2f kare=%llu\n",
			c.error ? c.error : "zaman aşımı", c.fps,
			(unsigned long long)c.frames);
		return 1;
	}
	nonzero = count_nonzero(c.frame, c.width, c.height, c.stride);
	if (nonzero == 0) {
		fprintf(stderr, "kare tamamen sıfır\n");
		return 1;
	}
	if (write_ppm(ppm, c.frame, c.width, c.height, c.stride,
		      strcmp(c.format_name, "BGRx") == 0 || strcmp(c.format_name, "BGRA") == 0) != 0) {
		fprintf(stderr, "ppm yazılamadı\n");
		return 1;
	}
	if (!c.cursor_nonzero || c.cursor_w <= 0 || c.cursor_h <= 0) {
		fprintf(stderr, "imleç bitmap'i boş\n");
		return 1;
	}
	if (cursor_single_color(c.cursor_px, c.cursor_w, c.cursor_h, c.cursor_bgr)) {
		fprintf(stderr, "imleç tek renk\n");
		return 1;
	}
	{
		char cursor_path[512];
		if (cursor_ppm_path(olcum, cursor_path, sizeof cursor_path) != 0 ||
		    write_ppm(cursor_path, c.cursor_px, c.cursor_w, c.cursor_h,
			      c.cursor_w * 4, c.cursor_bgr) != 0) {
			fprintf(stderr, "imleç ppm yazılamadı\n");
			return 1;
		}
	}
	if (file_sha256(ppm, sha) != 0) {
		fprintf(stderr, "sha256 hesaplanamadı\n");
		return 1;
	}
	out = fopen(olcum, "w");
	if (!out)
		return 1;
	fprintf(out, "node_id=%u\n", info.node_id);
	fprintf(out, "x=%d\n", info.x);
	fprintf(out, "y=%d\n", info.y);
	fprintf(out, "width=%d\n", info.width);
	fprintf(out, "height=%d\n", info.height);
	fprintf(out, "mapping_id=%s\n", info.mapping_id);
	fprintf(out, "format=%s\n", c.format_name[0] ? c.format_name : "RGBx");
	fprintf(out, "source_format=%s\n", c.source_format[0] ? c.source_format : "yok");
	fprintf(out, "fps=%.2f\n", c.fps);
	fprintf(out, "sha256=%s\n", sha);
	fprintf(out, "nonzero_pixels=%llu\n", (unsigned long long)nonzero);
	fprintf(out, "cursor_meta=%d\n", c.cursor_seen);
	fprintf(out, "cursor_position=%d,%d\n", c.cursor_x, c.cursor_y);
	fprintf(out, "cursor_hotspot=%d,%d\n", c.cursor_hx, c.cursor_hy);
	fprintf(out, "cursor_bitmap=%dx%d nonzero=%d\n", c.cursor_w, c.cursor_h, c.cursor_nonzero);
	if (fclose(out) != 0)
		return 1;
	printf("node_id=%u %dx%d %s kaynak=%s fps=%.2f sıfır_olmayan=%llu imleç=%d\n",
	       info.node_id, info.width, info.height,
	       c.format_name[0] ? c.format_name : "?",
	       c.source_format[0] ? c.source_format : "?",
	       c.fps, (unsigned long long)nonzero, c.cursor_seen);
	_exit(0);
}
