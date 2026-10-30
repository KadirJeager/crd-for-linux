#include <wayland-util.h>

/* ext-image-capture-source-v1.xml yabancı pencere kaynağına da işaret eder.
 * Bu istemci yalnız wl_output kaynağını bağlar; o arayüz hiç kullanılmaz. */
const struct wl_interface ext_foreign_toplevel_handle_v1_interface = {
	"ext_foreign_toplevel_handle_v1", 1, 0, NULL, 0, NULL
};
