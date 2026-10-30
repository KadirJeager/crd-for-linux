/* Portalın verdiği PipeWire ucundan tek kare alır.
 * capture_init çağırmaz; ikinci bir yakalama oturumu açmaz.
 */

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CURSOR_MAX 256

struct consumer {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	uint32_t node_id;
	int found_node;
	int format_ok;
	char format_name[32];
	int width;
	int height;
	int expect_w;
	int expect_h;
	uint8_t *frame;
	size_t frame_bytes;
	int frames;
	int done;
	int paused;
	uint8_t *cursor_px;
	int cursor_w;
	int cursor_h;
	int cursor_hx;
	int cursor_hy;
	int cursor_bgr;
	int cursor_nonzero;
	int cursor_ready;
	uint32_t self_node;
	uint32_t src_port;
	uint32_t dst_port;
	const char *error;
};

static const char *
format_name(enum spa_video_format fmt)
{
	switch (fmt) {
	case SPA_VIDEO_FORMAT_RGBx:
		return "RGBx";
	case SPA_VIDEO_FORMAT_BGRx:
		return "BGRx";
	case SPA_VIDEO_FORMAT_RGBA:
		return "RGBA";
	case SPA_VIDEO_FORMAT_BGRA:
		return "BGRA";
	default:
		return NULL;
	}
}

static int
peer_is_pipewire(int fd)
{
	struct sockaddr_un addr;
	struct ucred cred;
	socklen_t len = sizeof addr;
	socklen_t clen = sizeof cred;
	const char *base;

	memset(&addr, 0, sizeof addr);
	if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0)
		return -1;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0)
		return -1;
	if (addr.sun_family != AF_UNIX || addr.sun_path[0] == '\0')
		return -1;
	if (cred.pid == getpid())
		return -1;
	addr.sun_path[sizeof addr.sun_path - 1] = '\0';
	base = strrchr(addr.sun_path, '/');
	base = base != NULL ? base + 1 : addr.sun_path;
	if (strcmp(base, "pipewire-0") != 0)
		return -1;
	return 0;
}

static void
on_core_done(void *data, uint32_t id, int seq)
{
	struct consumer *c = data;

	(void) seq;
	if (id == PW_ID_CORE)
		pw_thread_loop_signal(c->loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
};

static void
on_global(void *data, uint32_t id, uint32_t permissions, const char *type,
	  uint32_t version, const struct spa_dict *props)
{
	struct consumer *c = data;

	(void) permissions;
	(void) version;
	if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0 && props != NULL) {
		const char *nid = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char *dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
		char *end = NULL;
		unsigned long node;

		if (nid != NULL && dir != NULL) {
			node = strtoul(nid, &end, 10);
			if (end != nid && node == c->node_id && strcmp(dir, "out") == 0 && c->src_port == 0)
				c->src_port = id;
			if (end != nid && node == c->self_node && strcmp(dir, "in") == 0 && c->dst_port == 0)
				c->dst_port = id;
		}
	}
	if (id == c->node_id && strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
		c->found_node = 1;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
};

static void
on_param(void *data, uint32_t id, const struct spa_pod *param)
{
	struct consumer *c = data;
	struct spa_video_info_raw info;
	const char *name;

	if (id != SPA_PARAM_Format || param == NULL)
		return;
	memset(&info, 0, sizeof info);
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;
	name = format_name(info.format);
	if (name == NULL)
		return;
	snprintf(c->format_name, sizeof c->format_name, "%s", name);
	c->width = (int) info.size.width;
	c->height = (int) info.size.height;
	c->format_ok = 1;
}

static void
take_cursor(struct consumer *c, struct spa_buffer *buf)
{
	struct spa_meta *meta;
	struct spa_meta_cursor *cursor;
	struct spa_meta_bitmap *bm;
	const uint8_t *px;
	size_t row;
	size_t pixels_at;
	size_t pixels_bytes;
	int y;
	int x;
	int nonzero = 0;
	int near_white = 0;
	int bgr;

	meta = spa_buffer_find_meta(buf, SPA_META_Cursor);
	if (meta == NULL || meta->data == NULL || meta->size < sizeof *cursor)
		return;
	cursor = meta->data;
	if (!spa_meta_cursor_is_valid(cursor))
		return;
	if (cursor->bitmap_offset < sizeof *cursor)
		return;
	if (meta->size < sizeof *bm || cursor->bitmap_offset > meta->size - sizeof *bm)
		return;
	bm = SPA_PTROFF(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);
	if (bm->format == 0 || bm->size.width == 0 || bm->size.height == 0)
		return;
	if (bm->size.width > CURSOR_MAX || bm->size.height > CURSOR_MAX)
		return;
	if (bm->offset < sizeof *bm || bm->stride < (int) bm->size.width * 4)
		return;
	pixels_at = cursor->bitmap_offset + (size_t) bm->offset;
	if (pixels_at < cursor->bitmap_offset || pixels_at > meta->size)
		return;
	pixels_bytes = (size_t) bm->stride * (size_t) bm->size.height;
	if (bm->stride <= 0 || pixels_bytes / (size_t) bm->stride != bm->size.height)
		return;
	if (pixels_bytes > meta->size - pixels_at)
		return;
	if (bm->format == SPA_VIDEO_FORMAT_BGRA || bm->format == SPA_VIDEO_FORMAT_BGRx)
		bgr = 1;
	else if (bm->format == SPA_VIDEO_FORMAT_RGBA || bm->format == SPA_VIDEO_FORMAT_RGBx)
		bgr = 0;
	else
		return;
	px = SPA_PTROFF(bm, bm->offset, uint8_t);
	row = (size_t) bm->size.width * 4u;
	for (y = 0; y < (int) bm->size.height; y++) {
		const uint8_t *src = px + (size_t) y * (size_t) bm->stride;

		for (x = 0; x < (int) bm->size.width; x++) {
			const uint8_t *p = src + (size_t) x * 4u;
			uint8_t r = bgr ? p[2] : p[0];
			uint8_t g = p[1];
			uint8_t b = bgr ? p[0] : p[2];

			if ((r | g | b) != 0)
				nonzero++;
			if (r >= 200 && g >= 200 && b >= 200)
				near_white = 1;
		}
	}
	if (nonzero < 1 || !near_white || c->cursor_px == NULL)
		return;
	for (y = 0; y < (int) bm->size.height; y++)
		memcpy(c->cursor_px + (size_t) y * row, px + (size_t) y * (size_t) bm->stride, row);
	c->cursor_w = (int) bm->size.width;
	c->cursor_h = (int) bm->size.height;
	c->cursor_hx = cursor->hotspot.x;
	c->cursor_hy = cursor->hotspot.y;
	c->cursor_bgr = bgr;
	c->cursor_nonzero = nonzero;
	c->cursor_ready = 1;
}

static void
on_process(void *data)
{
	struct consumer *c = data;
	struct pw_buffer *b;
	struct spa_data *d;
	size_t nbytes;

	b = pw_stream_dequeue_buffer(c->stream);
	if (b == NULL)
		return;
	d = &b->buffer->datas[0];
	nbytes = c->frame_bytes;
	if (c->format_ok && (c->width != c->expect_w || c->height != c->expect_h)) {
		c->error = "boyut uyuşmuyor";
		c->done = 1;
		pw_thread_loop_signal(c->loop, false);
	} else if (c->format_ok && c->frame != NULL && d->chunk != NULL &&
		   d->chunk->stride == c->expect_w * 4 && d->chunk->size >= nbytes &&
		   d->maxsize >= nbytes && d->chunk->offset <= d->maxsize - nbytes) {
		void *mapped = NULL;
		uint8_t *src = d->data;

		if (src == NULL && d->type == SPA_DATA_MemFd && d->fd >= 0) {
			mapped = mmap(NULL, d->maxsize, PROT_READ, MAP_SHARED, d->fd, d->mapoffset);
			if (mapped != MAP_FAILED)
				src = mapped;
			else
				mapped = NULL;
		}
		if (src != NULL) {
			memcpy(c->frame, src + d->chunk->offset, nbytes);
			c->frames++;
		}
		if (mapped != NULL)
			munmap(mapped, d->maxsize);
	}
	if (!c->cursor_ready)
		take_cursor(c, b->buffer);
	if (c->frames >= 1 && c->cursor_ready && c->error == NULL) {
		c->done = 1;
		pw_thread_loop_signal(c->loop, false);
	}
	pw_stream_queue_buffer(c->stream, b);
}

static void
on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	struct consumer *c = data;

	(void) old;
	if (state == PW_STREAM_STATE_PAUSED && !c->paused) {
		c->paused = 1;
		pw_thread_loop_signal(c->loop, false);
	}
	if (state == PW_STREAM_STATE_ERROR) {
		c->error = error != NULL ? error : "pipewire hata";
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

static int
consume(int fd, struct consumer *c)
{
	struct pw_context *ctx;
	struct pw_core *core = NULL;
	struct pw_registry *registry = NULL;
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
	int waits = 0;
	int rc = -1;

	pw_thread_loop_lock(c->loop);
	ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
	if (ctx != NULL)
		core = pw_context_connect_fd(ctx, fd, NULL, 0);
	if (core == NULL) {
		c->error = "tüketici pipewire fd bağlanamadı";
		if (ctx != NULL)
			pw_context_destroy(ctx);
		pw_thread_loop_unlock(c->loop);
		return -1;
	}
	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Screen",
				  PW_KEY_NODE_AUTOCONNECT, "false",
				  NULL);
	c->stream = pw_stream_new(core, "crd-portal-frame", props);
	if (c->stream == NULL) {
		c->error = "akış kurulamadı";
		goto out;
	}
	memset(&hook, 0, sizeof hook);
	pw_stream_add_listener(c->stream, &hook, &stream_events, c);
	size.width = (uint32_t) c->expect_w;
	size.height = (uint32_t) c->expect_h;
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
	{
		int meta_size = (int) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) +
				       (size_t) CURSOR_MAX * (size_t) CURSOR_MAX * 4u);

		params[n++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
			SPA_PARAM_META_size, SPA_POD_Int(meta_size));
	}
	{
		int32_t stride = c->expect_w * 4;
		int32_t bytes = stride * c->expect_h;
		params[n++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(bytes, bytes, bytes),
			SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(stride, stride, stride),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	}
	if (pw_stream_connect(c->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			      PW_STREAM_FLAG_MAP_BUFFERS, params, (uint32_t) n) < 0) {
		c->error = "akış bağlanamadı";
		goto out;
	}
	while (!c->paused && c->error == NULL) {
		if (pw_thread_loop_timed_wait(c->loop, 2) != 0)
			break;
	}
	c->self_node = pw_stream_get_node_id(c->stream);
	if (c->self_node == 0 || c->self_node == SPA_ID_INVALID) {
		c->error = "tüketici düğümü yok";
		goto out;
	}
	memset(&core_hook, 0, sizeof core_hook);
	memset(&reg_hook, 0, sizeof reg_hook);
	pw_core_add_listener(core, &core_hook, &core_events, c);
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(registry, &reg_hook, &registry_events, c);
	for (waits = 0; waits < 4 && (c->src_port == 0 || c->dst_port == 0 || !c->found_node); waits++) {
		pw_core_sync(core, PW_ID_CORE, 0);
		if (pw_thread_loop_timed_wait(c->loop, 2) != 0 && (c->src_port == 0 || c->dst_port == 0))
			continue;
	}
	if (!c->found_node) {
		c->error = "düğüm kayıtta yok";
		goto out;
	}
	if (c->src_port == 0 || c->dst_port == 0) {
		c->error = "kapı bulunamadı";
		goto out;
	}
	snprintf(out_node, sizeof out_node, "%u", c->node_id);
	snprintf(out_port, sizeof out_port, "%u", c->src_port);
	snprintf(in_node, sizeof in_node, "%u", c->self_node);
	snprintf(in_port, sizeof in_port, "%u", c->dst_port);
	link_props = pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, out_node,
				       PW_KEY_LINK_OUTPUT_PORT, out_port,
				       PW_KEY_LINK_INPUT_NODE, in_node,
				       PW_KEY_LINK_INPUT_PORT, in_port,
				       NULL);
	link = pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
				     PW_VERSION_LINK, link_props != NULL ? &link_props->dict : NULL, 0);
	if (link_props != NULL)
		pw_properties_free(link_props);
	if (link == NULL) {
		c->error = "bağ kurulamadı";
		goto out;
	}
	waits = 0;
	while (!c->done && c->error == NULL) {
		if (pw_thread_loop_timed_wait(c->loop, 2) != 0) {
			waits++;
			if (waits >= 6)
				break;
		}
	}
	if (c->error == NULL && c->frames < 1)
		c->error = "kare gelmedi";
	else if (c->error == NULL && !c->cursor_ready)
		c->error = "imleç meta yok";
	rc = (c->frames >= 1 && c->cursor_ready && c->error == NULL && c->format_ok) ? 0 : -1;

out:
	if (link != NULL)
		pw_proxy_destroy(link);
	if (registry != NULL)
		pw_proxy_destroy((struct pw_proxy *) registry);
	if (c->stream != NULL) {
		pw_stream_destroy(c->stream);
		c->stream = NULL;
	}
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_thread_loop_unlock(c->loop);
	return rc;
}

static int
frame_is_flat(const uint8_t *rgbx, int width, int height, int stride)
{
	const uint8_t r = rgbx[0];
	const uint8_t g = rgbx[1];
	const uint8_t b = rgbx[2];
	int y;
	int x;

	for (y = 0; y < height; y++) {
		const uint8_t *row = rgbx + (size_t) y * (size_t) stride;
		for (x = 0; x < width; x++) {
			const uint8_t *p = row + (size_t) x * 4;
			if (p[0] != r || p[1] != g || p[2] != b)
				return 0;
		}
	}
	return 1;
}

static int
write_ppm(const char *path, const uint8_t *packed, int width, int height, int stride, int bgr)
{
	FILE *f;
	int y;
	int x;

	f = fopen(path, "wb");
	if (f == NULL)
		return -1;
	if (fprintf(f, "P6\n%d %d\n255\n", width, height) < 0) {
		fclose(f);
		return -1;
	}
	for (y = 0; y < height; y++) {
		const uint8_t *row = packed + (size_t) y * (size_t) stride;
		for (x = 0; x < width; x++) {
			const uint8_t *p = row + (size_t) x * 4;
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

static int
file_sha256(const char *path, char out[65])
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
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[0]);
		close(pipefd[1]);
		execlp("sha256sum", "sha256sum", path, (char *) NULL);
		_exit(127);
	}
	close(pipefd[1]);
	n = read(pipefd[0], buf, sizeof buf - 1);
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (n < 64 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	memcpy(out, buf, 64);
	out[64] = '\0';
	return 0;
}

static int
cursor_ppm_path(const char *olcum, char *out, size_t n)
{
	const char *slash = strrchr(olcum, '/');
	int rc;

	if (slash == NULL)
		rc = snprintf(out, n, "cursor.ppm");
	else
		rc = snprintf(out, n, "%.*s/cursor.ppm", (int) (slash - olcum), olcum);
	return (rc < 0 || (size_t) rc >= n) ? -1 : 0;
}

static int
parse_u32(const char *text, uint32_t *out)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || errno != 0 || value > 0xfffffffful)
		return -1;
	*out = (uint32_t) value;
	return 0;
}

static int
parse_positive(const char *text, int *out)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (end == text || *end != '\0' || errno != 0 || value <= 0 || value > 16384)
		return -1;
	*out = (int) value;
	return 0;
}

int
main(int argc, char **argv)
{
	struct consumer c;
	struct stat st;
	uint32_t node_id = 0;
	int width = 0;
	int height = 0;
	int fd;
	char sha[65];
	char cursor_path[512];
	FILE *out;
	const char *ppm;
	const char *olcum;
	int bgr;

	if (argc != 7) {
		fprintf(stderr, "kullanım: frame-al <fd> <node_id> <width> <height> <frame.ppm> <olcum.txt>\n");
		return 2;
	}
	if (parse_positive(argv[1], &fd) != 0 || fd < 0) {
		fprintf(stderr, "fd geçersiz\n");
		return 2;
	}
	if (parse_u32(argv[2], &node_id) != 0 || node_id == 0) {
		fprintf(stderr, "node_id geçersiz\n");
		return 2;
	}
	if (parse_positive(argv[3], &width) != 0 || parse_positive(argv[4], &height) != 0) {
		fprintf(stderr, "boyut geçersiz\n");
		return 2;
	}
	ppm = argv[5];
	olcum = argv[6];
	if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
		fprintf(stderr, "fd soket değil\n");
		return 1;
	}
	if (peer_is_pipewire(fd) != 0) {
		fprintf(stderr, "fd oturum pipewire soketi değil\n");
		return 1;
	}

	cursor_path[0] = '\0';
	memset(&c, 0, sizeof c);
	c.node_id = node_id;
	c.expect_w = width;
	c.expect_h = height;
	c.frame_bytes = (size_t) width * (size_t) height * 4u;
	c.frame = calloc(1, c.frame_bytes);
	c.cursor_px = calloc((size_t) CURSOR_MAX * (size_t) CURSOR_MAX * 4u, 1);
	if (c.frame == NULL || c.cursor_px == NULL ||
	    cursor_ppm_path(olcum, cursor_path, sizeof cursor_path) != 0) {
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	unlink(cursor_path);
	pw_init(NULL, NULL);
	c.loop = pw_thread_loop_new("crd-portal-frame", NULL);
	if (c.loop == NULL) {
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	pw_thread_loop_start(c.loop);
	if (consume(fd, &c) != 0) {
		fprintf(stderr, "kare alınamadı: %s kare=%d biçim=%s düğüm=%u imleç=%d\n",
			c.error != NULL ? c.error : "zaman aşımı", c.frames,
			c.format_name[0] != '\0' ? c.format_name : "yok", node_id, c.cursor_ready);
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	if (c.width != width || c.height != height) {
		fprintf(stderr, "pazarlık boyutu %dx%d beklendi %dx%d\n", c.width, c.height, width, height);
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	if (frame_is_flat(c.frame, width, height, width * 4)) {
		fprintf(stderr, "kare tek renk\n");
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	if (c.cursor_w < 1 || c.cursor_h < 1 || c.cursor_nonzero < 1) {
		fprintf(stderr, "imleç pikseli yok\n");
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	bgr = strcmp(c.format_name, "BGRx") == 0 || strcmp(c.format_name, "BGRA") == 0;
	if (write_ppm(ppm, c.frame, width, height, width * 4, bgr) != 0) {
		fprintf(stderr, "ppm yazılamadı\n");
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	if (write_ppm(cursor_path, c.cursor_px, c.cursor_w, c.cursor_h, c.cursor_w * 4, c.cursor_bgr) != 0) {
		fprintf(stderr, "imleç ppm yazılamadı\n");
		unlink(ppm);
		unlink(cursor_path);
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	if (file_sha256(ppm, sha) != 0) {
		fprintf(stderr, "sha256 hesaplanamadı\n");
		unlink(ppm);
		unlink(cursor_path);
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	out = fopen(olcum, "w");
	if (out == NULL) {
		unlink(ppm);
		unlink(cursor_path);
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	fprintf(out, "node_id=%u\n", node_id);
	fprintf(out, "width=%d\n", width);
	fprintf(out, "height=%d\n", height);
	fprintf(out, "size=%dx%d\n", width, height);
	fprintf(out, "format=%s\n", c.format_name);
	fprintf(out, "sha256=%s\n", sha);
	fprintf(out, "cursor_width=%d\n", c.cursor_w);
	fprintf(out, "cursor_height=%d\n", c.cursor_h);
	fprintf(out, "cursor_nonzero=%d\n", c.cursor_nonzero);
	fprintf(out, "cursor_hotspot=%d,%d\n", c.cursor_hx, c.cursor_hy);
	if (fclose(out) != 0) {
		unlink(ppm);
		unlink(cursor_path);
		unlink(olcum);
		pw_thread_loop_stop(c.loop);
		pw_thread_loop_destroy(c.loop);
		free(c.cursor_px);
		free(c.frame);
		return 1;
	}
	pw_thread_loop_stop(c.loop);
	pw_thread_loop_destroy(c.loop);
	free(c.cursor_px);
	free(c.frame);
	printf("node_id=%u %dx%d %s\n", node_id, width, height, c.format_name);
	return 0;
}
