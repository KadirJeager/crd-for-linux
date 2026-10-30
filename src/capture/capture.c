#include "capture.h"
#include "cikti.h"
#include "cursor_theme.h"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

/* Üst sınır 30 kare/sn, kasıtlı olarak sabit.
 * Uzak masaüstü görüntüyü saniyede yaklaşık 30 kare taşır. Çıkışın her
 * karesini tam çözünürlükte 10 bit kopyalamak bant genişliğini katlar,
 * uzak görüntüyü iyileştirmez. Aralık çalışma anında değişmez. */
#define CRD_CAPTURE_MAX_FPS 30
#define CRD_CAPTURE_INTERVAL_NS (1000000000L / CRD_CAPTURE_MAX_FPS)

/* Biçim pazarlığının alt ve üst sınırı. Alt sınır, konağın 1×1 teklifini
 * aralık dışında bırakır. Üst sınır 8K'dır; bu makinenin çıkış modu değil. */
#define CRD_CAPTURE_MIN_W 320
#define CRD_CAPTURE_MIN_H 240
#define CRD_CAPTURE_MAX_W 7680
#define CRD_CAPTURE_MAX_H 4320

#define CURSOR_MAX 256
#define MAX_FORMATS 16
#define MAX_OUTPUTS 16

/* spa 1.6 meta.h bayrağı sayı olarak bırakır. Bit 0 imleci gizler. */
#define CRD_CURSOR_FLAG_HIDDEN (1u << 0)

struct plane {
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint8_t *map;
	size_t map_size;
	int stride;
	uint32_t width, height;
	uint32_t formats[MAX_FORMATS];
	int nformats;
	uint32_t format;
	int batch_open;
	int have_constraints;
	int busy;
	int stopped;
	int is_cursor;
	uint32_t transform;
	int64_t pts;
};

struct output_info {
	struct wl_output *output;
	int32_t x, y, width, height, scale, refresh;
	int have_mode;
	char name[128];
};

struct cursor_pub {
	int inside;
	int have_bitmap;
	int32_t x, y, hx, hy;
	uint32_t spa;
	uint32_t serial;
	int w, h, stride;
	uint8_t pixels[CURSOR_MAX * CURSOR_MAX * 4];
};

static struct {
	int running;
	int fatal;
	const char *fatal_msg;
	int stop;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct ext_output_image_capture_source_manager_v1 *src_mgr;
	struct ext_image_copy_capture_manager_v1 *cap_mgr;
	/* KWin yolu: ext-image-copy-capture yoksa zkde_screencast. KWin akışı
	   kendisi yayımlar; bu süreç düğüm açmaz, kare kopyalamaz. */
	struct zkde_screencast_unstable_v1 *kwin_sc;
	struct zkde_screencast_stream_unstable_v1 *kwin_stream;
	int kwin;
	int kwin_state; /* 0 bekliyor, 1 oluştu, -1 başarısız/kapandı */
	struct ext_image_capture_source_v1 *source;
	struct ext_image_copy_capture_cursor_session_v1 *cursor_session;

	struct output_info outputs[MAX_OUTPUTS];
	int noutputs;
	int output_overflow;
	struct output_info *chosen;
	char node_name[160];
	char output_name[128];
	char output_desc[256];
	int32_t logical_x, logical_y, logical_w, logical_h;

	struct plane screen;
	struct plane cursor_plane;
	int hot_x, hot_y;

	uint8_t *front;
	uint8_t *back;
	uint8_t cursor_scratch[CURSOR_MAX * CURSOR_MAX * 4];
	struct cursor_pub cursor;
	int have_frame;
	int64_t frame_pts;
	char source_format[32];
	/* İç bellek her zaman BGRx. Akış biçimini konak pazarlıkta seçer. */
	uint32_t spa_format;
	uint32_t out_format;
	/* İstemcinin istediği akış boyutu. Panel modu kalır; kare buna ölçeklenir. */
	uint32_t out_w;
	uint32_t out_h;
	uint32_t buf_w;
	uint32_t buf_h;
	/* Akış parametresinin son kurulan boyutu. Düğüm kimliği bundan bağımsız. */
	int32_t linked_w;
	int32_t linked_h;

	struct crd_stream_info info;

	struct pw_thread_loop *pw_loop;
	struct pw_context *pw_context;
	struct pw_core *pw_core;
	struct pw_stream *stream;
	struct spa_hook stream_hook;
	/* PipeWire düğümü oluşur oluşmaz yayımlanmaz. WirePlumber oturum
	   öğesi hazır olmadan konak bağlanırsa hedef bulunamaz ve akış
	   varsayılan kameraya düşer; kamera biçimi uyuşmaz, görüntü donar. */
	uint32_t pw_node_id;
	struct pw_stream *probe;
	struct spa_hook probe_hook;
	int probe_state;
	char probe_error[160];
	struct pw_registry *reg;
	struct spa_hook reg_hook;
	struct pw_metadata *defmeta;
	struct spa_hook meta_hook;
	char object_serial[32];
	uint32_t host_node_id;

	int timerfd;
	pthread_t thread;
	int thread_started;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int sync_ready;
} g;

static void log_err(const char *msg)
{
	fprintf(stderr, "crd-capture: %s\n", msg);
}

static int wait_flag(int *flag, int timeout_ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	while (!*flag && !g.fatal) {
		int rc = pthread_cond_timedwait(&g.cv, &g.mu, &ts);
		if (rc == ETIMEDOUT)
			return -1;
	}
	return (*flag && !g.fatal) ? 0 : -1;
}

static void set_fatal(const char *msg)
{
	pthread_mutex_lock(&g.mu);
	if (!g.fatal_msg)
		g.fatal_msg = msg;
	g.fatal = 1;
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.mu);
	log_err(msg);
}

static const char *shm_format_name(uint32_t format)
{
	switch (format) {
	case WL_SHM_FORMAT_XBGR2101010: return "XBGR2101010";
	case WL_SHM_FORMAT_ABGR2101010: return "ABGR2101010";
	case WL_SHM_FORMAT_XRGB2101010: return "XRGB2101010";
	case WL_SHM_FORMAT_ARGB2101010: return "ARGB2101010";
	case WL_SHM_FORMAT_ARGB8888: return "ARGB8888";
	case WL_SHM_FORMAT_XRGB8888: return "XRGB8888";
	case WL_SHM_FORMAT_ABGR8888: return "ABGR8888";
	case WL_SHM_FORMAT_XBGR8888: return "XBGR8888";
	default: return NULL;
	}
}

/* XBGR2101010 / ABGR2101010, little-endian sözcük: bit 0-9 R, 10-19 G, 20-29 B.
 * 8 bit, 10 bitin üst bitleridir. */
static void convert_10(const uint8_t *src, int src_stride, uint8_t *dst,
		       int dst_stride, int width, int height, int bgr)
{
	int y, x;

	for (y = 0; y < height; y++) {
		const uint8_t *s = src + (size_t)y * (size_t)src_stride;
		uint8_t *d = dst + (size_t)y * (size_t)dst_stride;
		for (x = 0; x < width; x++) {
			uint32_t p = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
				     ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
			uint8_t r = (uint8_t)((p & 0x3ffu) >> 2);
			uint8_t g = (uint8_t)(((p >> 10) & 0x3ffu) >> 2);
			uint8_t b = (uint8_t)(((p >> 20) & 0x3ffu) >> 2);

			if (bgr) {
				d[0] = b;
				d[1] = g;
				d[2] = r;
			} else {
				d[0] = r;
				d[1] = g;
				d[2] = b;
			}
			d[3] = 0xff;
			s += 4;
			d += 4;
		}
	}
}

static void copy_rows(const uint8_t *src, int src_stride,
		      uint8_t *dst, int dst_stride,
		      int width, int height)
{
	int y;
	size_t row = (size_t)width * 4;

	for (y = 0; y < height; y++)
		memcpy(dst + (size_t)y * (size_t)dst_stride,
		       src + (size_t)y * (size_t)src_stride, row);
}

/* wl_shm ARGB8888 belleği B,G,R,A'dır; SPA_VIDEO_FORMAT_BGRA ile aynı dizilim. */
static int cursor_spa_format(uint32_t shm, uint32_t *spa, int *tenbit)
{
	*tenbit = 0;
	switch (shm) {
	case WL_SHM_FORMAT_ARGB8888: *spa = SPA_VIDEO_FORMAT_BGRA; return 0;
	case WL_SHM_FORMAT_XRGB8888: *spa = SPA_VIDEO_FORMAT_BGRx; return 0;
	case WL_SHM_FORMAT_ABGR8888: *spa = SPA_VIDEO_FORMAT_RGBA; return 0;
	case WL_SHM_FORMAT_XBGR8888: *spa = SPA_VIDEO_FORMAT_RGBx; return 0;
	case WL_SHM_FORMAT_XBGR2101010:
	case WL_SHM_FORMAT_ABGR2101010:
		*spa = SPA_VIDEO_FORMAT_RGBx;
		*tenbit = 1;
		return 0;
	default:
		return -1;
	}
}

static void plane_note_event(struct plane *p)
{
	if (!p->batch_open) {
		p->batch_open = 1;
		p->nformats = 0;
	}
}

static void plane_add_format(struct plane *p, uint32_t format)
{
	int i;

	plane_note_event(p);
	for (i = 0; i < p->nformats; i++) {
		if (p->formats[i] == format)
			return;
	}
	if (p->nformats < MAX_FORMATS)
		p->formats[p->nformats++] = format;
}

#define FORMAT_NONE 0xffffffffu

static uint32_t pick_screen_format(struct plane *p)
{
	int i;

	for (i = 0; i < p->nformats; i++) {
		if (p->formats[i] == WL_SHM_FORMAT_XBGR2101010)
			return WL_SHM_FORMAT_XBGR2101010;
	}
	for (i = 0; i < p->nformats; i++) {
		if (p->formats[i] == WL_SHM_FORMAT_ABGR2101010)
			return WL_SHM_FORMAT_ABGR2101010;
	}
	/* Bu oturumun kopya biçimi tarama biçimi XBGR2101010 değil; birleştirici
	 * okuma için 8 bit XRGB8888 / ARGB8888 veriyor. 10 bit gelirse o seçilir. */
	for (i = 0; i < p->nformats; i++) {
		if (p->formats[i] == WL_SHM_FORMAT_XRGB8888)
			return WL_SHM_FORMAT_XRGB8888;
	}
	for (i = 0; i < p->nformats; i++) {
		if (p->formats[i] == WL_SHM_FORMAT_ARGB8888)
			return WL_SHM_FORMAT_ARGB8888;
	}
	return FORMAT_NONE;
}

static uint32_t pick_cursor_format(struct plane *p)
{
	static const uint32_t prefer[] = {
		WL_SHM_FORMAT_ARGB8888,
		WL_SHM_FORMAT_XRGB8888,
		WL_SHM_FORMAT_ABGR8888,
		WL_SHM_FORMAT_XBGR8888,
		WL_SHM_FORMAT_XBGR2101010,
		WL_SHM_FORMAT_ABGR2101010,
	};
	size_t n = sizeof prefer / sizeof prefer[0];
	size_t i;
	int j;

	for (i = 0; i < n; i++) {
		for (j = 0; j < p->nformats; j++) {
			if (p->formats[j] == prefer[i])
				return prefer[i];
		}
	}
	return FORMAT_NONE;
}

static void plane_destroy_buffer(struct plane *p)
{
	if (p->buffer) {
		wl_buffer_destroy(p->buffer);
		p->buffer = NULL;
	}
	if (p->pool) {
		wl_shm_pool_destroy(p->pool);
		p->pool = NULL;
	}
	if (p->map) {
		munmap(p->map, p->map_size);
		p->map = NULL;
		p->map_size = 0;
	}
}

static int plane_build(struct plane *p)
{
	uint32_t format;
	size_t stride, bytes;
	int fd;

	if (!p->have_constraints)
		return 0;
	format = p->is_cursor ? pick_cursor_format(p) : pick_screen_format(p);
	if (format == FORMAT_NONE) {
		int i;
		fprintf(stderr, "crd-capture: %s için shm biçimi yok (%d adet)\n",
			p->is_cursor ? "imleç" : "ekran", p->nformats);
		for (i = 0; i < p->nformats; i++) {
			uint32_t f = p->formats[i];
			fprintf(stderr, "crd-capture: biçim 0x%08x %c%c%c%c\n", f,
				(int)(f & 0xff), (int)((f >> 8) & 0xff),
				(int)((f >> 16) & 0xff), (int)((f >> 24) & 0xff));
		}
		fprintf(stderr, "crd-capture: boyut %ux%u\n", p->width, p->height);
		return p->is_cursor ? 0 : -1;
	}
	if (p->width == 0 || p->height == 0)
		return 0;
	if (p->is_cursor && (p->width > CURSOR_MAX || p->height > CURSOR_MAX)) {
		log_err("imleç arabelleği üst sınırın üzerinde, bitmap atlanıyor");
		return 0;
	}
	if (p->buffer && p->format == format &&
	    p->map_size == (size_t)p->stride * p->height &&
	    (uint32_t)(p->stride / 4) == p->width)
		return 0;

	plane_destroy_buffer(p);
	stride = (size_t)p->width * 4;
	bytes = stride * p->height;
	fd = memfd_create(p->is_cursor ? "crd-cursor" : "crd-frame", MFD_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "crd-capture: memfd: %s\n", strerror(errno));
		return -1;
	}
	if (ftruncate(fd, (off_t)bytes) < 0) {
		fprintf(stderr, "crd-capture: ftruncate: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	p->map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p->map == MAP_FAILED) {
		fprintf(stderr, "crd-capture: mmap: %s\n", strerror(errno));
		p->map = NULL;
		close(fd);
		return -1;
	}
	p->map_size = bytes;
	p->stride = (int)stride;
	p->pool = wl_shm_create_pool(g.shm, fd, (int32_t)bytes);
	close(fd);
	if (!p->pool) {
		log_err("wl_shm havuzu kurulamadı");
		return -1;
	}
	p->buffer = wl_shm_pool_create_buffer(p->pool, 0, (int32_t)p->width,
					      (int32_t)p->height, (int32_t)stride, format);
	if (!p->buffer) {
		log_err("wl_buffer kurulamadı");
		return -1;
	}
	p->format = format;
	if (!p->is_cursor) {
		const char *name = shm_format_name(format);
		snprintf(g.source_format, sizeof g.source_format, "%s",
			 name ? name : "unknown");
		/* Panel 10 bit olsa da iç bellek BGRx kalır. PipeWire biçimini
		   konak seçer; tek Id verilirse konak listeyi tüketir ve akış ölür. */
		g.spa_format = SPA_VIDEO_FORMAT_BGRx;
		fprintf(stderr,
			"crd-capture: kaynak %s, akış seçenekleri BGRx BGRA RGBx RGBA\n",
			g.source_format);
	}
	return 0;
}

/* XRGB8888 / ARGB8888 belleği B,G,R,(A|X). Akış BGRx; dolgu baytı 0. */
static void copy_bgrx(const uint8_t *src, int src_stride, uint8_t *dst,
		      int dst_stride, int width, int height)
{
	int y, x;

	for (y = 0; y < height; y++) {
		const uint8_t *s = src + (size_t)y * (size_t)src_stride;
		uint8_t *d = dst + (size_t)y * (size_t)dst_stride;
		for (x = 0; x < width; x++) {
			d[0] = s[0];
			d[1] = s[1];
			d[2] = s[2];
			d[3] = 0xff;
			s += 4;
			d += 4;
		}
	}
}

static void publish_screen(struct plane *p)
{
	uint8_t *swap;

	if (p->width != (uint32_t)g.info.width || p->height != (uint32_t)g.info.height)
		return;
	if (p->format == WL_SHM_FORMAT_XBGR2101010 ||
	    p->format == WL_SHM_FORMAT_ABGR2101010)
		convert_10(p->map, p->stride, g.back, g.info.width * 4,
			   g.info.width, g.info.height, 1);
	else if (p->format == WL_SHM_FORMAT_XRGB8888 ||
		 p->format == WL_SHM_FORMAT_ARGB8888)
		copy_bgrx(p->map, p->stride, g.back, g.info.width * 4,
			  g.info.width, g.info.height);
	else
		return;
	pthread_mutex_lock(&g.mu);
	swap = g.front;
	g.front = g.back;
	g.back = swap;
	g.frame_pts = p->pts;
	g.have_frame = 1;
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.mu);
}

static int bytes_all_zero(const uint8_t *px, int stride, int height)
{
	int y, x;

	for (y = 0; y < height; y++) {
		const uint8_t *row = px + (size_t)y * (size_t)stride;
		for (x = 0; x < stride; x++) {
			if (row[x])
				return 0;
		}
	}
	return 1;
}

static void publish_cursor(struct plane *p)
{
	uint32_t spa = 0;
	int tenbit = 0;
	int stride;
	static int theme_logged;

	if (cursor_spa_format(p->format, &spa, &tenbit) < 0)
		return;
	if (p->width == 0 || p->height == 0 ||
	    p->width > CURSOR_MAX || p->height > CURSOR_MAX)
		return;
	stride = (int)p->width * 4;
	if (tenbit)
		convert_10(p->map, p->stride, g.cursor_scratch, stride,
			   (int)p->width, (int)p->height, 0);
	else
		copy_rows(p->map, p->stride, g.cursor_scratch, stride,
			  (int)p->width, (int)p->height);

	/* Hyprland 0.56 tema imlecinde wl_surface olmadığı için kopyayı
	 * saydam temizler. Boyut ve sıcak nokta dolu kalır. Pikseller
	 * hyprcursor temasındaki aynı şekildedir; uydurma dolgu yok. */
	if (bytes_all_zero(g.cursor_scratch, stride, (int)p->height)) {
		if (cursor_theme_copy((int)p->width, (int)p->height, g.hot_x, g.hot_y,
				      g.cursor_scratch, stride) < 0) {
			pthread_mutex_lock(&g.mu);
			g.cursor.have_bitmap = 0;
			pthread_mutex_unlock(&g.mu);
			return;
		}
		spa = SPA_VIDEO_FORMAT_BGRA;
		if (!theme_logged) {
			fprintf(stderr,
				"crd-capture: imleç kopyası saydam; hyprcursor %dx%d sıcak nokta %d,%d\n",
				(int)p->width, (int)p->height, g.hot_x, g.hot_y);
			theme_logged = 1;
		}
	}

	pthread_mutex_lock(&g.mu);
	memcpy(g.cursor.pixels, g.cursor_scratch, (size_t)stride * p->height);
	g.cursor.w = (int)p->width;
	g.cursor.h = (int)p->height;
	g.cursor.stride = stride;
	g.cursor.spa = spa;
	g.cursor.hx = g.hot_x;
	g.cursor.hy = g.hot_y;
	g.cursor.have_bitmap = 1;
	g.cursor.serial++;
	if (g.cursor.serial == 0)
		g.cursor.serial = 1;
	pthread_mutex_unlock(&g.mu);
}

static void on_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
			   uint32_t width, uint32_t height)
{
	struct plane *p = data;
	(void)session;
	plane_note_event(p);
	p->width = width;
	p->height = height;
}

static void on_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
			  uint32_t format)
{
	(void)session;
	plane_add_format(data, format);
}

static void on_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
			     struct wl_array *device)
{
	(void)session;
	(void)device;
	plane_note_event(data);
}

static void on_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
			     uint32_t format, struct wl_array *modifiers)
{
	(void)session;
	(void)format;
	(void)modifiers;
	plane_note_event(data);
}

static void on_session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
	struct plane *p = data;
	(void)session;
	p->batch_open = 0;
	p->have_constraints = 1;
	if (p->busy)
		return;
	if (plane_build(p) < 0)
		set_fatal(p->is_cursor ? "imleç arabelleği kurulamadı" :
					 "ekran arabelleği kurulamadı");
}

static void on_session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
	struct plane *p = data;
	(void)session;
	p->stopped = 1;
	if (!p->is_cursor)
		set_fatal("yakalama oturumu durdu");
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = on_buffer_size,
	.shm_format = on_shm_format,
	.dmabuf_device = on_dmabuf_device,
	.dmabuf_format = on_dmabuf_format,
	.done = on_session_done,
	.stopped = on_session_stopped,
};

static void on_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			 uint32_t transform)
{
	struct plane *p = data;
	(void)frame;
	p->transform = transform;
}

static void on_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		      int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)data;
	(void)frame;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void on_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
				 uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
	struct plane *p = data;
	(void)frame;
	p->pts = (int64_t)((((uint64_t)tv_sec_hi << 32) | tv_sec_lo) * 1000000000ull + tv_nsec);
}

static void finish_frame(struct plane *p, struct ext_image_copy_capture_frame_v1 *frame)
{
	ext_image_copy_capture_frame_v1_destroy(frame);
	if (p->frame == frame)
		p->frame = NULL;
	p->busy = 0;
	if (p->have_constraints)
		plane_build(p);
}

static void on_frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
	struct plane *p = data;

	if (p->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		finish_frame(p, frame);
		if (!p->is_cursor)
			set_fatal("ekran dönüşümü normal değil");
		return;
	}
	if (p->is_cursor)
		publish_cursor(p);
	else
		publish_screen(p);
	finish_frame(p, frame);
}

static void on_frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			    uint32_t reason)
{
	struct plane *p = data;

	finish_frame(p, frame);
	if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED ||
	    reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
		if (!p->is_cursor)
			set_fatal("kare kopyası başarısız");
	}
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = on_transform,
	.damage = on_damage,
	.presentation_time = on_presentation_time,
	.ready = on_frame_ready,
	.failed = on_frame_failed,
};

static void on_cursor_enter(void *data, struct ext_image_copy_capture_cursor_session_v1 *session)
{
	(void)data;
	(void)session;
	pthread_mutex_lock(&g.mu);
	g.cursor.inside = 1;
	pthread_mutex_unlock(&g.mu);
}

static void on_cursor_leave(void *data, struct ext_image_copy_capture_cursor_session_v1 *session)
{
	(void)data;
	(void)session;
	pthread_mutex_lock(&g.mu);
	g.cursor.inside = 0;
	pthread_mutex_unlock(&g.mu);
}

static void on_cursor_position(void *data, struct ext_image_copy_capture_cursor_session_v1 *session,
			       int32_t x, int32_t y)
{
	(void)data;
	(void)session;
	pthread_mutex_lock(&g.mu);
	g.cursor.inside = 1;
	g.cursor.x = x;
	g.cursor.y = y;
	pthread_mutex_unlock(&g.mu);
}

static void on_cursor_hotspot(void *data, struct ext_image_copy_capture_cursor_session_v1 *session,
			      int32_t x, int32_t y)
{
	(void)data;
	(void)session;
	/* Protokol: sıcak nokta sonraki ready ile geçerlidir. */
	g.hot_x = x;
	g.hot_y = y;
}

static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
	.enter = on_cursor_enter,
	.leave = on_cursor_leave,
	.position = on_cursor_position,
	.hotspot = on_cursor_hotspot,
};

static void on_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
			int32_t phys_w, int32_t phys_h, int32_t subpixel,
			const char *make, const char *model, int32_t transform)
{
	struct output_info *o = data;
	(void)output;
	(void)phys_w;
	(void)phys_h;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	o->x = x;
	o->y = y;
}

static void on_mode(void *data, struct wl_output *output, uint32_t flags,
		    int32_t width, int32_t height, int32_t refresh)
{
	struct output_info *o = data;
	(void)output;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		o->width = width;
		o->height = height;
		o->refresh = refresh;
		o->have_mode = 1;
	}
}

static void on_output_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void on_scale(void *data, struct wl_output *output, int32_t scale)
{
	struct output_info *o = data;
	(void)output;
	o->scale = scale;
}

static void on_name(void *data, struct wl_output *output, const char *name)
{
	struct output_info *o = data;
	(void)output;
	snprintf(o->name, sizeof o->name, "%s", name ? name : "");
}

static void on_description(void *data, struct wl_output *output, const char *description)
{
	(void)data;
	(void)output;
	(void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = on_geometry,
	.mode = on_mode,
	.done = on_output_done,
	.scale = on_scale,
	.name = on_name,
	.description = on_description,
};

static void on_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
			     struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
	(void)x;
	(void)y;
	/* Yüzey yok. set_cursor yok: imleç, klavye ve odak değişmez. */
}

static void on_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
			     struct wl_surface *surface)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
}

static void on_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
			      wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)x;
	(void)y;
}

static void on_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
			      uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)time;
	(void)button;
	(void)state;
}

static void on_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
			    uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}

static void on_pointer_frame(void *data, struct wl_pointer *pointer)
{
	(void)data;
	(void)pointer;
}

static void on_pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
	(void)data;
	(void)pointer;
	(void)source;
}

static void on_pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
				 uint32_t axis)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

static void on_pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
				     int32_t discrete)
{
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = on_pointer_enter,
	.leave = on_pointer_leave,
	.motion = on_pointer_motion,
	.button = on_pointer_button,
	.axis = on_pointer_axis,
	.frame = on_pointer_frame,
	.axis_source = on_pointer_axis_source,
	.axis_stop = on_pointer_axis_stop,
	.axis_discrete = on_pointer_axis_discrete,
};

static void on_seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
		g.pointer = wl_seat_get_pointer(seat);
		if (g.pointer)
			wl_pointer_add_listener(g.pointer, &pointer_listener, NULL);
	}
}

static void on_seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = on_seat_caps,
	.name = on_seat_name,
};

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
		      const char *interface, uint32_t version)
{
	(void)data;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		g.shm = wl_registry_bind(registry, name, &wl_shm_interface, version < 1 ? version : 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output_info *o;
		uint32_t v = version < 4 ? version : 4;

		if (g.noutputs >= MAX_OUTPUTS) {
			g.output_overflow = 1;
			return;
		}
		o = &g.outputs[g.noutputs++];
		memset(o, 0, sizeof *o);
		o->scale = 1;
		o->output = wl_registry_bind(registry, name, &wl_output_interface, v);
		wl_output_add_listener(o->output, &output_listener, o);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !g.seat) {
		uint32_t v = version < 5 ? version : 5;
		g.seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
		wl_seat_add_listener(g.seat, &seat_listener, NULL);
	} else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		g.src_mgr = wl_registry_bind(registry, name,
					     &ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0 && !g.kwin_sc) {
		/* v5: created olayı düğüm kimliğini verir (v6'da serial'a taşındı). */
		g.kwin_sc = wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface,
					     version < 5 ? version : 5);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		g.cap_mgr = wl_registry_bind(registry, name,
					     &ext_image_copy_capture_manager_v1_interface, 1);
	}
}

static void on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = on_global,
	.global_remove = on_global_remove,
};

static int plane_capture(struct plane *p)
{
	if (g.stop || p->busy || p->stopped || !p->buffer)
		return -1;
	p->frame = ext_image_copy_capture_session_v1_create_frame(p->session);
	if (!p->frame)
		return -1;
	ext_image_copy_capture_frame_v1_add_listener(p->frame, &frame_listener, p);
	ext_image_copy_capture_frame_v1_attach_buffer(p->frame, p->buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(p->frame, 0, 0,
						      (int32_t)p->width, (int32_t)p->height);
	ext_image_copy_capture_frame_v1_capture(p->frame);
	p->busy = 1;
	return 0;
}

static void on_tick(void)
{
	int have = 0;
	enum pw_stream_state st;

	if (g.stop)
		return;
	plane_capture(&g.screen);
	plane_capture(&g.cursor_plane);
	if (!g.stream)
		return;
	pthread_mutex_lock(&g.mu);
	have = g.have_frame;
	pthread_mutex_unlock(&g.mu);
	st = pw_stream_get_state(g.stream, NULL);
	if (have && st == PW_STREAM_STATE_STREAMING)
		pw_stream_trigger_process(g.stream);
	wl_display_flush(g.display);
}

static int setup_wayland(void);

static void release_wayland_client(void)
{
	if (g.display)
		wl_display_disconnect(g.display);
	g.display = NULL;
	g.registry = NULL;
	g.shm = NULL;
	g.seat = NULL;
	g.pointer = NULL;
	g.src_mgr = NULL;
	g.cap_mgr = NULL;
	g.source = NULL;
	g.cursor_session = NULL;
	g.noutputs = 0;
	g.output_overflow = 0;
	g.chosen = NULL;
	memset(g.outputs, 0, sizeof g.outputs);
	memset(&g.screen, 0, sizeof g.screen);
	memset(&g.cursor_plane, 0, sizeof g.cursor_plane);
	g.cursor.have_bitmap = 0;
	g.cursor.inside = 0;
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

static int reconnect_wayland(void)
{
	release_wayland_client();
	/* Kopan oturumun fatal bayrağı kapanış değil. Yeni soket gelene kadar
	   beklenir. g.stop, ya da bu bekleme sırasında gelen g.fatal, çıkar. */
	pthread_mutex_lock(&g.mu);
	if (g.stop) {
		pthread_mutex_unlock(&g.mu);
		return -1;
	}
	g.fatal = 0;
	g.fatal_msg = NULL;
	pthread_mutex_unlock(&g.mu);

	for (;;) {
		char path[108];

		pthread_mutex_lock(&g.mu);
		if (g.stop || g.fatal) {
			pthread_mutex_unlock(&g.mu);
			return -1;
		}
		pthread_mutex_unlock(&g.mu);
		if (read_ekran_path(path, sizeof path) == 0 &&
		    setenv("WAYLAND_DISPLAY", path, 1) == 0 &&
		    setup_wayland() == 0) {
			pthread_mutex_lock(&g.mu);
			g.fatal = 0;
			g.fatal_msg = NULL;
			pthread_mutex_unlock(&g.mu);
			fprintf(stderr, "crd-capture: ekran yeniden bağlandı %s\n", path);
			return 0;
		}
		release_wayland_client();
		pthread_mutex_lock(&g.mu);
		if (g.stop) {
			pthread_mutex_unlock(&g.mu);
			return -1;
		}
		g.fatal = 0;
		g.fatal_msg = NULL;
		pthread_mutex_unlock(&g.mu);
		usleep(250000);
	}
}

static void *wayland_thread(void *arg)
{
	(void)arg;
	while (!g.stop) {
		struct pollfd pf[2];
		int timeout_flush;

		while (wl_display_prepare_read(g.display) != 0) {
			if (wl_display_dispatch_pending(g.display) < 0) {
				log_err("wayland olayları okunamadı");
				if (reconnect_wayland() != 0)
					return NULL;
				continue;
			}
		}
		timeout_flush = wl_display_flush(g.display);
		if (timeout_flush < 0 && errno != EAGAIN) {
			wl_display_cancel_read(g.display);
			log_err("wayland flush başarısız");
			if (reconnect_wayland() != 0)
				return NULL;
			continue;
		}
		pf[0].fd = wl_display_get_fd(g.display);
		pf[0].events = POLLIN;
		if (timeout_flush < 0)
			pf[0].events = (short)(pf[0].events | POLLOUT);
		pf[1].fd = g.timerfd;
		pf[1].events = POLLIN;
		if (poll(pf, 2, -1) < 0) {
			wl_display_cancel_read(g.display);
			if (errno == EINTR)
				continue;
			set_fatal("bekleme başarısız");
			return NULL;
		}
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			if (wl_display_read_events(g.display) < 0) {
				log_err("wayland bağlantısı koptu");
				if (reconnect_wayland() != 0)
					return NULL;
				continue;
			}
		} else {
			wl_display_cancel_read(g.display);
		}
		if (wl_display_dispatch_pending(g.display) < 0) {
			log_err("wayland olayları işlenemedi");
			if (reconnect_wayland() != 0)
				return NULL;
			continue;
		}
		if (pf[1].revents & POLLIN) {
			uint64_t expirations = 0;
			ssize_t n = read(g.timerfd, &expirations, sizeof expirations);
			(void)n;
			on_tick();
		}
	}
	return NULL;
}

static void fill_cursor_meta(struct spa_buffer *buf)
{
	struct spa_meta *meta;
	struct spa_meta_cursor *c;
	struct spa_meta_bitmap *bm;
	size_t need;
	int inside, have_bitmap, w, h, stride, hx, hy, x, y;
	uint32_t spa, serial;

	meta = spa_buffer_find_meta(buf, SPA_META_Cursor);
	if (!meta || !meta->data || meta->size < sizeof(struct spa_meta_cursor))
		return;

	pthread_mutex_lock(&g.mu);
	inside = g.cursor.inside;
	have_bitmap = g.cursor.have_bitmap;
	x = g.cursor.x;
	y = g.cursor.y;
	hx = g.cursor.hx;
	hy = g.cursor.hy;
	w = g.cursor.w;
	h = g.cursor.h;
	stride = g.cursor.stride;
	spa = g.cursor.spa;
	serial = g.cursor.serial;
	c = meta->data;
	memset(c, 0, sizeof *c);
	c->id = serial ? serial : 1;
	c->position.x = x;
	c->position.y = y;
	c->hotspot.x = hx;
	c->hotspot.y = hy;
	c->bitmap_offset = 0;
	if (!inside || !have_bitmap) {
		c->flags = CRD_CURSOR_FLAG_HIDDEN;
		pthread_mutex_unlock(&g.mu);
		return;
	}
	need = sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) +
	       (size_t)stride * (size_t)h;
	if (meta->size < need) {
		c->flags = 0;
		pthread_mutex_unlock(&g.mu);
		return;
	}
	c->flags = 0;
	c->bitmap_offset = sizeof(struct spa_meta_cursor);
	bm = SPA_PTROFF(c, c->bitmap_offset, struct spa_meta_bitmap);
	bm->format = spa;
	bm->size.width = (uint32_t)w;
	bm->size.height = (uint32_t)h;
	bm->stride = stride;
	bm->offset = sizeof(struct spa_meta_bitmap);
	memcpy(SPA_PTROFF(bm, bm->offset, uint8_t), g.cursor.pixels,
	       (size_t)stride * (size_t)h);
	pthread_mutex_unlock(&g.mu);
}

/* Tam kare + sıfır boyutlu sonlandırıcı (spa_meta_region_is_valid false). */
static void fill_video_damage_meta(struct spa_buffer *buf, int width, int height)
{
	struct spa_meta *meta;
	struct spa_meta_region *r;

	if (width < 1 || height < 1)
		return;
	meta = spa_buffer_find_meta(buf, SPA_META_VideoDamage);
	if (!meta || !meta->data ||
	    meta->size < 2 * sizeof(struct spa_meta_region))
		return;
	memset(meta->data, 0, meta->size);
	r = meta->data;
	r->region.position.x = 0;
	r->region.position.y = 0;
	r->region.size.width = (uint32_t)width;
	r->region.size.height = (uint32_t)height;
}

static uint32_t g_frame_seq;

static int format_swaps_red_blue(uint32_t fmt)
{
	return fmt == SPA_VIDEO_FORMAT_RGBx || fmt == SPA_VIDEO_FORMAT_RGBA;
}

static int format_supported(uint32_t fmt)
{
	return fmt == SPA_VIDEO_FORMAT_BGRx || fmt == SPA_VIDEO_FORMAT_BGRA ||
	       format_swaps_red_blue(fmt);
}

static const char *spa_format_name(uint32_t fmt)
{
	switch (fmt) {
	case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
	case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
	case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
	case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
	default: return "bilinmeyen";
	}
}

/* Kaynak bellek B,G,R,x. Seçilen biçim RGBx/RGBA ise kırmızı ile mavi yer değiştirir. */
static void scale_bgrx(const uint8_t *src, int src_w, int src_h,
			uint8_t *dst, int dst_w, int dst_h, int swap_rb)
{
	int y;

	if (src_w == dst_w && src_h == dst_h && !swap_rb) {
		memcpy(dst, src, (size_t)src_w * 4u * (size_t)src_h);
		return;
	}
	for (y = 0; y < dst_h; y++) {
		int sy = (int)(((int64_t)y * src_h) / dst_h);
		const uint8_t *srow;
		uint8_t *drow = dst + (size_t)y * (size_t)dst_w * 4u;
		int x;

		if (sy >= src_h)
			sy = src_h - 1;
		srow = src + (size_t)sy * (size_t)src_w * 4u;
		for (x = 0; x < dst_w; x++) {
			int sx = (int)(((int64_t)x * src_w) / dst_w);
			const uint8_t *sp;
			uint8_t *dp;

			if (sx >= src_w)
				sx = src_w - 1;
			sp = srow + (size_t)sx * 4u;
			dp = drow + (size_t)x * 4u;
			if (swap_rb) {
				dp[0] = sp[2];
				dp[1] = sp[1];
				dp[2] = sp[0];
			} else {
				dp[0] = sp[0];
				dp[1] = sp[1];
				dp[2] = sp[2];
			}
			dp[3] = 0xff;
		}
	}
}

static void on_pw_process(void *data)
{
	struct pw_buffer *b;
	struct spa_data *d;
	struct spa_meta_header *header;
	size_t nbytes;
	int stride;
	int dst_w;
	int dst_h;
	int64_t pts = 0;

	(void)data;
	b = pw_stream_dequeue_buffer(g.stream);
	if (!b)
		return;
	d = &b->buffer->datas[0];
	dst_w = g.out_w ? (int)g.out_w : g.info.width;
	dst_h = g.out_h ? (int)g.out_h : g.info.height;
	if (dst_w < 1 || dst_h < 1) {
		pw_stream_return_buffer(g.stream, b);
		return;
	}
	stride = dst_w * 4;
	nbytes = (size_t)stride * (size_t)dst_h;
	if (!d->data || d->maxsize < nbytes) {
		static int small_logged;

		if (!small_logged) {
			fprintf(stderr,
				"crd-capture: tampon küçük max=%u gerek=%zu %dx%d\n",
				d->maxsize, nbytes, dst_w, dst_h);
			small_logged = 1;
		}
		pw_stream_return_buffer(g.stream, b);
		return;
	}
	uint32_t fmt;
	int swap;

	pthread_mutex_lock(&g.mu);
	if (!g.have_frame || g.info.width < 1 || g.info.height < 1) {
		pthread_mutex_unlock(&g.mu);
		pw_stream_return_buffer(g.stream, b);
		return;
	}
	fmt = g.out_format ? g.out_format : SPA_VIDEO_FORMAT_BGRx;
	swap = format_swaps_red_blue(fmt);
	if (!format_supported(fmt)) {
		pthread_mutex_unlock(&g.mu);
		pw_stream_return_buffer(g.stream, b);
		return;
	}
	scale_bgrx(g.front, g.info.width, g.info.height, d->data, dst_w, dst_h, swap);
	pts = g.frame_pts;
	pthread_mutex_unlock(&g.mu);
	d->chunk->offset = 0;
	d->chunk->stride = stride;
	d->chunk->size = (uint32_t)nbytes;
	header = spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof *header);
	if (header) {
		header->seq = ++g_frame_seq;
		header->flags = 0;
		header->pts = pts;
		header->dts_offset = 0;
	}
	fill_cursor_meta(b->buffer);
	fill_video_damage_meta(b->buffer, dst_w, dst_h);
	pw_stream_queue_buffer(g.stream, b);
}

static void on_pw_state(void *data, enum pw_stream_state old, enum pw_stream_state state,
			const char *error)
{
	uint32_t id;
	(void)data;
	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "crd-capture: pipewire: %s\n", error ? error : "hata");
		return;
	}
	if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
		return;
	id = pw_stream_get_node_id(g.stream);
	if (id == 0 || id == SPA_ID_INVALID)
		return;
	pthread_mutex_lock(&g.mu);
	if (g.pw_node_id == 0) {
		g.pw_node_id = id;
		pthread_cond_broadcast(&g.cv);
	}
	pthread_mutex_unlock(&g.mu);
}

static void on_pw_param(void *data, uint32_t id, const struct spa_pod *param)
{
	struct spa_video_info_raw info;

	(void)data;
	if (id != SPA_PARAM_Format || !param)
		return;
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;
	if (info.size.width < CRD_CAPTURE_MIN_W || info.size.height < CRD_CAPTURE_MIN_H ||
	    info.size.width > CRD_CAPTURE_MAX_W || info.size.height > CRD_CAPTURE_MAX_H)
		return;
	if (!format_supported(info.format)) {
		fprintf(stderr, "crd-capture: konak biçimi yok sayıldı %s\n",
			spa_format_name(info.format));
		return;
	}
	g.out_format = info.format;
	g.out_w = info.size.width;
	g.out_h = info.size.height;
	fprintf(stderr, "crd-capture: akış %s %ux%u\n",
		spa_format_name(g.out_format), g.out_w, g.out_h);
	/* Tampon, seçilen boyuta göre büyür. Aynı boyutta tekrar pazarlık
	   konağın biçim listesini bitirir ve akış donar. */
	if (g.buf_w == g.out_w && g.buf_h == g.out_h)
		return;
	{
		uint8_t podmem[1024];
		struct spa_pod_builder b;
		const struct spa_pod *params[1];
		int32_t stride = (int32_t)info.size.width * 4;
		int32_t bytes = stride * (int32_t)info.size.height;

		spa_pod_builder_init(&b, podmem, sizeof podmem);
		params[0] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(bytes),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
				(1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
		if (params[0]) {
			g.buf_w = g.out_w;
			g.buf_h = g.out_h;
			pw_stream_update_params(g.stream, params, 1);
		}
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_pw_state,
	.param_changed = on_pw_param,
	.process = on_pw_process,
};

static struct spa_pod *build_format(struct spa_pod_builder *b)
{
	struct spa_rectangle def = SPA_RECTANGLE((uint32_t)g.info.width,
						 (uint32_t)g.info.height);
	/* Hostun ilk teklifi 1x1. Aralık 1'den başlarsa PipeWire o varsayılanı
	   seçer ve telefon simsiyah bir piksel alır. 1x1 aralık dışında kalsın;
	   kesişim bu monitörün boyuna düşer. Gerçek telefon boyutu yine sığar. */
	struct spa_rectangle min = SPA_RECTANGLE(CRD_CAPTURE_MIN_W, CRD_CAPTURE_MIN_H);
	struct spa_rectangle max = SPA_RECTANGLE(CRD_CAPTURE_MAX_W, CRD_CAPTURE_MAX_H);
	struct spa_fraction defr = SPA_FRACTION(CRD_CAPTURE_MAX_FPS, 1);
	struct spa_fraction minr = SPA_FRACTION(0, 1);
	struct spa_fraction maxr = SPA_FRACTION(60, 1);

	/* Konak BGRx, BGRA, RGBx veya RGBA seçer. Panelin 10 bit biçimi
	   tek Id olarak verilirse liste biter ve akış donar. */
	return spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
			SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRA,
			SPA_VIDEO_FORMAT_RGBx,
			SPA_VIDEO_FORMAT_RGBA),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defr, &minr, &maxr));
}

static void on_probe_state(void *data, enum pw_stream_state old, enum pw_stream_state state,
			  const char *error)
{
	(void)data;
	(void)old;
	if (state != PW_STREAM_STATE_ERROR && state != PW_STREAM_STATE_STREAMING)
		return;
	pthread_mutex_lock(&g.mu);
	if (g.probe_state == 1) {
		if (state == PW_STREAM_STATE_STREAMING)
			g.probe_state = 2;
		else {
			g.probe_state = 3;
			snprintf(g.probe_error, sizeof g.probe_error, "%s",
				 error && error[0] ? error : "bağ kurulamadı");
		}
		pthread_cond_broadcast(&g.cv);
	}
	pthread_mutex_unlock(&g.mu);
}

static void on_probe_param(void *data, uint32_t id, const struct spa_pod *param)
{
	(void)data;
	(void)param;
	if (id != SPA_PARAM_Format)
		return;
	pthread_mutex_lock(&g.mu);
	if (g.probe_state == 1) {
		g.probe_state = 2;
		pthread_cond_broadcast(&g.cv);
	}
	pthread_mutex_unlock(&g.mu);
}

static void on_probe_process(void *data)
{
	struct pw_buffer *b;

	(void)data;
	if (!g.probe)
		return;
	b = pw_stream_dequeue_buffer(g.probe);
	if (b)
		pw_stream_queue_buffer(g.probe, b);
}

static const struct pw_stream_events probe_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_probe_state,
	.param_changed = on_probe_param,
	.process = on_probe_process,
};

/* Konak node.target ile bağlanır ve dont-fallback koymaz. Öğe yoksa
   WirePlumber varsayılan video kaynağına, yani kameraya düşer. */
static int probe_link(uint32_t target)
{
	struct pw_properties *props;
	uint8_t mem[4096];
	struct spa_pod_builder b;
	const struct spa_pod *params[4];
	char target_text[16];
	struct timespec ts;
	int state;
	int32_t stride;
	int32_t bytes;

	pthread_mutex_lock(&g.mu);
	g.probe_state = 1;
	g.probe_error[0] = '\0';
	pthread_mutex_unlock(&g.mu);

	pw_thread_loop_lock(g.pw_loop);
	props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Stream/Input/Video",
				  PW_KEY_MEDIA_NAME, "crd-capture-bag",
				  PW_KEY_NODE_NAME, "crd-capture-bag",
				  PW_KEY_NODE_AUTOCONNECT, "true",
				  "node.dont-fallback", "true",
				  NULL);
	if (!props) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	snprintf(target_text, sizeof target_text, "%u", target);
	pw_properties_set(props, "node.target", target_text);
	memset(&g.probe_hook, 0, sizeof g.probe_hook);
	g.probe = pw_stream_new(g.pw_core, "crd-capture-bag", props);
	if (!g.probe) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	pw_stream_add_listener(g.probe, &g.probe_hook, &probe_events, NULL);
	spa_pod_builder_init(&b, mem, sizeof mem);
	params[0] = build_format(&b);
	stride = g.info.width * 4;
	bytes = stride * g.info.height;
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(bytes, 64, CRD_CAPTURE_MAX_W * CRD_CAPTURE_MAX_H * 4),
		SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(stride, 4, CRD_CAPTURE_MAX_W * 4),
		SPA_PARAM_BUFFERS_dataType,
		SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	if (pw_stream_connect(g.probe, PW_DIRECTION_INPUT, target,
			      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
			      params, 2) < 0) {
		pw_stream_destroy(g.probe);
		g.probe = NULL;
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	pw_thread_loop_unlock(g.pw_loop);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_nsec += 800000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&g.mu);
	while (g.probe_state == 1 && !g.fatal)
		if (pthread_cond_timedwait(&g.cv, &g.mu, &ts) == ETIMEDOUT)
			break;
	state = g.probe_state;
	pthread_mutex_unlock(&g.mu);

	pw_thread_loop_lock(g.pw_loop);
	if (g.probe) {
		pw_stream_destroy(g.probe);
		g.probe = NULL;
	}
	pw_thread_loop_unlock(g.pw_loop);
	return state == 2 ? 0 : -1;
}

static int publish_when_linkable(void)
{
	struct timespec ts;
	int attempt;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += 5;
	pthread_mutex_lock(&g.mu);
	while (g.pw_node_id == 0 && !g.fatal)
		if (pthread_cond_timedwait(&g.cv, &g.mu, &ts) == ETIMEDOUT)
			break;
	pthread_mutex_unlock(&g.mu);
	if (g.pw_node_id == 0 || g.fatal)
		return -1;

	for (attempt = 1; attempt <= 15; attempt++) {
		if (probe_link(g.pw_node_id) == 0) {
			pthread_mutex_lock(&g.mu);
			g.info.node_id = g.pw_node_id;
			pthread_cond_broadcast(&g.cv);
			pthread_mutex_unlock(&g.mu);
			fprintf(stderr, "crd-capture: ekran düğümü %u bağlanabilir\n",
				g.pw_node_id);
			return 0;
		}
		fprintf(stderr, "crd-capture: ekran düğümü %u henüz bağlanamıyor (%s)\n",
			g.pw_node_id, g.probe_error[0] ? g.probe_error : "zaman aşımı");
		usleep(200000);
	}
	log_err("ekran düğümü oturum yöneticisine bağlanamadı");
	return -1;
}

static int pinning;

static void pin_host(uint32_t host_id)
{
	if (pinning || !g.defmeta || host_id == 0 || g.object_serial[0] == '\0')
		return;
	pinning = 1;
	if (pw_metadata_set_property(g.defmeta, host_id, "target.object", "Spa:Id",
				     g.object_serial) < 0) {
		pinning = 0;
		fprintf(stderr, "crd-capture: konak hedefi yazılamadı\n");
		return;
	}
	fprintf(stderr, "crd-capture: konak %u ekrana sabitlendi (%s)\n", host_id,
		g.object_serial);
	pinning = 0;
}

static int on_meta_property(void *data, uint32_t subject, const char *key, const char *type,
			    const char *value)
{
	(void)data;
	(void)type;
	if (subject == 0 || subject != g.host_node_id || !key || strcmp(key, "target.object") != 0)
		return 0;
	if (value && strcmp(value, g.object_serial) == 0)
		return 0;
	pin_host(subject);
	return 0;
}

static const struct pw_metadata_events meta_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_meta_property,
};

static void on_reg_global(void *data, uint32_t id, uint32_t perms, const char *type,
			  uint32_t version, const struct spa_dict *props)
{
	const char *name;
	const char *serial;

	(void)data;
	(void)perms;
	if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		name = props ? spa_dict_lookup(props, "metadata.name") : NULL;
		if (!g.defmeta && name && strcmp(name, "default") == 0) {
			g.defmeta = pw_registry_bind(g.reg, id, type, version, 0);
			if (g.defmeta)
				pw_metadata_add_listener(g.defmeta, &g.meta_hook, &meta_events, NULL);
		}
		return;
	}
	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props)
		return;
	name = spa_dict_lookup(props, "node.name");
	serial = spa_dict_lookup(props, "object.serial");
	if (!name)
		return;
	if (g.node_name[0] != '\0' && strcmp(name, g.node_name) == 0 && serial &&
	    strcmp(serial, g.object_serial) != 0) {
		snprintf(g.object_serial, sizeof g.object_serial, "%s", serial);
		if (g.host_node_id)
			pin_host(g.host_node_id);
		return;
	}
	if (strcmp(name, "chrome-remote-desktop-host") == 0) {
		g.host_node_id = id;
		pin_host(id);
	}
}

static const struct pw_registry_events reg_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_reg_global,
};

static void arm_registry(void)
{
	g.reg = pw_core_get_registry(g.pw_core, PW_VERSION_REGISTRY, 0);
	if (!g.reg)
		return;
	memset(&g.reg_hook, 0, sizeof g.reg_hook);
	memset(&g.meta_hook, 0, sizeof g.meta_hook);
	pw_registry_add_listener(g.reg, &g.reg_hook, &reg_events, NULL);
}

static int start_pipewire(void)
{
	struct pw_properties *props;
	uint8_t podmem[8192];
	struct spa_pod_builder builder;
	const struct spa_pod *params[8];
	int n = 0;
	int meta_size;

	pw_init(NULL, NULL);
	g.pw_loop = pw_thread_loop_new("crd-capture", NULL);
	if (!g.pw_loop)
		return -1;
	pw_thread_loop_lock(g.pw_loop);
	g.pw_context = pw_context_new(pw_thread_loop_get_loop(g.pw_loop), NULL, 0);
	if (!g.pw_context) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	g.pw_core = pw_context_connect(g.pw_context, NULL, 0);
	if (!g.pw_core) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	arm_registry();
	/* Video/Source varsayılan kamera adayıdır. Açılışta kamera geç
	   gelirse bu düğüm varsayılan olur, konak "varsayılanı izle" diye
	   işaretlenir, kamera gelince akış oraya taşınır ve biçim uyuşmazlığıyla
	   ölür. Source/Screen listeye girmez; node.target ile bağlanmak sürer. */
	props = pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Video/Source/Screen",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Screen",
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_NODE_NAME, g.node_name,
		PW_KEY_NODE_DESCRIPTION, g.output_desc[0] ? g.output_desc : g.output_name,
		PW_KEY_MEDIA_NAME, g.output_name,
		PW_KEY_NODE_DRIVER, "true",
		PW_KEY_VIDEO_FORMAT, "BGRx",
		NULL);
	if (!props) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	pw_properties_set(props, "crd.capture.source-format", g.source_format);
	pw_properties_setf(props, PW_KEY_VIDEO_SIZE, "%dx%d", g.info.width, g.info.height);
	pw_properties_setf(props, PW_KEY_VIDEO_RATE, "%d/1", CRD_CAPTURE_MAX_FPS);
	g.stream = pw_stream_new(g.pw_core, g.output_name, props);
	if (!g.stream) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	pw_stream_add_listener(g.stream, &g.stream_hook, &stream_events, NULL);
	g.out_format = SPA_VIDEO_FORMAT_BGRx;
	g.buf_w = (uint32_t)g.info.width;
	g.buf_h = (uint32_t)g.info.height;

	spa_pod_builder_init(&builder, podmem, sizeof podmem);
	params[n++] = build_format(&builder);
	meta_size = (int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) +
			  (size_t)CURSOR_MAX * CURSOR_MAX * 4);
	params[n++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_Int(meta_size));
	params[n++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
	params[n++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, SPA_POD_Int((int)(2 * sizeof(struct spa_meta_region))));
	{
		int32_t stride = g.info.width * 4;
		int32_t bytes = stride * g.info.height;
		int32_t max_bytes = CRD_CAPTURE_MAX_W * CRD_CAPTURE_MAX_H * 4;
		params[n++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(bytes, 64, max_bytes),
			SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(stride, 4, CRD_CAPTURE_MAX_W * 4),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	}
	if (pw_stream_connect(g.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			      PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
			      params, (uint32_t)n) < 0) {
		pw_thread_loop_unlock(g.pw_loop);
		return -1;
	}
	pw_thread_loop_start(g.pw_loop);
	pw_thread_loop_unlock(g.pw_loop);
	g.linked_w = g.info.width;
	g.linked_h = g.info.height;
	return publish_when_linkable();
}

static int pipewire_socket_path(char *out, size_t n)
{
	const char *remote = getenv("PIPEWIRE_REMOTE");
	const char *dir;

	if (!remote || !remote[0])
		remote = "pipewire-0";
	if (remote[0] == '/') {
		if (strlen(remote) + 1 > n)
			return -1;
		memcpy(out, remote, strlen(remote) + 1);
		return 0;
	}
	dir = getenv("PIPEWIRE_RUNTIME_DIR");
	if (!dir || !dir[0])
		dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !dir[0])
		return -1;
	if (snprintf(out, n, "%s/%s", dir, remote) >= (int)n)
		return -1;
	return 0;
}

static void arm_timer(void)
{
	struct itimerspec its;

	memset(&its, 0, sizeof its);
	its.it_value.tv_nsec = 1000000L;
	its.it_interval.tv_nsec = CRD_CAPTURE_INTERVAL_NS;
	timerfd_settime(g.timerfd, 0, &its, NULL);
}

static int install_frames(int32_t x, int32_t y, int32_t w, int32_t h,
			  const char *mapping)
{
	size_t bytes;
	uint8_t *front, *back, *old_front, *old_back;

	if (w < 1 || h < 1)
		return -1;
	bytes = (size_t)w * (size_t)h * 4u;
	front = malloc(bytes);
	back = malloc(bytes);
	if (!front || !back) {
		free(front);
		free(back);
		log_err("kare belleği ayrılamadı");
		return -1;
	}
	memset(front, 0, bytes);
	memset(back, 0, bytes);
	pthread_mutex_lock(&g.mu);
	old_front = g.front;
	old_back = g.back;
	g.front = front;
	g.back = back;
	g.have_frame = 0;
	g.info.x = x;
	g.info.y = y;
	g.info.width = w;
	g.info.height = h;
	snprintf(g.info.mapping_id, sizeof g.info.mapping_id, "%s",
		 mapping ? mapping : "");
	pthread_mutex_unlock(&g.mu);
	free(old_front);
	free(old_back);
	return 0;
}

static void publish_output_info(int32_t x, int32_t y, int32_t w, int32_t h,
				const char *mapping)
{
	pthread_mutex_lock(&g.mu);
	g.info.x = x;
	g.info.y = y;
	g.info.width = w;
	g.info.height = h;
	snprintf(g.info.mapping_id, sizeof g.info.mapping_id, "%s",
		 mapping ? mapping : "");
	pthread_mutex_unlock(&g.mu);
}

/* Boyut değişince düğüm ve kimlik durur. Tampon tam kare olmalı;
   on_pw_param ile aynı sabit boy, yoksa 8192 baytlık boş kare gelir. */
static int retarget_stream_size(void)
{
	uint8_t podmem[8192];
	struct spa_pod_builder b;
	const struct spa_pod *params[2];
	int32_t w, h, stride, bytes;
	uint32_t old_bw, old_bh;

	if (!g.stream || !g.pw_loop)
		return 0;
	pthread_mutex_lock(&g.mu);
	w = g.info.width;
	h = g.info.height;
	pthread_mutex_unlock(&g.mu);
	if (w < 1 || h < 1)
		return -1;
	stride = w * 4;
	bytes = stride * h;
	pw_thread_loop_lock(g.pw_loop);
	old_bw = g.buf_w;
	old_bh = g.buf_h;
	g.buf_w = (uint32_t)w;
	g.buf_h = (uint32_t)h;
	spa_pod_builder_init(&b, podmem, sizeof podmem);
	params[0] = build_format(&b);
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(bytes),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
			(1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	if (!params[0] || !params[1] ||
	    pw_stream_update_params(g.stream, params, 2) < 0) {
		g.buf_w = old_bw;
		g.buf_h = old_bh;
		pw_thread_loop_unlock(g.pw_loop);
		log_err("akış boyutu güncellenemedi");
		return -1;
	}
	g.linked_w = w;
	g.linked_h = h;
	pw_thread_loop_unlock(g.pw_loop);
	fprintf(stderr, "crd-capture: akış boyutu %dx%d\n", w, h);
	return 0;
}

static int setup_wayland(void)
{
	int i;
	int32_t new_w, new_h, prev_w, prev_h;
	int size_changed;

	g.display = wl_display_connect(NULL);
	if (!g.display) {
		log_err("wayland görüntüsüne bağlanılamadı");
		return -1;
	}
	g.registry = wl_display_get_registry(g.display);
	wl_registry_add_listener(g.registry, &registry_listener, NULL);
	if (wl_display_roundtrip(g.display) < 0 || wl_display_roundtrip(g.display) < 0) {
		log_err("wayland turu başarısız");
		return -1;
	}
	/* ext-image-copy-capture tam değilse ve KWin akış arayüzü varsa KWin yolu. */
	g.kwin = (!g.src_mgr || !g.cap_mgr) && g.kwin_sc != NULL;
	if (!g.kwin && (!g.shm || !g.src_mgr || !g.cap_mgr || !g.pointer)) {
		fprintf(stderr,
			"crd-capture: eksik nesne shm=%d kaynak=%d yakalama=%d koltuk=%d işaretçi=%d çıkış=%d WAYLAND_DISPLAY=%s\n",
			g.shm != NULL, g.src_mgr != NULL, g.cap_mgr != NULL, g.seat != NULL,
			g.pointer != NULL, g.noutputs,
			getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(yok)");
		return -1;
	}
	if (g.output_overflow) {
		log_err("wl_output sayısı sınırı aşıldı");
		return -1;
	}
	{
		struct crd_cikti want;
		char derived[160];

		if (crd_cikti_bul(&want) != 0) {
			log_err("çıktı seçilemedi");
			return -1;
		}
		if (crd_cikti_dugum_adi(want.name, derived, sizeof derived) != 0) {
			log_err("düğüm adı türetilemedi");
			return -1;
		}
		g.chosen = NULL;
		for (i = 0; i < g.noutputs; i++) {
			if (g.outputs[i].have_mode && strcmp(g.outputs[i].name, want.name) == 0)
				g.chosen = &g.outputs[i];
		}
		if (!g.chosen) {
			fprintf(stderr, "crd-capture: seçilen çıktı %s wl_output listesinde yok\n",
				want.name);
			return -1;
		}
		snprintf(g.output_name, sizeof g.output_name, "%s", want.name);
		snprintf(g.output_desc, sizeof g.output_desc, "%s", want.description);
		g.logical_x = want.x;
		g.logical_y = want.y;
		g.logical_w = want.width;
		g.logical_h = want.height;
		if (!g.stream) {
			snprintf(g.node_name, sizeof g.node_name, "%s", derived);
		} else if (strcmp(g.node_name, derived) != 0) {
			fprintf(stderr, "crd-capture: çıktı düğümü %s, kurulu düğüm %s duruyor\n",
				derived, g.node_name);
		}
		fprintf(stderr, "crd-capture: düğüm %s\n", g.node_name);
	}
	pthread_mutex_lock(&g.mu);
	prev_w = g.info.width;
	prev_h = g.info.height;
	pthread_mutex_unlock(&g.mu);
	new_w = g.logical_w;
	new_h = g.logical_h;
	size_changed = new_w != prev_w || new_h != prev_h;

	if (g.kwin) {
		publish_output_info(g.chosen->x, g.chosen->y, new_w, new_h, g.chosen->name);
		return 0;
	}
	g.source = ext_output_image_capture_source_manager_v1_create_source(g.src_mgr, g.chosen->output);
	if (!g.source) {
		log_err("görüntü kaynağı kurulamadı");
		return -1;
	}
	g.screen.is_cursor = 0;
	g.screen.session = ext_image_copy_capture_manager_v1_create_session(g.cap_mgr, g.source, 0);
	if (!g.screen.session) {
		log_err("ekran oturumu kurulamadı");
		return -1;
	}
	ext_image_copy_capture_session_v1_add_listener(g.screen.session, &session_listener, &g.screen);

	g.cursor_plane.is_cursor = 1;
	g.cursor_session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
		g.cap_mgr, g.source, g.pointer);
	if (!g.cursor_session) {
		log_err("imleç oturumu kurulamadı");
		return -1;
	}
	ext_image_copy_capture_cursor_session_v1_add_listener(g.cursor_session, &cursor_listener, NULL);
	g.cursor_plane.session = ext_image_copy_capture_cursor_session_v1_get_capture_session(g.cursor_session);
	if (!g.cursor_plane.session) {
		log_err("imleç kare oturumu kurulamadı");
		return -1;
	}
	ext_image_copy_capture_session_v1_add_listener(g.cursor_plane.session, &session_listener, &g.cursor_plane);

	if (wl_display_roundtrip(g.display) < 0 || g.fatal) {
		log_err(g.fatal_msg ? g.fatal_msg : "kısıt turu başarısız");
		return -1;
	}
	if (!g.screen.have_constraints || !g.screen.buffer) {
		log_err("ekran kısıtları gelmedi");
		return -1;
	}
	if (g.screen.width != (uint32_t)new_w || g.screen.height != (uint32_t)new_h) {
		fprintf(stderr,
			"crd-capture: yakalama %ux%u, xdg mantıksal %dx%d, wl_output modu %dx%d\n",
			g.screen.width, g.screen.height, g.logical_w, g.logical_h,
			g.chosen->width, g.chosen->height);
		log_err("yakalama boyutu xdg mantıksal boyut ile uyuşmuyor");
		return -1;
	}
	if (g.spa_format != SPA_VIDEO_FORMAT_RGBx && g.spa_format != SPA_VIDEO_FORMAT_BGRx) {
		log_err("akış biçimi kurulamadı");
		return -1;
	}
	if (!g.front || !g.back || size_changed) {
		if (install_frames(g.logical_x, g.logical_y, new_w, new_h,
				   g.output_name) < 0)
			return -1;
	} else {
		publish_output_info(g.logical_x, g.logical_y, new_w, new_h,
				    g.output_name);
	}
	if (prev_w > 0 && size_changed)
		fprintf(stderr, "crd-capture: ekran %dx%d -> %dx%d\n",
			prev_w, prev_h, new_w, new_h);
	if (g.stream && (new_w != g.linked_w || new_h != g.linked_h)) {
		if (retarget_stream_size() < 0)
			return -1;
	}
	return 0;
}

static void on_kwin_created(void *data, struct zkde_screencast_stream_unstable_v1 *s,
			    uint32_t node)
{
	(void)data;
	(void)s;
	pthread_mutex_lock(&g.mu);
	g.info.node_id = node;
	g.kwin_state = 1;
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.mu);
	fprintf(stderr, "crd-capture: KWin akışı düğüm %u\n", node);
}

static void on_kwin_failed(void *data, struct zkde_screencast_stream_unstable_v1 *s,
			   const char *error)
{
	(void)data;
	(void)s;
	fprintf(stderr, "crd-capture: KWin akışı başarısız: %s\n", error ? error : "?");
	g.kwin_state = -1;
	set_fatal("KWin akışı başarısız");
}

static void on_kwin_closed(void *data, struct zkde_screencast_stream_unstable_v1 *s)
{
	(void)data;
	(void)s;
	g.kwin_state = -1;
	set_fatal("KWin akışı kapandı");
}

static const struct zkde_screencast_stream_unstable_v1_listener kwin_stream_listener = {
	.closed = on_kwin_closed,
	.created = on_kwin_created,
	.failed = on_kwin_failed,
};

/* KWin olaylarını dağıtır. Bağlantı ya da akış giderse fatal; koltuk
   değişiminde yeniden bağlanma bu yolda yok. Test edilmedi. */
static void *kwin_thread(void *arg)
{
	(void)arg;
	while (!g.stop && g.kwin_state >= 0) {
		struct pollfd pf;

		while (wl_display_prepare_read(g.display) != 0)
			if (wl_display_dispatch_pending(g.display) < 0)
				goto lost;
		if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(g.display);
			goto lost;
		}
		pf.fd = wl_display_get_fd(g.display);
		pf.events = POLLIN;
		if (poll(&pf, 1, 250) > 0) {
			if (wl_display_read_events(g.display) < 0)
				goto lost;
		} else {
			wl_display_cancel_read(g.display);
		}
		if (wl_display_dispatch_pending(g.display) < 0)
			goto lost;
	}
	return NULL;
lost:
	set_fatal("KWin bağlantısı koptu");
	return NULL;
}

static int kwin_start(void)
{
	int i;

	g.kwin_stream = zkde_screencast_unstable_v1_stream_output(
		g.kwin_sc, g.chosen->output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA);
	if (!g.kwin_stream) {
		log_err("KWin akışı istenemedi");
		return -1;
	}
	zkde_screencast_stream_unstable_v1_add_listener(g.kwin_stream, &kwin_stream_listener, NULL);
	for (i = 0; i < 50 && g.kwin_state == 0; i++)
		if (wl_display_roundtrip(g.display) < 0)
			break;
	if (g.kwin_state != 1) {
		log_err(g.fatal_msg ? g.fatal_msg : "KWin akış düğümü gelmedi");
		return -1;
	}
	if (pthread_create(&g.thread, NULL, kwin_thread, NULL) != 0) {
		log_err("KWin iş parçacığı başlamadı");
		return -1;
	}
	g.thread_started = 1;
	g.running = 1;
	return 0;
}

int capture_init(void)
{
	pthread_condattr_t attr;

	if (g.running)
		return 0;
	if (g.sync_ready && g.fatal)
		return -1;

	memset(&g, 0, sizeof g);
	pthread_mutex_init(&g.mu, NULL);
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&g.cv, &attr);
	pthread_condattr_destroy(&attr);
	g.sync_ready = 1;
	g.timerfd = -1;

	if (setup_wayland() < 0)
		return -1;
	if (g.kwin)
		return kwin_start();
	if (start_pipewire() < 0) {
		log_err("pipewire düğümü kurulamadı");
		return -1;
	}
	g.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (g.timerfd < 0) {
		log_err("zamanlayıcı kurulamadı");
		return -1;
	}
	arm_timer();
	if (pthread_create(&g.thread, NULL, wayland_thread, NULL) != 0) {
		log_err("wayland iş parçacığı başlamadı");
		return -1;
	}
	g.thread_started = 1;

	pthread_mutex_lock(&g.mu);
	if (wait_flag(&g.have_frame, 4000) < 0) {
		pthread_mutex_unlock(&g.mu);
		log_err(g.fatal_msg ? g.fatal_msg : "ilk kare gelmedi");
		return -1;
	}
	pthread_mutex_unlock(&g.mu);
	g.running = 1;
	return 0;
}

int capture_stream_info(struct crd_stream_info *out)
{
	if (!out || !g.running)
		return -1;
	pthread_mutex_lock(&g.mu);
	*out = g.info;
	pthread_mutex_unlock(&g.mu);
	if (out->node_id == 0 || out->width <= 0 || out->height <= 0 || out->mapping_id[0] == '\0')
		return -1;
	return 0;
}

int capture_open_pipewire_remote(void)
{
	char path[108];
	struct sockaddr_un addr;
	int fd;

	if (!g.running)
		return -1;
	if (pipewire_socket_path(path, sizeof path) < 0)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path) + 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}
