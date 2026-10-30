/* NetworkMonitor sürüm 3 kütüphanesi.
   İmzalar /usr/share/dbus-1/interfaces/org.freedesktop.portal.NetworkMonitor.xml
   belgesinden okunur. Bu dosya veriyolu açmaz ve ad almaz. */

#include "netmon.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>

#define NETMON_XML "/usr/share/dbus-1/interfaces/org.freedesktop.portal.NetworkMonitor.xml"
#define NETMON_PATH "/org/freedesktop/portal/desktop"
#define NETMON_IFACE "org.freedesktop.portal.NetworkMonitor"
#define NETMON_VERSION 3u

typedef struct {
  GDBusConnection *connection;
  GDBusNodeInfo *node;
  guint registration_id;
  guint changed_emit_idle;
  gboolean stopping;
  gboolean document_ok;
  gulong network_changed_id;
  gulong notify_available_id;
  gulong notify_metered_id;
  gulong notify_connectivity_id;
} State;

typedef struct {
  const char *name;
  const char *in0;
  const char *in0_sig;
  const char *in1;
  const char *in1_sig;
  const char *out0;
  const char *out0_sig;
} MethodSpec;

static State g_state;
static char *g_error_text;

static const MethodSpec k_methods[] = {
  {"GetAvailable", NULL, NULL, NULL, NULL, "available", "b"},
  {"GetMetered", NULL, NULL, NULL, NULL, "metered", "b"},
  {"GetConnectivity", NULL, NULL, NULL, NULL, "connectivity", "u"},
  {"GetStatus", NULL, NULL, NULL, NULL, "status", "a{sv}"},
  {"CanReach", "hostname", "s", "port", "u", "reachable", "b"},
};

static void
set_error(const char *message)
{
  g_free(g_error_text);
  g_error_text = g_strdup(message != NULL ? message : "bilinmeyen hata");
}

static void
set_error_printf(const char *fmt, ...)
{
  va_list ap;
  char *text;

  va_start(ap, fmt);
  text = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_free(g_error_text);
  g_error_text = text;
}

const char *
netmon_error(void)
{
  return g_error_text;
}

static guint
count_ptrs(void **items)
{
  guint count = 0;

  if (items == NULL)
    return 0;
  while (items[count] != NULL)
    count++;
  return count;
}

static gboolean
arg_matches(GDBusArgInfo **args, guint index, const char *name, const char *signature)
{
  if (args == NULL || args[index] == NULL)
    return FALSE;
  return g_strcmp0(args[index]->name, name) == 0 &&
         g_strcmp0(args[index]->signature, signature) == 0;
}

static gboolean
interface_matches_document(GDBusInterfaceInfo *iface, GError **error)
{
  guint i;
  guint method_count;
  GDBusSignalInfo *changed;
  GDBusPropertyInfo *version;

  if (iface == NULL || g_strcmp0(iface->name, NETMON_IFACE) != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "arayüz adı yok");
    return FALSE;
  }

  method_count = count_ptrs((void **) iface->methods);
  if (method_count != G_N_ELEMENTS(k_methods)) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                "metot sayısı %u", method_count);
    return FALSE;
  }

  for (i = 0; i < G_N_ELEMENTS(k_methods); i++) {
    const MethodSpec *spec = &k_methods[i];
    GDBusMethodInfo *method;
    guint in_count;
    guint out_count;

    method = g_dbus_interface_info_lookup_method(iface, spec->name);
    if (method == NULL) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "metot yok: %s", spec->name);
      return FALSE;
    }
    in_count = count_ptrs((void **) method->in_args);
    out_count = count_ptrs((void **) method->out_args);
    if (spec->in0 == NULL && in_count != 0) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s giriş imzası", spec->name);
      return FALSE;
    }
    if (spec->in0 != NULL && (in_count < 1 || !arg_matches(method->in_args, 0, spec->in0, spec->in0_sig))) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s ilk giriş", spec->name);
      return FALSE;
    }
    if (spec->in1 == NULL && in_count > 1) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s fazla giriş", spec->name);
      return FALSE;
    }
    if (spec->in1 != NULL && (in_count != 2 || !arg_matches(method->in_args, 1, spec->in1, spec->in1_sig))) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s ikinci giriş", spec->name);
      return FALSE;
    }
    if (out_count != 1 || !arg_matches(method->out_args, 0, spec->out0, spec->out0_sig)) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s çıkış imzası", spec->name);
      return FALSE;
    }
  }

  if (count_ptrs((void **) iface->signals) != 1) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "sinyal sayısı");
    return FALSE;
  }
  changed = g_dbus_interface_info_lookup_signal(iface, "changed");
  if (changed == NULL || count_ptrs((void **) changed->args) != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "changed sinyali");
    return FALSE;
  }

  if (count_ptrs((void **) iface->properties) != 1) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "özellik sayısı");
    return FALSE;
  }
  version = g_dbus_interface_info_lookup_property(iface, "version");
  if (version == NULL || g_strcmp0(version->signature, "u") != 0 ||
      (version->flags & G_DBUS_PROPERTY_INFO_FLAGS_READABLE) == 0 ||
      (version->flags & G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE) != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "version özelliği");
    return FALSE;
  }
  return TRUE;
}

static gboolean
load_interface(GError **error)
{
  char *xml = NULL;
  GDBusInterfaceInfo *iface;

  if (g_state.document_ok && g_state.node != NULL)
    return TRUE;

  if (g_state.node != NULL) {
    g_dbus_node_info_unref(g_state.node);
    g_state.node = NULL;
  }
  g_state.document_ok = FALSE;

  if (!g_file_get_contents(NETMON_XML, &xml, NULL, error))
    return FALSE;
  g_state.node = g_dbus_node_info_new_for_xml(xml, error);
  g_free(xml);
  if (g_state.node == NULL)
    return FALSE;
  if (g_state.node->interfaces == NULL || g_state.node->interfaces[0] == NULL ||
      g_state.node->interfaces[1] != NULL) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "belgede tek arayüz yok");
    g_dbus_node_info_unref(g_state.node);
    g_state.node = NULL;
    return FALSE;
  }
  iface = g_dbus_node_info_lookup_interface(g_state.node, NETMON_IFACE);
  if (!interface_matches_document(iface, error)) {
    g_dbus_node_info_unref(g_state.node);
    g_state.node = NULL;
    return FALSE;
  }
  g_state.document_ok = TRUE;
  return TRUE;
}

static const char *
real_monitor_name(void)
{
  const char *name;

  name = G_OBJECT_TYPE_NAME(g_network_monitor_get_default());
  if (g_strcmp0(name, "GNetworkMonitorNM") == 0 ||
      g_strcmp0(name, "GNetworkMonitorNetlink") == 0)
    return name;
  return NULL;
}

const char *
netmon_monitor_name(void)
{
  return real_monitor_name();
}

static int
monitor_check(void)
{
  if (real_monitor_name() != NULL)
    return 0;
  set_error_printf("%s gerçek ağ durumu vermez",
                   G_OBJECT_TYPE_NAME(g_network_monitor_get_default()));
  return -ENODEV;
}

int
netmon_check(void)
{
  GError *error = NULL;

  if (!load_interface(&error)) {
    set_error(error != NULL ? error->message : "belge okunamadı");
    g_clear_error(&error);
    return -EINVAL;
  }
  return monitor_check();
}

static void
read_status(gboolean *available, gboolean *metered, guint32 *connectivity)
{
  GNetworkMonitor *monitor = g_network_monitor_get_default();

  *available = g_network_monitor_get_network_available(monitor);
  *metered = g_network_monitor_get_network_metered(monitor);
  *connectivity = (guint32) g_network_monitor_get_connectivity(monitor);
}

static gboolean
emit_changed_idle(gpointer user_data)
{
  State *state = user_data;
  GError *error = NULL;

  state->changed_emit_idle = 0;
  if (state->stopping || state->registration_id == 0 || state->connection == NULL)
    return G_SOURCE_REMOVE;
  if (g_dbus_connection_is_closed(state->connection))
    return G_SOURCE_REMOVE;
  if (!g_dbus_connection_emit_signal(state->connection,
                                     NULL,
                                     NETMON_PATH,
                                     NETMON_IFACE,
                                     "changed",
                                     NULL,
                                     &error)) {
    fprintf(stderr, "changed yayınlanamadı: %s\n", error->message);
    g_clear_error(&error);
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_changed(State *state)
{
  if (state->stopping || state->registration_id == 0 || state->connection == NULL)
    return;
  if (g_dbus_connection_is_closed(state->connection))
    return;
  if (state->changed_emit_idle != 0)
    return;
  state->changed_emit_idle = g_idle_add(emit_changed_idle, state);
}

static void
on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer user_data)
{
  (void) monitor;
  (void) available;
  schedule_changed(user_data);
}

static void
on_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void) object;
  (void) pspec;
  schedule_changed(user_data);
}

static void
watch_monitor(State *state)
{
  GNetworkMonitor *monitor;

  if (state->network_changed_id != 0)
    return;
  monitor = g_network_monitor_get_default();
  state->network_changed_id = g_signal_connect(monitor, "network-changed",
                                               G_CALLBACK(on_network_changed), state);
  state->notify_available_id = g_signal_connect(monitor, "notify::network-available",
                                                G_CALLBACK(on_notify), state);
  state->notify_metered_id = g_signal_connect(monitor, "notify::network-metered",
                                              G_CALLBACK(on_notify), state);
  state->notify_connectivity_id = g_signal_connect(monitor, "notify::connectivity",
                                                   G_CALLBACK(on_notify), state);
}

static void
unwatch_monitor(State *state)
{
  GNetworkMonitor *monitor;

  if (state->network_changed_id == 0 && state->notify_available_id == 0 &&
      state->notify_metered_id == 0 && state->notify_connectivity_id == 0)
    return;
  monitor = g_network_monitor_get_default();
  if (state->network_changed_id != 0)
    g_signal_handler_disconnect(monitor, state->network_changed_id);
  if (state->notify_available_id != 0)
    g_signal_handler_disconnect(monitor, state->notify_available_id);
  if (state->notify_metered_id != 0)
    g_signal_handler_disconnect(monitor, state->notify_metered_id);
  if (state->notify_connectivity_id != 0)
    g_signal_handler_disconnect(monitor, state->notify_connectivity_id);
  state->network_changed_id = 0;
  state->notify_available_id = 0;
  state->notify_metered_id = 0;
  state->notify_connectivity_id = 0;
}

static void
finish_can_reach(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GDBusMethodInvocation *invocation = user_data;
  GError *error = NULL;
  gboolean reachable;

  reachable = g_network_monitor_can_reach_finish(G_NETWORK_MONITOR(source), result, &error);
  if (error != NULL)
    g_dbus_method_invocation_return_gerror(invocation, error);
  else
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", reachable));
  g_clear_error(&error);
}

static void
handle_can_reach(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const char *hostname = NULL;
  guint32 port = 0;
  GSocketConnectable *address;

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(su)"))) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                          G_DBUS_ERROR_INVALID_ARGS, "CanReach imzası");
    return;
  }
  g_variant_get(parameters, "(&su)", &hostname, &port);
  if (hostname == NULL || hostname[0] == '\0') {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                          G_DBUS_ERROR_INVALID_ARGS, "hostname boş");
    return;
  }
  if (port > G_MAXUINT16) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                          G_DBUS_ERROR_INVALID_ARGS, "kapı aralığı dışında");
    return;
  }
  address = g_network_address_new(hostname, (guint16) port);
  g_network_monitor_can_reach_async(g_network_monitor_get_default(),
                                    address,
                                    NULL,
                                    finish_can_reach,
                                    invocation);
  g_object_unref(address);
}

static void
handle_get_status(GDBusMethodInvocation *invocation)
{
  gboolean available = FALSE;
  gboolean metered = FALSE;
  guint32 connectivity = 0;
  GVariantBuilder builder;

  read_status(&available, &metered, &connectivity);
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{sv}", "available", g_variant_new_boolean(available));
  g_variant_builder_add(&builder, "{sv}", "metered", g_variant_new_boolean(metered));
  g_variant_builder_add(&builder, "{sv}", "connectivity", g_variant_new_uint32(connectivity));
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &builder));
}

static void
on_method_call(GDBusConnection *connection,
               const char *sender,
               const char *object_path,
               const char *interface_name,
               const char *method_name,
               GVariant *parameters,
               GDBusMethodInvocation *invocation,
               gpointer user_data)
{
  gboolean available = FALSE;
  gboolean metered = FALSE;
  guint32 connectivity = 0;

  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) user_data;

  if (g_strcmp0(method_name, "GetAvailable") == 0) {
    read_status(&available, &metered, &connectivity);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", available));
    return;
  }
  if (g_strcmp0(method_name, "GetMetered") == 0) {
    read_status(&available, &metered, &connectivity);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", metered));
    return;
  }
  if (g_strcmp0(method_name, "GetConnectivity") == 0) {
    read_status(&available, &metered, &connectivity);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", connectivity));
    return;
  }
  if (g_strcmp0(method_name, "GetStatus") == 0) {
    handle_get_status(invocation);
    return;
  }
  if (g_strcmp0(method_name, "CanReach") == 0) {
    handle_can_reach(invocation, parameters);
    return;
  }
  g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                        G_DBUS_ERROR_UNKNOWN_METHOD, "metot yok");
}

static GVariant *
on_get_property(GDBusConnection *connection,
                const char *sender,
                const char *object_path,
                const char *interface_name,
                const char *property_name,
                GError **error,
                gpointer user_data)
{
  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) user_data;

  if (g_strcmp0(property_name, "version") != 0) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "bilinmeyen özellik");
    return NULL;
  }
  return g_variant_new_uint32(NETMON_VERSION);
}

static gboolean
on_set_property(GDBusConnection *connection,
                const char *sender,
                const char *object_path,
                const char *interface_name,
                const char *property_name,
                GVariant *value,
                GError **error,
                gpointer user_data)
{
  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) property_name;
  (void) value;
  (void) user_data;

  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "version salt okunur");
  return FALSE;
}

static const GDBusInterfaceVTable k_vtable = {
  on_method_call,
  on_get_property,
  on_set_property,
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
};

void
netmon_unregister(void)
{
  State *state = &g_state;

  state->stopping = TRUE;
  if (state->changed_emit_idle != 0) {
    g_source_remove(state->changed_emit_idle);
    state->changed_emit_idle = 0;
  }
  unwatch_monitor(state);
  if (state->registration_id != 0 && state->connection != NULL &&
      !g_dbus_connection_is_closed(state->connection))
    g_dbus_connection_unregister_object(state->connection, state->registration_id);
  state->registration_id = 0;
  if (state->connection != NULL) {
    g_object_unref(state->connection);
    state->connection = NULL;
  }
  if (state->node != NULL) {
    g_dbus_node_info_unref(state->node);
    state->node = NULL;
  }
  state->document_ok = FALSE;
  state->stopping = FALSE;
}

int
netmon_register(GDBusConnection *connection)
{
  GError *error = NULL;
  GDBusInterfaceInfo *iface;
  int rc;

  if (g_state.registration_id != 0) {
    set_error("arayüz zaten kayıtlı");
    return -EBUSY;
  }
  if (connection == NULL) {
    set_error("bağlantı yok");
    return -EINVAL;
  }
  if (g_dbus_connection_is_closed(connection)) {
    set_error("bağlantı kapalı");
    return -ENOTCONN;
  }

  rc = netmon_check();
  if (rc != 0)
    return rc;

  iface = g_dbus_node_info_lookup_interface(g_state.node, NETMON_IFACE);
  if (iface == NULL) {
    set_error("arayüz yok");
    return -EINVAL;
  }

  g_state.connection = g_object_ref(connection);
  g_state.stopping = FALSE;
  g_state.registration_id = g_dbus_connection_register_object(g_state.connection,
                                                             NETMON_PATH,
                                                             iface,
                                                             &k_vtable,
                                                             &g_state,
                                                             NULL,
                                                             &error);
  if (g_state.registration_id == 0) {
    set_error(error != NULL ? error->message : "nesne yayınlanamadı");
    g_clear_error(&error);
    g_object_unref(g_state.connection);
    g_state.connection = NULL;
    return -EIO;
  }

  watch_monitor(&g_state);
  g_clear_pointer(&g_error_text, g_free);
  return 0;
}
