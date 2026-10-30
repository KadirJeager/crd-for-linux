/* GTK'nin gtk_init'i bir Wayland ekranı ister. Karşılama ekranının
 * soketi girişte kapanır ve host ölür. Bu süreç hiç kapanmayan boş bir
 * ekran sunar; görüntü buradan alınmaz, portal gerçek oturumdan alır.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server.h>
#include "gen/xdg-shell-server.h"

static void res_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	res_destroy(client, resource);
}

static const struct wl_buffer_interface buffer_impl = {
	.destroy = buffer_destroy,
};

static void pool_create_buffer(struct wl_client *client, struct wl_resource *resource,
			       uint32_t id, int32_t offset, int32_t width,
			       int32_t height, int32_t stride, uint32_t format)
{
	struct wl_resource *buffer;

	(void)resource;
	(void)offset;
	(void)width;
	(void)height;
	(void)stride;
	(void)format;
	buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(buffer, &buffer_impl, NULL, NULL);
}

static void pool_resize(struct wl_client *client, struct wl_resource *resource,
			int32_t size)
{
	(void)client;
	(void)resource;
	(void)size;
}

static const struct wl_shm_pool_interface pool_impl = {
	.create_buffer = pool_create_buffer,
	.destroy = res_destroy,
	.resize = pool_resize,
};

static void shm_create_pool(struct wl_client *client, struct wl_resource *resource,
			    uint32_t id, int32_t fd, int32_t size)
{
	struct wl_resource *pool;

	(void)resource;
	(void)size;
	close(fd);
	pool = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
	if (!pool) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(pool, &pool_impl, NULL, NULL);
}

static const struct wl_shm_interface shm_impl = {
	.create_pool = shm_create_pool,
	.release = res_destroy,
};

static void bind_shm(struct wl_client *client, void *data, uint32_t version,
		     uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &wl_shm_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &shm_impl, NULL, NULL);
	wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
	wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
			   struct wl_resource *buffer, int32_t x, int32_t y)
{
	(void)client;
	(void)resource;
	(void)buffer;
	(void)x;
	(void)y;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
			   int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource,
			  uint32_t callback)
{
	struct wl_resource *cb;

	(void)resource;
	cb = wl_resource_create(client, &wl_callback_interface, 1, callback);
	if (!cb) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_callback_send_done(cb, 0);
	wl_resource_destroy(cb);
}

static void surface_region(struct wl_client *client, struct wl_resource *resource,
			   struct wl_resource *region)
{
	(void)client;
	(void)resource;
	(void)region;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void surface_transform(struct wl_client *client, struct wl_resource *resource,
			      int32_t transform)
{
	(void)client;
	(void)resource;
	(void)transform;
}

static void surface_scale(struct wl_client *client, struct wl_resource *resource,
			  int32_t scale)
{
	(void)client;
	(void)resource;
	(void)scale;
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource,
			   int32_t x, int32_t y)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
}

static const struct wl_surface_interface surface_impl = {
	.destroy = res_destroy,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.set_opaque_region = surface_region,
	.set_input_region = surface_region,
	.commit = surface_commit,
	.set_buffer_transform = surface_transform,
	.set_buffer_scale = surface_scale,
	.damage_buffer = surface_damage,
	.offset = surface_offset,
	.get_release = surface_frame,
};

static void compositor_create_surface(struct wl_client *client,
				      struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *surface;

	(void)resource;
	surface = wl_resource_create(client, &wl_surface_interface, 6, id);
	if (!surface) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface, &surface_impl, NULL, NULL);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
		       int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static const struct wl_region_interface region_impl = {
	.destroy = res_destroy,
	.add = region_add,
	.subtract = region_add,
};

static void compositor_create_region(struct wl_client *client,
				     struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *region;

	(void)resource;
	region = wl_resource_create(client, &wl_region_interface, 1, id);
	if (!region) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
	.release = res_destroy,
};

static void bind_compositor(struct wl_client *client, void *data,
			    uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	if (version > 6)
		version = 6;
	resource = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, NULL, NULL);
}

static void subsurface_position(struct wl_client *client, struct wl_resource *resource,
				int32_t x, int32_t y)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
}

static void subsurface_place(struct wl_client *client, struct wl_resource *resource,
			     struct wl_resource *sibling)
{
	(void)client;
	(void)resource;
	(void)sibling;
}

static void subsurface_sync(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static const struct wl_subsurface_interface subsurface_impl = {
	.destroy = res_destroy,
	.set_position = subsurface_position,
	.place_above = subsurface_place,
	.place_below = subsurface_place,
	.set_sync = subsurface_sync,
	.set_desync = subsurface_sync,
};

static void subcompositor_get(struct wl_client *client, struct wl_resource *resource,
			      uint32_t id, struct wl_resource *surface,
			      struct wl_resource *parent)
{
	struct wl_resource *sub;

	(void)resource;
	(void)surface;
	(void)parent;
	sub = wl_resource_create(client, &wl_subsurface_interface, 1, id);
	if (!sub) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(sub, &subsurface_impl, NULL, NULL);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = res_destroy,
	.get_subsurface = subcompositor_get,
};

static void bind_named(struct wl_client *client, void *data, uint32_t version,
		       uint32_t id)
{
	const struct wl_interface *iface = data;
	struct wl_resource *resource;
	const void *impl = NULL;

	if (iface == &wl_subcompositor_interface)
		impl = &subcompositor_impl;
	resource = wl_resource_create(client, iface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	if (impl)
		wl_resource_set_implementation(resource, impl, NULL, NULL);
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
			       uint32_t serial, struct wl_resource *surface,
			       int32_t hotspot_x, int32_t hotspot_y)
{
	(void)client;
	(void)resource;
	(void)serial;
	(void)surface;
	(void)hotspot_x;
	(void)hotspot_y;
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor,
	.release = res_destroy,
};

static const struct wl_keyboard_interface keyboard_impl = {
	.release = res_destroy,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource,
			     uint32_t id)
{
	struct wl_resource *pointer;

	(void)resource;
	pointer = wl_resource_create(client, &wl_pointer_interface, 8, id);
	if (!pointer) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(pointer, &pointer_impl, NULL, NULL);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource,
			      uint32_t id)
{
	struct wl_resource *keyboard;

	(void)resource;
	keyboard = wl_resource_create(client, &wl_keyboard_interface, 8, id);
	if (!keyboard) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(keyboard, &keyboard_impl, NULL, NULL);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource,
			   uint32_t id)
{
	(void)client;
	(void)resource;
	(void)id;
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = res_destroy,
};

static void bind_seat(struct wl_client *client, void *data, uint32_t version,
		      uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	if (version > 8)
		version = 8;
	resource = wl_resource_create(client, &wl_seat_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &seat_impl, NULL, NULL);
	wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER);
	if (version >= 2)
		wl_seat_send_name(resource, "crd");
}

static const struct wl_output_interface output_impl = {
	.release = res_destroy,
};

static void bind_output(struct wl_client *client, void *data, uint32_t version,
			uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	if (version > 4)
		version = 4;
	resource = wl_resource_create(client, &wl_output_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &output_impl, NULL, NULL);
	/* gtk_init için sentetik wl_output. Ad CRD-1; yakalama bu soketi okumaz.
	   2560×1440 ve 600×340 mm bu panelin ölçüsüdür ama kompozitörden okunmaz:
	   süreç sunucudur, /run/crd-for-linux/ekran dosyası bağ anında vardır,
	   okumak ayrı iştir. Sayılar bilinçli olarak duruyor. */
	wl_output_send_geometry(resource, 0, 0, 600, 340, 0, "CRD", "gtk",
				WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(resource,
			    WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
			    2560, 1440, 60000);
	if (version >= 2) {
		wl_output_send_scale(resource, 1);
		wl_output_send_done(resource);
	}
	if (version >= 4)
		wl_output_send_name(resource, "CRD-1");
}

static void wm_create_positioner(struct wl_client *client, struct wl_resource *resource,
				uint32_t id)
{
	struct wl_resource *positioner;

	(void)resource;
	positioner = wl_resource_create(client, &xdg_positioner_interface, 1, id);
	if (!positioner)
		wl_client_post_no_memory(client);
}

static void wm_get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
			       uint32_t id, struct wl_resource *surface)
{
	struct wl_resource *xdg;

	(void)resource;
	(void)surface;
	xdg = wl_resource_create(client, &xdg_surface_interface, 1, id);
	if (!xdg)
		wl_client_post_no_memory(client);
}

static void wm_pong(struct wl_client *client, struct wl_resource *resource,
		    uint32_t serial)
{
	(void)client;
	(void)resource;
	(void)serial;
}

static const struct xdg_wm_base_interface wm_impl = {
	.destroy = res_destroy,
	.create_positioner = wm_create_positioner,
	.get_xdg_surface = wm_get_xdg_surface,
	.pong = wm_pong,
};

static void bind_wm(struct wl_client *client, void *data, uint32_t version,
		    uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	if (version > 1)
		version = 1;
	resource = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wm_impl, NULL, NULL);
}

static int listen_socket(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	unlink(path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof addr.sun_path) {
		close(fd);
		return -1;
	}
	memcpy(addr.sun_path, path, strlen(path) + 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}
	chmod(path, 0666);
	return fd;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/run/crd-for-linux/gtk.sock";
	struct wl_display *display;
	int fd;

	signal(SIGPIPE, SIG_IGN);
	fd = listen_socket(path);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	display = wl_display_create();
	if (!display || wl_display_add_socket_fd(display, fd) < 0) {
		fprintf(stderr, "ekran soketi eklenemedi\n");
		return 1;
	}
	wl_global_create(display, &wl_compositor_interface, 6, NULL, bind_compositor);
	wl_global_create(display, &wl_subcompositor_interface, 1,
			 (void *)&wl_subcompositor_interface, bind_named);
	wl_global_create(display, &wl_shm_interface, 2, NULL, bind_shm);
	wl_global_create(display, &wl_seat_interface, 8, NULL, bind_seat);
	wl_global_create(display, &wl_output_interface, 4, NULL, bind_output);
	wl_global_create(display, &xdg_wm_base_interface, 1, NULL, bind_wm);
	wl_display_run(display);
	wl_display_destroy(display);
	return 0;
}
