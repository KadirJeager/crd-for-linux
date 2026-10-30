#include "portal.h"

#include "capture.h"
#include "clipboard.h"
#include "eis_server.h"

#include <stdint.h>
#include "netmon.h"
#include "olay.h"
#include "olay_kanit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

/*
 * Arayüz sürümleri, 29 Ekim 2026 tarihli xdg-desktop-portal belgesinden:
 *   RemoteDesktop 2
 *     https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html
 *   ScreenCast 6
 *     https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html
 *   Clipboard 1
 *     https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Clipboard.html
 * Session belgesi version özelliğini listeler; daha yüksek bir revizyon yazmaz.
 *
 * ScreenCast 6, pipewire-serial anahtarını varsa diye tanımlar. Yakalama
 * API'si seri numarası vermez; anahtar uydurulmaz.
 *
 * OpenPipeWireRemote, capture_open_pipewire_remote() ile oturumun PipeWire
 * soketine bağlı SOCK_STREAM ucunu döndürür. Uç kopyalanıp istemciye verilir,
 * yereldeki kopya kapatılır. socketpair kullanılmaz.
 * RemoteDesktop.Start ve ScreenCast.Start akışı capture_stream_info() ile
 * dolar. Akış yoksa Response 0 dönülmez. ScreenCast.CreateSession ayrı bir
 * oturum açar; RemoteDesktop oturumuyla paylaşılmaz. İkisi de aynı yakalama
 * akışını bildirebilir.
 * ConnectToEIS yalnız RemoteDesktop oturumunda olay_open_client_fd() ile
 * yeni bir istemci soketi verir. İlk soket yeniden kullanılmaz.
 * EIS sunucusunu olay_init bir kez açar; burada eis_server_init çağrılmaz.
 * Canlı kapı kapalı kalır: fare, tuş ve tekerlek kompozitöre gitmez.
 * Pano, clipboard_set / clipboard_get ile Wayland panosuna gider.
 * org.freedesktop.host.portal.Registry dışa açılmaz.
 *
 * NetworkMonitor 3 libcrd_netmon ile aynı açık bağlantıya kaydolur.
 * netmon_register org.freedesktop.portal.Desktop adını almaz; adı bu
 * süreç alır. Kütüphane available, metered ve connectivity değerlerini
 * kendi ağ kaynağından okur. Bu dosya ikinci bir GNetworkMonitor tutmaz
 * ve sabit çevrimiçi üretmez. Kapanışta netmon_unregister çağrılır.
 */

#define CRD_BUS_NAME "org.freedesktop.portal.Desktop"
#define CRD_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define CRD_IFACE_REMOTE "org.freedesktop.portal.RemoteDesktop"
#define CRD_IFACE_SCREEN "org.freedesktop.portal.ScreenCast"
#define CRD_IFACE_CLIPBOARD "org.freedesktop.portal.Clipboard"
#define CRD_IFACE_REQUEST "org.freedesktop.portal.Request"
#define CRD_IFACE_SESSION "org.freedesktop.portal.Session"

#define CRD_REMOTE_DESKTOP_VERSION 2u
#define CRD_SCREEN_CAST_VERSION 6u
#define CRD_CLIPBOARD_VERSION 1u
#define CRD_SESSION_VERSION 1u
#define CRD_DEVICE_TYPES 3u
#define CRD_SOURCE_MONITOR 1u
#define CRD_SOURCE_VIRTUAL 4u
#define CRD_CURSOR_METADATA 4u
#define CRD_CLIP_MAX_BYTES (16u * 1024u * 1024u)

static const gchar k_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.portal.RemoteDesktop'>"
    "    <property name='version' type='u' access='read'/>"
    "    <property name='AvailableDeviceTypes' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='handle' direction='out'/>"
    "    </method>"
    "    <method name='SelectDevices'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='handle' direction='out'/>"
    "    </method>"
    "    <method name='Start'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='handle' direction='out'/>"
    "    </method>"
    "    <method name='ConnectToEIS'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.portal.ScreenCast'>"
    "    <property name='version' type='u' access='read'/>"
    "    <property name='AvailableSourceTypes' type='u' access='read'/>"
    "    <property name='AvailableCursorModes' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='handle' direction='out'/>"
    "    </method>"
    "    <method name='SelectSources'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='handle' direction='out'/>"
    "    </method>"
    "    <method name='Start'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='handle' direction='out'/>"
    "    </method>"
    "    <method name='OpenPipeWireRemote'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.portal.Clipboard'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='RequestClipboard'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <method name='SetSelection'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <method name='SelectionWrite'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='u' name='serial' direction='in'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "    <method name='SelectionWriteDone'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='u' name='serial' direction='in'/>"
    "      <arg type='b' name='success' direction='in'/>"
    "    </method>"
    "    <method name='SelectionRead'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='mime_type' direction='in'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "    <signal name='SelectionOwnerChanged'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='a{sv}' name='options'/>"
    "    </signal>"
    "    <signal name='SelectionTransfer'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='s' name='mime_type'/>"
    "      <arg type='u' name='serial'/>"
    "    </signal>"
    "  </interface>"
    "  <interface name='org.freedesktop.portal.Request'>"
    "    <method name='Close'/>"
    "    <signal name='Response'>"
    "      <arg type='u' name='response'/>"
    "      <arg type='a{sv}' name='results'/>"
    "    </signal>"
    "  </interface>"
    "  <interface name='org.freedesktop.portal.Session'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='Close'/>"
    "    <signal name='Closed'>"
    "      <arg type='a{sv}' name='details'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

typedef struct CrdServer CrdServer;
typedef struct CrdSession CrdSession;
typedef struct CrdRequest CrdRequest;

struct CrdServer {
  GDBusConnection *connection;
  GMainLoop *loop;
  GDBusNodeInfo *node;
  GHashTable *sessions;
  GHashTable *requests;
  guint name_owner;
  guint name_watch;
  guint rd_reg;
  guint sc_reg;
  guint cb_reg;
  guint clip_poll;
  int exit_code;
  gboolean name_acquired;
  gboolean clipboard_ready;
};

typedef struct CrdXfer CrdXfer;

struct CrdXfer {
  guint serial;
  char *mime;
  int read_fd;
  guint watch;
  unsigned char *data;
  size_t len;
  gboolean armed;
  gboolean eof;
  gboolean finished;
};

struct CrdSession {
  CrdServer *server;
  char *sender;
  char *path;
  guint registration;
  guint close_idle;
  guint next_serial;
  gboolean screencast;
  gboolean devices_selected;
  gboolean clipboard_requested;
  gboolean sources_selected;
  gboolean started;
  gboolean eis_open;
  gboolean we_own;
  gboolean own_confirmed;
  gboolean owner_notified;
  uint64_t seen_clip_serial;
  unsigned char *owned;
  size_t owned_len;
  GPtrArray *xfers;
};

struct CrdRequest {
  CrdServer *server;
  char *path;
  guint registration;
  guint idle_id;
  GVariant *body;
};

typedef struct {
  CrdServer *server;
  char *path;
} CrdDrop;

static const GDBusInterfaceVTable request_vtable;
static const GDBusInterfaceVTable session_vtable;
static const GDBusInterfaceVTable desktop_vtable;

static void session_close_later(CrdSession *session);
static void xfer_free(gpointer data);

static char *
address_unix_path(const char *address)
{
  const char *marker;
  const char *end;

  if (address == NULL)
    return NULL;
  marker = strstr(address, "unix:path=");
  if (marker == NULL)
    return NULL;
  marker += strlen("unix:path=");
  end = strchr(marker, ',');
  if (end == NULL)
    return g_strdup(marker);
  return g_strndup(marker, (gsize) (end - marker));
}

static gboolean
address_is_real_session_bus(const char *address)
{
  const char *env;
  char *given = NULL;
  char *session_path = NULL;
  char *runtime_bus = NULL;
  gboolean match = FALSE;

  if (address == NULL || address[0] == '\0')
    return TRUE;

  env = g_getenv("DBUS_SESSION_BUS_ADDRESS");
  if (env != NULL && g_strcmp0(env, address) == 0)
    return TRUE;

  given = address_unix_path(address);
  session_path = address_unix_path(env);
  runtime_bus = g_build_filename(g_get_user_runtime_dir(), "bus", NULL);
  if (given != NULL && session_path != NULL && g_strcmp0(given, session_path) == 0)
    match = TRUE;
  if (given != NULL && g_strcmp0(given, runtime_bus) == 0)
    match = TRUE;

  g_free(given);
  g_free(session_path);
  g_free(runtime_bus);
  return match;
}

static gboolean
valid_token(const char *token)
{
  const char *cursor;

  if (token == NULL || token[0] == '\0')
    return FALSE;
  for (cursor = token; *cursor != '\0'; cursor++) {
    if (!g_ascii_isalnum(*cursor) && *cursor != '_')
      return FALSE;
  }
  return TRUE;
}

static char *
make_handle(const char *kind, const char *sender, const char *token)
{
  char *element;
  char *path;
  char *cursor;

  if (sender == NULL || sender[0] != ':' || !valid_token(token))
    return NULL;

  element = g_strdup(sender + 1);
  for (cursor = element; *cursor != '\0'; cursor++) {
    if (*cursor == '.')
      *cursor = '_';
  }

  path = g_strdup_printf("%s/%s/%s/%s", CRD_OBJECT_PATH, kind, element, token);
  g_free(element);
  if (!g_variant_is_object_path(path)) {
    g_free(path);
    return NULL;
  }
  return path;
}

static void
discard_variant(GVariant *variant)
{
  if (variant != NULL)
    g_variant_unref(g_variant_ref_sink(variant));
}

static void
fail_call(GDBusMethodInvocation *invocation, gint code, const char *message)
{
  g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, code, "%s", message);
}

static CrdSession *
session_lookup(CrdServer *server, const char *path, const char *sender)
{
  CrdSession *session;

  session = g_hash_table_lookup(server->sessions, path);
  if (session == NULL || g_strcmp0(session->sender, sender) != 0)
    return NULL;
  return session;
}

static void
session_free(gpointer data)
{
  CrdSession *session = data;

  if (session->close_idle != 0)
    g_source_remove(session->close_idle);
  if (session->registration != 0 && session->server->connection != NULL) {
    g_dbus_connection_unregister_object(session->server->connection, session->registration);
  }
  if (session->xfers != NULL)
    g_ptr_array_unref(session->xfers);
  free(session->owned);
  g_free(session->sender);
  g_free(session->path);
  g_free(session);
}

static void
request_free(gpointer data)
{
  CrdRequest *request = data;

  if (request->idle_id != 0)
    g_source_remove(request->idle_id);
  if (request->registration != 0 && request->server->connection != NULL) {
    g_dbus_connection_unregister_object(request->server->connection, request->registration);
  }
  if (request->body != NULL)
    g_variant_unref(request->body);
  g_free(request->path);
  g_free(request);
}

static gboolean
emit_response_idle(gpointer data)
{
  CrdRequest *request = data;
  GError *error = NULL;

  request->idle_id = 0;
  if (request->server->connection == NULL || request->body == NULL)
    return G_SOURCE_REMOVE;

  /* body gömülü. emit_signal yüzen referansı tüketir; gömülü referans kalır. */
  if (!g_dbus_connection_emit_signal(request->server->connection,
                                     NULL,
                                     request->path,
                                     CRD_IFACE_REQUEST,
                                     "Response",
                                     request->body,
                                     &error)) {
    fprintf(stderr, "Response gönderilemedi: %s\n", error->message);
    g_error_free(error);
  }
  return G_SOURCE_REMOVE;
}

static gboolean
drop_request_idle(gpointer data)
{
  CrdDrop *drop = data;

  g_hash_table_remove(drop->server->requests, drop->path);
  g_free(drop->path);
  g_free(drop);
  return G_SOURCE_REMOVE;
}

static gboolean
session_closed_idle(gpointer data)
{
  CrdSession *session = data;
  CrdServer *server = session->server;
  GVariantBuilder details;
  GError *error = NULL;
  char *path;

  session->close_idle = 0;
  g_variant_builder_init(&details, G_VARIANT_TYPE("a{sv}"));
  if (server->connection != NULL) {
    if (!g_dbus_connection_emit_signal(server->connection,
                                       NULL,
                                       session->path,
                                       CRD_IFACE_SESSION,
                                       "Closed",
                                       g_variant_new("(@a{sv})", g_variant_builder_end(&details)),
                                       &error)) {
      fprintf(stderr, "Closed gönderilemedi: %s\n", error->message);
      g_clear_error(&error);
    }
  } else {
    g_variant_builder_clear(&details);
  }

  path = g_strdup(session->path);
  g_hash_table_remove(server->sessions, path);
  g_free(path);
  return G_SOURCE_REMOVE;
}

static void
session_close_later(CrdSession *session)
{
  if (session->close_idle == 0)
    session->close_idle = g_idle_add(session_closed_idle, session);
}

static void
request_method(GDBusConnection *connection G_GNUC_UNUSED,
               const gchar *sender G_GNUC_UNUSED,
               const gchar *object_path G_GNUC_UNUSED,
               const gchar *interface_name G_GNUC_UNUSED,
               const gchar *method_name,
               GVariant *parameters G_GNUC_UNUSED,
               GDBusMethodInvocation *invocation,
               gpointer user_data)
{
  CrdRequest *request = user_data;
  CrdDrop *drop;

  if (g_strcmp0(method_name, "Close") != 0) {
    fail_call(invocation, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown request method");
    return;
  }

  if (request->idle_id != 0) {
    g_source_remove(request->idle_id);
    request->idle_id = 0;
  }
  g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
  drop = g_new0(CrdDrop, 1);
  drop->server = request->server;
  drop->path = g_strdup(request->path);
  g_idle_add(drop_request_idle, drop);
}

static GVariant *
session_get_property(GDBusConnection *connection G_GNUC_UNUSED,
                     const gchar *sender G_GNUC_UNUSED,
                     const gchar *object_path G_GNUC_UNUSED,
                     const gchar *interface_name G_GNUC_UNUSED,
                     const gchar *property_name,
                     GError **error,
                     gpointer user_data G_GNUC_UNUSED)
{
  if (g_strcmp0(property_name, "version") == 0)
    return g_variant_new_uint32(CRD_SESSION_VERSION);
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown session property");
  return NULL;
}

static void
session_method(GDBusConnection *connection G_GNUC_UNUSED,
               const gchar *sender G_GNUC_UNUSED,
               const gchar *object_path G_GNUC_UNUSED,
               const gchar *interface_name G_GNUC_UNUSED,
               const gchar *method_name,
               GVariant *parameters G_GNUC_UNUSED,
               GDBusMethodInvocation *invocation,
               gpointer user_data)
{
  CrdSession *session = user_data;

  if (g_strcmp0(method_name, "Close") != 0) {
    fail_call(invocation, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown session method");
    return;
  }
  session_close_later(session);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
}

static const GDBusInterfaceVTable request_vtable = {
  request_method,
  NULL,
  NULL,
  { 0 }
};

static const GDBusInterfaceVTable session_vtable = {
  session_method,
  session_get_property,
  NULL,
  { 0 }
};

static gboolean
request_open(CrdServer *server, const char *path, GVariant *body, GError **error)
{
  CrdRequest *request;
  GDBusInterfaceInfo *iface;

  if (g_hash_table_contains(server->requests, path)) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "request already exists");
    discard_variant(body);
    return FALSE;
  }

  request = g_new0(CrdRequest, 1);
  request->server = server;
  request->path = g_strdup(path);
  request->body = g_variant_ref_sink(body);
  iface = g_dbus_node_info_lookup_interface(server->node, CRD_IFACE_REQUEST);
  request->registration = g_dbus_connection_register_object(server->connection,
                                                            path,
                                                            iface,
                                                            &request_vtable,
                                                            request,
                                                            NULL,
                                                            error);
  if (request->registration == 0) {
    request_free(request);
    return FALSE;
  }

  g_hash_table_insert(server->requests, g_strdup(path), request);
  /* Metot dönüşünden sonra Response. Host çağrıdan önce dinler. */
  request->idle_id = g_idle_add(emit_response_idle, request);
  return TRUE;
}

static gboolean
session_open(CrdServer *server, const char *sender, const char *path, gboolean screencast, GError **error)
{
  CrdSession *session;
  GDBusInterfaceInfo *iface;

  if (g_hash_table_contains(server->sessions, path)) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "session already exists");
    return FALSE;
  }

  session = g_new0(CrdSession, 1);
  session->server = server;
  session->sender = g_strdup(sender);
  session->path = g_strdup(path);
  session->screencast = screencast;
  session->next_serial = 1;
  session->xfers = g_ptr_array_new_with_free_func(xfer_free);
  iface = g_dbus_node_info_lookup_interface(server->node, CRD_IFACE_SESSION);
  session->registration = g_dbus_connection_register_object(server->connection,
                                                            path,
                                                            iface,
                                                            &session_vtable,
                                                            session,
                                                            NULL,
                                                            error);
  if (session->registration == 0) {
    session_free(session);
    return FALSE;
  }
  g_hash_table_insert(server->sessions, g_strdup(path), session);
  return TRUE;
}

static gboolean
schedule_request(CrdServer *server,
                 GDBusMethodInvocation *invocation,
                 const char *sender,
                 GVariant *options,
                 GVariant *results)
{
  const char *token = NULL;
  char *path;
  GError *error = NULL;
  GVariant *body;

  if (!g_variant_lookup(options, "handle_token", "&s", &token) || !valid_token(token)) {
    fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "handle_token is required");
    discard_variant(results);
    return FALSE;
  }

  path = make_handle("request", sender, token);
  if (path == NULL) {
    fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "request path is invalid");
    discard_variant(results);
    return FALSE;
  }

  body = g_variant_new("(u@a{sv})", 0u, results);
  if (!request_open(server, path, body, &error)) {
    g_dbus_method_invocation_return_gerror(invocation, error);
    g_error_free(error);
    g_free(path);
    return FALSE;
  }

  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path));
  g_free(path);
  return TRUE;
}

static gboolean
return_fd(GDBusMethodInvocation *invocation, int fd, gboolean close_fd)
{
  GUnixFDList *list;
  GError *error = NULL;
  gint index;

  list = g_unix_fd_list_new();
  index = g_unix_fd_list_append(list, fd, &error);
  if (close_fd)
    close(fd);
  if (index < 0) {
    g_dbus_method_invocation_return_gerror(invocation, error);
    g_error_free(error);
    g_object_unref(list);
    return FALSE;
  }

  g_dbus_method_invocation_return_value_with_unix_fd_list(invocation,
                                                          g_variant_new("(h)", index),
                                                          list);
  g_object_unref(list);
  return TRUE;
}

static gboolean
return_eis_fd(GDBusMethodInvocation *invocation)
{
  int fd = olay_open_client_fd();

  if (fd < 0) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "olay client fd is not ready");
    return FALSE;
  }
  /* Her ConnectToEIS yeni soket alır. D-Bus kopyalar, bu uç kapanır. */
  return return_fd(invocation, fd, TRUE);
}

static void
xfer_free(gpointer data)
{
  CrdXfer *xfer = data;

  if (xfer == NULL)
    return;
  if (xfer->watch != 0)
    g_source_remove(xfer->watch);
  if (xfer->read_fd >= 0)
    close(xfer->read_fd);
  free(xfer->data);
  g_free(xfer->mime);
  g_free(xfer);
}

static CrdXfer *
xfer_lookup(CrdSession *session, guint serial)
{
  guint index;

  for (index = 0; index < session->xfers->len; index++) {
    CrdXfer *xfer = g_ptr_array_index(session->xfers, index);
    if (xfer->serial == serial)
      return xfer;
  }
  return NULL;
}

static int
xfer_append(CrdXfer *xfer, const unsigned char *bytes, size_t n)
{
  unsigned char *next;

  if (n == 0)
    return 0;
  if (xfer->len > CRD_CLIP_MAX_BYTES || n > CRD_CLIP_MAX_BYTES - xfer->len)
    return -1;
  next = realloc(xfer->data, xfer->len + n);
  if (next == NULL)
    return -1;
  memcpy(next + xfer->len, bytes, n);
  xfer->data = next;
  xfer->len += n;
  return 0;
}

static int
xfer_drain(CrdXfer *xfer)
{
  unsigned char tmp[8192];

  if (xfer->read_fd < 0)
    return 0;

  for (;;) {
    ssize_t n = read(xfer->read_fd, tmp, sizeof tmp);

    if (n > 0) {
      if (xfer_append(xfer, tmp, (size_t) n) != 0)
        return -1;
      continue;
    }
    if (n == 0) {
      xfer->eof = TRUE;
      return 0;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    return -1;
  }
}

static gboolean
xfer_readable(gint fd, GIOCondition condition, gpointer user_data)
{
  CrdXfer *xfer = user_data;

  (void) fd;
  if (xfer_drain(xfer) != 0)
    xfer->eof = TRUE;
  if ((condition & (G_IO_HUP | G_IO_ERR)) != 0)
    xfer->eof = TRUE;
  if (xfer->eof && xfer->watch != 0) {
    xfer->watch = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
clipboard_session_ready(CrdSession *session)
{
  return session != NULL && session->started && session->clipboard_requested;
}

static void
emit_selection_transfer(CrdSession *session, const char *mime, guint serial)
{
  GError *error = NULL;

  if (!g_dbus_connection_emit_signal(session->server->connection,
                                     NULL,
                                     CRD_OBJECT_PATH,
                                     CRD_IFACE_CLIPBOARD,
                                     "SelectionTransfer",
                                     g_variant_new("(osu)", session->path, mime, serial),
                                     &error)) {
    fprintf(stderr, "SelectionTransfer gönderilemedi: %s\n", error->message);
    g_error_free(error);
  }
}

static void
emit_owner_changed(CrdSession *session, gboolean has_text)
{
  GVariantBuilder options;
  GVariantBuilder mimes;
  GError *error = NULL;

  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_init(&mimes, G_VARIANT_TYPE("as"));
  if (has_text)
    g_variant_builder_add(&mimes, "s", "text/plain");
  g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_builder_end(&mimes));
  g_variant_builder_add(&options, "{sv}", "session_is_owner", g_variant_new_boolean(FALSE));

  if (!g_dbus_connection_emit_signal(session->server->connection,
                                     NULL,
                                     CRD_OBJECT_PATH,
                                     CRD_IFACE_CLIPBOARD,
                                     "SelectionOwnerChanged",
                                     g_variant_new("(o@a{sv})", session->path, g_variant_builder_end(&options)),
                                     &error)) {
    fprintf(stderr, "SelectionOwnerChanged gönderilemedi: %s\n", error->message);
    g_error_free(error);
  }
}

static gboolean
bytes_eq(const void *left, size_t left_len, const void *right, size_t right_len)
{
  if (left_len != right_len)
    return FALSE;
  if (left_len == 0)
    return TRUE;
  return memcmp(left, right, left_len) == 0;
}

static int
read_text_plain(unsigned char **out, size_t *out_len)
{
  void *first = NULL;
  void *second = NULL;
  size_t first_len = 0;
  size_t second_len = 0;
  int first_rc;
  int second_rc;

  *out = NULL;
  *out_len = 0;
  first_rc = clipboard_get("text/plain", &first, &first_len);
  second_rc = clipboard_get("text/plain", &second, &second_len);
  if (second_rc == 0) {
    free(first);
    *out = second;
    *out_len = second_len;
    return 0;
  }
  free(second);
  if (first_rc == 0) {
    *out = first;
    *out_len = first_len;
    return 0;
  }
  free(first);
  return -1;
}

static gboolean
clipboard_poll_cb(gpointer user_data)
{
  CrdServer *server = user_data;
  GHashTableIter iter;
  gpointer value;

  if (!server->clipboard_ready || server->sessions == NULL)
    return G_SOURCE_CONTINUE;

  clipboard_pump();

  g_hash_table_iter_init(&iter, server->sessions);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CrdSession *session = value;
    unsigned char *current = NULL;
    size_t current_len = 0;
    uint64_t serial = clipboard_selection_serial();
    int rc;

    if (!session->we_own) {
      if (session->seen_clip_serial != serial) {
        session->seen_clip_serial = serial;
        emit_owner_changed(session, clipboard_has("text/plain") == 1);
      }
      continue;
    }
    if (session->owner_notified)
      continue;

    rc = read_text_plain(&current, &current_len);
    if (!session->own_confirmed) {
      if (rc == 0 && bytes_eq(current, current_len, session->owned, session->owned_len))
        session->own_confirmed = TRUE;
      free(current);
      continue;
    }
    if (rc == 0 && bytes_eq(current, current_len, session->owned, session->owned_len)) {
      free(current);
      continue;
    }
    session->we_own = FALSE;
    session->owner_notified = TRUE;
    session->seen_clip_serial = serial;
    emit_owner_changed(session, rc == 0);
    free(current);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
remember_owned_text(CrdSession *session, const unsigned char *data, size_t len)
{
  unsigned char *copy = NULL;

  if (len > 0) {
    copy = malloc(len);
    if (copy == NULL)
      return FALSE;
    memcpy(copy, data, len);
  }
  free(session->owned);
  session->owned = copy;
  session->owned_len = len;
  session->we_own = TRUE;
  session->own_confirmed = FALSE;
  session->owner_notified = FALSE;
  return TRUE;
}

static gboolean
handle_set_selection(CrdServer *server,
                     const char *sender,
                     GVariant *parameters,
                     GDBusMethodInvocation *invocation)
{
  const char *session_handle = NULL;
  GVariant *options = NULL;
  GVariant *mime_list = NULL;
  GVariantIter iter;
  CrdSession *session;
  const char *mime = NULL;
  guint emitted = 0;

  g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);
  session = session_lookup(server, session_handle, sender);
  if (!clipboard_session_ready(session)) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "SetSelection is not valid for this session");
    g_variant_unref(options);
    return TRUE;
  }

  mime_list = g_variant_lookup_value(options, "mime_types", G_VARIANT_TYPE("as"));
  if (mime_list == NULL || g_variant_n_children(mime_list) == 0) {
    fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "mime_types is required");
    if (mime_list != NULL)
      g_variant_unref(mime_list);
    g_variant_unref(options);
    return TRUE;
  }

  g_variant_iter_init(&iter, mime_list);
  while (g_variant_iter_next(&iter, "&s", &mime)) {
    CrdXfer *xfer;

    if (mime == NULL || mime[0] == '\0') {
      fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "mime type is empty");
      g_variant_unref(mime_list);
      g_variant_unref(options);
      return TRUE;
    }
    xfer = g_new0(CrdXfer, 1);
    xfer->serial = session->next_serial++;
    xfer->mime = g_strdup(mime);
    xfer->read_fd = -1;
    g_ptr_array_add(session->xfers, xfer);
    emitted++;
  }
  g_variant_unref(mime_list);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));

  for (guint index = session->xfers->len - emitted; index < session->xfers->len; index++) {
    CrdXfer *xfer = g_ptr_array_index(session->xfers, index);
    emit_selection_transfer(session, xfer->mime, xfer->serial);
  }
  g_variant_unref(options);
  return TRUE;
}

static gboolean
handle_selection_write(CrdServer *server,
                       const char *sender,
                       GVariant *parameters,
                       GDBusMethodInvocation *invocation)
{
  const char *session_handle = NULL;
  guint serial = 0;
  CrdSession *session;
  CrdXfer *xfer;
  int ends[2] = {-1, -1};
  int flags;

  g_variant_get(parameters, "(&ou)", &session_handle, &serial);
  session = session_lookup(server, session_handle, sender);
  xfer = session != NULL ? xfer_lookup(session, serial) : NULL;
  if (!clipboard_session_ready(session) || xfer == NULL || xfer->armed || xfer->finished) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "SelectionWrite is not valid for this serial");
    return TRUE;
  }
  if (pipe2(ends, O_CLOEXEC) != 0) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, g_strerror(errno));
    return TRUE;
  }
  flags = fcntl(ends[0], F_GETFL, 0);
  if (flags < 0 || fcntl(ends[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(ends[0]);
    close(ends[1]);
    fail_call(invocation, G_DBUS_ERROR_FAILED, "selection pipe could not be prepared");
    return TRUE;
  }
  xfer->read_fd = ends[0];
  xfer->watch = g_unix_fd_add(ends[0], G_IO_IN | G_IO_HUP | G_IO_ERR, xfer_readable, xfer);
  if (xfer->watch == 0) {
    close(ends[0]);
    close(ends[1]);
    xfer->read_fd = -1;
    fail_call(invocation, G_DBUS_ERROR_FAILED, "selection pipe watch failed");
    return TRUE;
  }
  xfer->armed = TRUE;
  if (!return_fd(invocation, ends[1], TRUE)) {
    xfer->finished = TRUE;
    return TRUE;
  }
  return TRUE;
}

static gboolean
handle_selection_write_done(CrdServer *server,
                            const char *sender,
                            GVariant *parameters,
                            GDBusMethodInvocation *invocation)
{
  const char *session_handle = NULL;
  guint serial = 0;
  gboolean success = FALSE;
  CrdSession *session;
  CrdXfer *xfer;
  int rc;

  g_variant_get(parameters, "(&oub)", &session_handle, &serial, &success);
  session = session_lookup(server, session_handle, sender);
  xfer = session != NULL ? xfer_lookup(session, serial) : NULL;
  if (!clipboard_session_ready(session) || xfer == NULL || !xfer->armed || xfer->finished) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "SelectionWriteDone is not valid for this serial");
    return TRUE;
  }

  if (xfer->watch != 0) {
    g_source_remove(xfer->watch);
    xfer->watch = 0;
  }
  if (xfer_drain(xfer) != 0) {
    xfer->finished = TRUE;
    fail_call(invocation, G_DBUS_ERROR_FAILED, "selection bytes could not be read");
    return TRUE;
  }
  xfer->finished = TRUE;
  if (!success) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
    return TRUE;
  }

  rc = clipboard_set(xfer->mime, xfer->len > 0 ? xfer->data : NULL, xfer->len);
  if (rc != 0) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "clipboard_set failed");
    return TRUE;
  }
  if (g_strcmp0(xfer->mime, "text/plain") == 0 &&
      !remember_owned_text(session, xfer->data, xfer->len)) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "owned clipboard copy failed");
    return TRUE;
  }
  g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
  return TRUE;
}

static gboolean
handle_selection_read(CrdServer *server,
                      const char *sender,
                      GVariant *parameters,
                      GDBusMethodInvocation *invocation)
{
  const char *session_handle = NULL;
  const char *mime = NULL;
  CrdSession *session;
  void *data = NULL;
  size_t len = 0;
  int ends[2] = {-1, -1};
  size_t off = 0;

  g_variant_get(parameters, "(&o&s)", &session_handle, &mime);
  session = session_lookup(server, session_handle, sender);
  if (!clipboard_session_ready(session) || mime == NULL || mime[0] == '\0') {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "SelectionRead is not valid for this session");
    return TRUE;
  }
  if (clipboard_get(mime, &data, &len) != 0) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "clipboard_get failed");
    return TRUE;
  }
  if (pipe2(ends, O_CLOEXEC) != 0) {
    free(data);
    fail_call(invocation, G_DBUS_ERROR_FAILED, g_strerror(errno));
    return TRUE;
  }
  while (off < len) {
    ssize_t wrote = write(ends[1], (const unsigned char *) data + off, len - off);
    if (wrote < 0) {
      if (errno == EINTR)
        continue;
      close(ends[0]);
      close(ends[1]);
      free(data);
      fail_call(invocation, G_DBUS_ERROR_FAILED, "selection read pipe write failed");
      return TRUE;
    }
    off += (size_t) wrote;
  }
  free(data);
  close(ends[1]);
  if (!return_fd(invocation, ends[0], TRUE))
    return TRUE;
  return TRUE;
}

static GVariant *
empty_results(void)
{
  GVariantBuilder builder;

  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end(&builder);
}

static void
log_capture_stream(void)
{
  struct crd_stream_info info;

  if (capture_stream_info(&info) == 0) {
    fprintf(stderr,
            "capture_stream node_id=%u size=%dx%d mapping=%s\n",
            info.node_id,
            info.width,
            info.height,
            info.mapping_id);
  }
}

static guint32 selected_source_type = CRD_SOURCE_MONITOR;

static gboolean
append_capture_stream(GVariantBuilder *streams)
{
  struct crd_stream_info info;
  GVariantBuilder props;
  guint32 reported_type = CRD_SOURCE_MONITOR;

  if (capture_stream_info(&info) != 0)
    return FALSE;

  /* The host asks for a virtual source when it thinks there is no seat.
     The pixels are still the captured output. Report the type it asked for
     so the stream is not dropped, without creating another output. */
  if (selected_source_type == CRD_SOURCE_VIRTUAL)
    reported_type = CRD_SOURCE_VIRTUAL;

  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&props, "{sv}", "mapping_id", g_variant_new_string(info.mapping_id));
  g_variant_builder_add(&props, "{sv}", "position", g_variant_new("(ii)", (int) info.x, (int) info.y));
  g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", (int) info.width, (int) info.height));
  g_variant_builder_add(&props, "{sv}", "source_type", g_variant_new_uint32(reported_type));
  g_variant_builder_add(streams, "(u@a{sv})", info.node_id, g_variant_builder_end(&props));
  return TRUE;
}

static GVariant *
start_results(gboolean clipboard_enabled)
{
  GVariantBuilder streams;
  GVariantBuilder results;

  g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
  if (!append_capture_stream(&streams)) {
    g_variant_builder_clear(&streams);
    return NULL;
  }

  g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&results, "{sv}", "devices", g_variant_new_uint32(CRD_DEVICE_TYPES));
  g_variant_builder_add(&results, "{sv}", "clipboard_enabled", g_variant_new_boolean(clipboard_enabled));
  g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
  return g_variant_builder_end(&results);
}

static GVariant *
screencast_start_results(void)
{
  GVariantBuilder streams;
  GVariantBuilder results;

  g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
  if (!append_capture_stream(&streams)) {
    g_variant_builder_clear(&streams);
    return NULL;
  }

  g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
  return g_variant_builder_end(&results);
}

static void
handle_create_session(CrdServer *server,
                      const gchar *sender,
                      GVariant *parameters,
                      GDBusMethodInvocation *invocation,
                      gboolean screencast)
{
  GVariant *options = g_variant_get_child_value(parameters, 0);
  const char *handle_token = NULL;
  const char *session_token = NULL;
  char *session_path;
  GVariantBuilder results;
  GError *error = NULL;

  g_variant_lookup(options, "handle_token", "&s", &handle_token);
  g_variant_lookup(options, "session_handle_token", "&s", &session_token);
  if (!valid_token(handle_token) || !valid_token(session_token)) {
    fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS,
              "handle_token and session_handle_token are required");
    g_variant_unref(options);
    return;
  }

  session_path = make_handle("session", sender, session_token);
  if (session_path == NULL) {
    fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "session path is invalid");
    g_variant_unref(options);
    return;
  }
  if (!session_open(server, sender, session_path, screencast, &error)) {
    g_dbus_method_invocation_return_gerror(invocation, error);
    g_error_free(error);
    g_free(session_path);
    g_variant_unref(options);
    return;
  }

  g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
  /* Belge: session_handle yanlışlıkla 's' uygulanmış nesne yoludur. */
  g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(session_path));
  if (!schedule_request(server, invocation, sender, options, g_variant_builder_end(&results)))
    g_hash_table_remove(server->sessions, session_path);
  g_free(session_path);
  g_variant_unref(options);
}

static gboolean
finish_start(CrdServer *server,
             GDBusMethodInvocation *invocation,
             const char *sender,
             GVariant *options,
             CrdSession *session,
             GVariant *results)
{
  if (results == NULL) {
    fail_call(invocation, G_DBUS_ERROR_FAILED, "capture stream is not ready");
    return FALSE;
  }
  if (!schedule_request(server, invocation, sender, options, results))
    return FALSE;
  log_capture_stream();
  session->started = TRUE;
  return TRUE;
}

static void
desktop_method(GDBusConnection *connection G_GNUC_UNUSED,
               const gchar *sender,
               const gchar *object_path G_GNUC_UNUSED,
               const gchar *interface_name,
               const gchar *method_name,
               GVariant *parameters,
               GDBusMethodInvocation *invocation,
               gpointer user_data)
{
  CrdServer *server = user_data;

  if (g_strcmp0(method_name, "CreateSession") == 0 &&
      (g_strcmp0(interface_name, CRD_IFACE_REMOTE) == 0 ||
       g_strcmp0(interface_name, CRD_IFACE_SCREEN) == 0)) {
    handle_create_session(server, sender, parameters, invocation,
                          g_strcmp0(interface_name, CRD_IFACE_SCREEN) == 0);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_REMOTE) == 0 &&
      g_strcmp0(method_name, "SelectDevices") == 0) {
    const char *session_handle = NULL;
    GVariant *options = NULL;
    CrdSession *session;
    guint32 types = 0;

    g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || session->screencast || session->started || session->devices_selected) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "SelectDevices is not valid for this session");
      g_variant_unref(options);
      return;
    }
    if (!g_variant_lookup(options, "types", "u", &types) || types != CRD_DEVICE_TYPES) {
      fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "types must be 3");
      g_variant_unref(options);
      return;
    }
    if (!schedule_request(server, invocation, sender, options, empty_results())) {
      g_variant_unref(options);
      return;
    }
    session->devices_selected = TRUE;
    g_variant_unref(options);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_REMOTE) == 0 &&
      g_strcmp0(method_name, "Start") == 0) {
    const char *session_handle = NULL;
    const char *parent_window = NULL;
    GVariant *options = NULL;
    CrdSession *session;

    g_variant_get(parameters, "(&o&s@a{sv})", &session_handle, &parent_window, &options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || session->screencast || session->started ||
        !session->devices_selected || !session->sources_selected) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "Start is not valid for this session");
      g_variant_unref(options);
      return;
    }
    if (parent_window == NULL || parent_window[0] != '\0') {
      fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "parent_window must be empty");
      g_variant_unref(options);
      return;
    }
    if (!finish_start(server, invocation, sender, options, session,
                      start_results(session->clipboard_requested))) {
      g_variant_unref(options);
      return;
    }
    g_variant_unref(options);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_SCREEN) == 0 &&
      g_strcmp0(method_name, "Start") == 0) {
    const char *session_handle = NULL;
    const char *parent_window = NULL;
    GVariant *options = NULL;
    CrdSession *session;

    g_variant_get(parameters, "(&o&s@a{sv})", &session_handle, &parent_window, &options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || !session->screencast || session->started || !session->sources_selected) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "Start is not valid for this session");
      g_variant_unref(options);
      return;
    }
    if (parent_window == NULL) {
      fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS, "parent_window is missing");
      g_variant_unref(options);
      return;
    }
    if (!finish_start(server, invocation, sender, options, session, screencast_start_results())) {
      g_variant_unref(options);
      return;
    }
    g_variant_unref(options);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_REMOTE) == 0 &&
      g_strcmp0(method_name, "ConnectToEIS") == 0) {
    const char *session_handle = NULL;
    GVariant *options = NULL;
    CrdSession *session;

    g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);
    g_variant_unref(options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || session->screencast || !session->started || session->eis_open) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "ConnectToEIS is not valid for this session");
      return;
    }
    if (!return_eis_fd(invocation))
      return;
    session->eis_open = TRUE;
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_SCREEN) == 0 &&
      g_strcmp0(method_name, "SelectSources") == 0) {
    const char *session_handle = NULL;
    GVariant *options = NULL;
    CrdSession *session;
    gboolean multiple = FALSE;
    guint32 types = 0;
    guint32 cursor_mode = 0;

    g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || session->started || session->sources_selected ||
        (!session->screencast && !session->devices_selected)) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "SelectSources is not valid for this session");
      g_variant_unref(options);
      return;
    }
    g_variant_lookup(options, "multiple", "b", &multiple);
    g_variant_lookup(options, "types", "u", &types);
    g_variant_lookup(options, "cursor_mode", "u", &cursor_mode);
    g_printerr("SelectSources multiple=%d types=%u cursor_mode=%u\n",
               multiple, types, cursor_mode);
    /* types is a bitfield: 1 monitor, 4 virtual. This host sends 4.
       Either bit is the existing HDMI stream. cursor_mode 4 is metadata. */
    if ((types & (CRD_SOURCE_MONITOR | CRD_SOURCE_VIRTUAL)) == 0 ||
        (cursor_mode != 0 &&
         (cursor_mode & CRD_CURSOR_METADATA) == 0 &&
         cursor_mode != 2)) {
      g_printerr("SelectSources rejected multiple=%d types=%u cursor_mode=%u\n",
                 multiple, types, cursor_mode);
      fail_call(invocation, G_DBUS_ERROR_INVALID_ARGS,
                "SelectSources needs a monitor or virtual source and cursor mode 2 or 4");
      g_variant_unref(options);
      return;
    }
    selected_source_type = (types & CRD_SOURCE_MONITOR) != 0 ? CRD_SOURCE_MONITOR : CRD_SOURCE_VIRTUAL;
    if (!schedule_request(server, invocation, sender, options, empty_results())) {
      g_variant_unref(options);
      return;
    }
    session->sources_selected = TRUE;
    g_variant_unref(options);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_SCREEN) == 0 &&
      g_strcmp0(method_name, "OpenPipeWireRemote") == 0) {
    const char *session_handle = NULL;
    GVariant *options = NULL;
    CrdSession *session;

    g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);
    g_variant_unref(options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || !session->started) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "OpenPipeWireRemote is not valid for this session");
      return;
    }
    {
      int fd = capture_open_pipewire_remote();

      if (fd < 0) {
        fail_call(invocation, G_DBUS_ERROR_FAILED, "capture pipewire fd is not ready");
        return;
      }
      /* Kütüphane yeni bir bağlantı verir. İstemciye kopyası gider, bu uç kapanır. */
      if (!return_fd(invocation, fd, TRUE))
        return;
    }
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_CLIPBOARD) == 0 &&
      g_strcmp0(method_name, "RequestClipboard") == 0) {
    const char *session_handle = NULL;
    GVariant *options = NULL;
    CrdSession *session;

    g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);
    g_variant_unref(options);
    session = session_lookup(server, session_handle, sender);
    if (session == NULL || session->screencast || session->started || session->clipboard_requested ||
        !session->devices_selected) {
      fail_call(invocation, G_DBUS_ERROR_FAILED, "RequestClipboard is not valid for this session");
      return;
    }
    session->clipboard_requested = TRUE;
    /* Bu metotta Request.Response yoktur. */
    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_CLIPBOARD) == 0 &&
      g_strcmp0(method_name, "SetSelection") == 0) {
    handle_set_selection(server, sender, parameters, invocation);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_CLIPBOARD) == 0 &&
      g_strcmp0(method_name, "SelectionWrite") == 0) {
    handle_selection_write(server, sender, parameters, invocation);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_CLIPBOARD) == 0 &&
      g_strcmp0(method_name, "SelectionWriteDone") == 0) {
    handle_selection_write_done(server, sender, parameters, invocation);
    return;
  }

  if (g_strcmp0(interface_name, CRD_IFACE_CLIPBOARD) == 0 &&
      g_strcmp0(method_name, "SelectionRead") == 0) {
    handle_selection_read(server, sender, parameters, invocation);
    return;
  }

  fail_call(invocation, G_DBUS_ERROR_UNKNOWN_METHOD, "method is not implemented");
}

static GVariant *
desktop_get_property(GDBusConnection *connection G_GNUC_UNUSED,
                     const gchar *sender G_GNUC_UNUSED,
                     const gchar *object_path G_GNUC_UNUSED,
                     const gchar *interface_name,
                     const gchar *property_name,
                     GError **error,
                     gpointer user_data G_GNUC_UNUSED)
{
  if (g_strcmp0(interface_name, CRD_IFACE_REMOTE) == 0) {
    if (g_strcmp0(property_name, "version") == 0)
      return g_variant_new_uint32(CRD_REMOTE_DESKTOP_VERSION);
    if (g_strcmp0(property_name, "AvailableDeviceTypes") == 0)
      return g_variant_new_uint32(CRD_DEVICE_TYPES);
  } else if (g_strcmp0(interface_name, CRD_IFACE_SCREEN) == 0) {
    if (g_strcmp0(property_name, "version") == 0)
      return g_variant_new_uint32(CRD_SCREEN_CAST_VERSION);
    if (g_strcmp0(property_name, "AvailableSourceTypes") == 0)
      return g_variant_new_uint32(CRD_SOURCE_MONITOR | CRD_SOURCE_VIRTUAL);
    if (g_strcmp0(property_name, "AvailableCursorModes") == 0)
      return g_variant_new_uint32(CRD_CURSOR_METADATA);
  } else if (g_strcmp0(interface_name, CRD_IFACE_CLIPBOARD) == 0) {
    if (g_strcmp0(property_name, "version") == 0)
      return g_variant_new_uint32(CRD_CLIPBOARD_VERSION);
  }

  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property");
  return NULL;
}

static const GDBusInterfaceVTable desktop_vtable = {
  desktop_method,
  desktop_get_property,
  NULL,
  { 0 }
};

static void
on_name_owner_changed(GDBusConnection *connection G_GNUC_UNUSED,
                      const gchar *sender_name G_GNUC_UNUSED,
                      const gchar *object_path G_GNUC_UNUSED,
                      const gchar *interface_name G_GNUC_UNUSED,
                      const gchar *signal_name G_GNUC_UNUSED,
                      GVariant *parameters,
                      gpointer user_data)
{
  CrdServer *server = user_data;
  const char *name = NULL;
  const char *old_owner = NULL;
  const char *new_owner = NULL;
  GHashTableIter iter;
  gpointer value;
  GList *doomed = NULL;
  GList *cursor;

  g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (name == NULL || name[0] != ':' || new_owner == NULL || new_owner[0] != '\0')
    return;

  g_hash_table_iter_init(&iter, server->sessions);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CrdSession *session = value;
    if (g_strcmp0(session->sender, name) == 0)
      doomed = g_list_prepend(doomed, session);
  }
  for (cursor = doomed; cursor != NULL; cursor = cursor->next)
    session_close_later(cursor->data);
  g_list_free(doomed);
  (void) old_owner;
}

static void
on_name_acquired(GDBusConnection *connection G_GNUC_UNUSED,
                 const gchar *name,
                 gpointer user_data)
{
  CrdServer *server = user_data;
  server->name_acquired = TRUE;
  fprintf(stderr, "ad alındı: %s\n", name);
}

static void
on_name_lost(GDBusConnection *connection G_GNUC_UNUSED,
             const gchar *name,
             gpointer user_data)
{
  CrdServer *server = user_data;

  if (!server->name_acquired) {
    fprintf(stderr, "ad alınamadı: %s\n", name);
    server->exit_code = 4;
  }
  if (server->loop != NULL && g_main_loop_is_running(server->loop))
    g_main_loop_quit(server->loop);
}

static gboolean
on_terminate(gpointer user_data)
{
  CrdServer *server = user_data;

  if (server->loop != NULL && g_main_loop_is_running(server->loop))
    g_main_loop_quit(server->loop);
  return G_SOURCE_CONTINUE;
}

static gboolean
export_desktop(CrdServer *server, GError **error)
{
  GDBusInterfaceInfo *remote;
  GDBusInterfaceInfo *screen;
  GDBusInterfaceInfo *clipboard;

  remote = g_dbus_node_info_lookup_interface(server->node, CRD_IFACE_REMOTE);
  screen = g_dbus_node_info_lookup_interface(server->node, CRD_IFACE_SCREEN);
  clipboard = g_dbus_node_info_lookup_interface(server->node, CRD_IFACE_CLIPBOARD);
  server->rd_reg = g_dbus_connection_register_object(server->connection,
                                                     CRD_OBJECT_PATH,
                                                     remote,
                                                     &desktop_vtable,
                                                     server,
                                                     NULL,
                                                     error);
  if (server->rd_reg == 0)
    return FALSE;
  server->sc_reg = g_dbus_connection_register_object(server->connection,
                                                     CRD_OBJECT_PATH,
                                                     screen,
                                                     &desktop_vtable,
                                                     server,
                                                     NULL,
                                                     error);
  if (server->sc_reg == 0)
    return FALSE;
  server->cb_reg = g_dbus_connection_register_object(server->connection,
                                                     CRD_OBJECT_PATH,
                                                     clipboard,
                                                     &desktop_vtable,
                                                     server,
                                                     NULL,
                                                     error);
  return server->cb_reg != 0;
}

int
crd_portal_run(const char *bus_address)
{
  CrdServer server;
  GError *error = NULL;
  guint term_id;
  guint int_id;

  memset(&server, 0, sizeof server);
  server.exit_code = 0;

  if (address_is_real_session_bus(bus_address)) {
    fprintf(stderr, "gerçek oturum veriyolu reddedildi\n");
    return 3;
  }

  if (capture_init() != 0) {
    fprintf(stderr, "capture_init başarısız\n");
    return 4;
  }
  eis_server_set_capture_info(capture_stream_info);

  {
    int olay_rc = olay_init();
    if (olay_rc != 0) {
      fprintf(stderr, "olay_init başarısız: %s\n", olay_init_error());
      return 4;
    }
  }
  if (clipboard_init() != 0) {
    fprintf(stderr, "clipboard_init başarısız\n");
    olay_fini();
    return 4;
  }
  server.clipboard_ready = TRUE;

  server.connection = g_dbus_connection_new_for_address_sync(
      bus_address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL,
      NULL,
      &error);
  if (server.connection == NULL) {
    fprintf(stderr, "veriyoluna bağlanılamadı: %s\n", error->message);
    g_error_free(error);
    olay_fini();
    return 4;
  }
  g_dbus_connection_set_exit_on_close(server.connection, FALSE);

  server.node = g_dbus_node_info_new_for_xml(k_introspection, &error);
  if (server.node == NULL) {
    fprintf(stderr, "arayüz okunamadı: %s\n", error->message);
    g_error_free(error);
    g_object_unref(server.connection);
    olay_fini();
    return 4;
  }

  server.sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, session_free);
  server.requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, request_free);
  if (!export_desktop(&server, &error)) {
    fprintf(stderr, "nesne yayınlanamadı: %s\n", error->message);
    g_error_free(error);
    server.exit_code = 4;
    goto done;
  }
  {
    int netmon_rc = netmon_register(server.connection);
    if (netmon_rc != 0) {
      const char *message = netmon_error();
      fprintf(stderr, "netmon_register başarısız: %s\n",
              message != NULL ? message : g_strerror(netmon_rc < 0 ? -netmon_rc : netmon_rc));
      server.exit_code = 4;
      goto done;
    }
  }

  server.name_watch = g_dbus_connection_signal_subscribe(server.connection,
                                                         "org.freedesktop.DBus",
                                                         "org.freedesktop.DBus",
                                                         "NameOwnerChanged",
                                                         "/org/freedesktop/DBus",
                                                         NULL,
                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                         on_name_owner_changed,
                                                         &server,
                                                         NULL);
  server.name_owner = g_bus_own_name_on_connection(server.connection,
                                                   CRD_BUS_NAME,
                                                   G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                                   on_name_acquired,
                                                   on_name_lost,
                                                   &server,
                                                   NULL);
  server.loop = g_main_loop_new(NULL, FALSE);
  if (olay_kanit_arm(g_getenv("CRD_OLAY_KANIT")) != 0) {
    fprintf(stderr, "olay kanıtı kurulamadı\n");
    server.exit_code = 4;
    goto done;
  }
  server.clip_poll = g_timeout_add(200, clipboard_poll_cb, &server);
  term_id = g_unix_signal_add(SIGTERM, on_terminate, &server);
  int_id = g_unix_signal_add(SIGINT, on_terminate, &server);
  g_main_loop_run(server.loop);
  g_source_remove(term_id);
  g_source_remove(int_id);

done:
  if (server.clip_poll != 0)
    g_source_remove(server.clip_poll);
  if (server.name_owner != 0)
    g_bus_unown_name(server.name_owner);
  if (server.name_watch != 0 && server.connection != NULL)
    g_dbus_connection_signal_unsubscribe(server.connection, server.name_watch);
  if (server.requests != NULL)
    g_hash_table_destroy(server.requests);
  if (server.sessions != NULL)
    g_hash_table_destroy(server.sessions);
  if (server.connection != NULL) {
    netmon_unregister();
    if (server.cb_reg != 0)
      g_dbus_connection_unregister_object(server.connection, server.cb_reg);
    if (server.sc_reg != 0)
      g_dbus_connection_unregister_object(server.connection, server.sc_reg);
    if (server.rd_reg != 0)
      g_dbus_connection_unregister_object(server.connection, server.rd_reg);
    g_object_unref(server.connection);
    server.connection = NULL;
  }
  if (server.node != NULL)
    g_dbus_node_info_unref(server.node);
  if (server.loop != NULL)
    g_main_loop_unref(server.loop);
  olay_kanit_flush();
  olay_fini();
  return server.exit_code;
}
