/* Özel veriyolunda NetworkMonitor sunar.
   Arayüz libcrd_netmon üzerinden kaydedilir; ad bu süreçte alınır.
   Gerçek oturum veriyoluna bağlanmaz. */

#include "netmon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-unix.h>

#define NETMON_BUS_NAME "org.freedesktop.portal.Desktop"
#define REAL_SESSION_BUS_PATH "/run/user/1000/bus"

typedef struct {
  GDBusConnection *connection;
  guint own_id;
  guint sigterm_id;
  guint sigint_id;
  GMainLoop *loop;
  int exit_code;
  gboolean stopping;
} Process;

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
  end = marker;
  while (*end != '\0' && *end != ',' && *end != ';')
    end++;
  if (end == marker)
    return NULL;
  return g_strndup(marker, (gsize) (end - marker));
}

static char *
normalize_path(const char *path)
{
  char *canon;
  char *resolved;
  gsize length;

  if (path == NULL || path[0] == '\0')
    return NULL;
  canon = g_canonicalize_filename(path, NULL);
  resolved = realpath(canon, NULL);
  if (resolved != NULL) {
    g_free(canon);
    canon = g_strdup(resolved);
    free(resolved);
  }
  length = strlen(canon);
  while (length > 1 && canon[length - 1] == '/') {
    canon[--length] = '\0';
  }
  return canon;
}

static gboolean
paths_match(const char *left, const char *right)
{
  char *a;
  char *b;
  gboolean match;

  if (left == NULL || right == NULL)
    return FALSE;
  a = normalize_path(left);
  b = normalize_path(right);
  match = a != NULL && b != NULL && g_strcmp0(a, b) == 0;
  g_free(a);
  g_free(b);
  return match;
}

static gboolean
one_address_is_real(const char *address)
{
  const char *env;
  char *given;
  char *session_path;
  char *runtime_bus;
  gboolean match = FALSE;

  if (address == NULL || address[0] == '\0')
    return TRUE;
  if (g_strcmp0(address, REAL_SESSION_BUS_PATH) == 0 ||
      paths_match(address, REAL_SESSION_BUS_PATH))
    return TRUE;

  env = g_getenv("DBUS_SESSION_BUS_ADDRESS");
  if (env != NULL && g_strcmp0(env, address) == 0)
    return TRUE;

  given = address_unix_path(address);
  session_path = address_unix_path(env);
  runtime_bus = g_build_filename(g_get_user_runtime_dir(), "bus", NULL);

  if (paths_match(given, REAL_SESSION_BUS_PATH) ||
      paths_match(given, session_path) ||
      paths_match(given, runtime_bus) ||
      g_strcmp0(given, REAL_SESSION_BUS_PATH) == 0 ||
      g_strcmp0(given, runtime_bus) == 0)
    match = TRUE;

  g_free(given);
  g_free(session_path);
  g_free(runtime_bus);
  return match;
}

static gboolean
address_is_real_session_bus(const char *address)
{
  char **parts;
  guint i;
  gboolean match = FALSE;

  if (address == NULL || address[0] == '\0')
    return TRUE;

  if (one_address_is_real(address))
    return TRUE;

  parts = g_strsplit(address, ";", 0);
  for (i = 0; parts[i] != NULL; i++) {
    g_strstrip(parts[i]);
    if (parts[i][0] != '\0' && one_address_is_real(parts[i]))
      match = TRUE;
  }
  g_strfreev(parts);
  return match;
}

static void
on_name_acquired(GDBusConnection *connection, const char *name, gpointer user_data)
{
  Process *process = user_data;
  GNetworkMonitor *monitor;
  gboolean available;
  gboolean metered;
  guint32 connectivity;

  (void) connection;
  if (process->stopping)
    return;
  monitor = g_network_monitor_get_default();
  available = g_network_monitor_get_network_available(monitor);
  metered = g_network_monitor_get_network_metered(monitor);
  connectivity = (guint32) g_network_monitor_get_connectivity(monitor);
  fprintf(stderr, "ad alındı: %s\n", name);
  fprintf(stderr, "durum available=%s metered=%s connectivity=%u\n",
          available ? "true" : "false",
          metered ? "true" : "false",
          connectivity);
}

static void
on_name_lost(GDBusConnection *connection, const char *name, gpointer user_data)
{
  Process *process = user_data;

  (void) connection;
  if (process->stopping)
    return;
  fprintf(stderr, "ad alınamadı: %s\n", name);
  process->exit_code = 4;
  if (process->loop != NULL && g_main_loop_is_running(process->loop))
    g_main_loop_quit(process->loop);
}

static void
on_closed(GDBusConnection *connection,
          gboolean remote_peer_vanished,
          GError *error,
          gpointer user_data)
{
  Process *process = user_data;

  (void) connection;
  (void) remote_peer_vanished;
  if (process->stopping)
    return;
  fprintf(stderr, "veriyolu kapandı: %s\n", error != NULL ? error->message : "neden yok");
  process->exit_code = 4;
  if (process->loop != NULL && g_main_loop_is_running(process->loop))
    g_main_loop_quit(process->loop);
}

static gboolean
on_unix_signal(gpointer user_data)
{
  Process *process = user_data;

  process->stopping = TRUE;
  process->exit_code = 0;
  if (process->loop != NULL && g_main_loop_is_running(process->loop))
    g_main_loop_quit(process->loop);
  return G_SOURCE_REMOVE;
}

static void
shutdown_process(Process *process)
{
  process->stopping = TRUE;
  if (process->sigterm_id != 0) {
    g_source_remove(process->sigterm_id);
    process->sigterm_id = 0;
  }
  if (process->sigint_id != 0) {
    g_source_remove(process->sigint_id);
    process->sigint_id = 0;
  }
  if (process->own_id != 0) {
    g_bus_unown_name(process->own_id);
    process->own_id = 0;
  }
  netmon_unregister();
  if (process->loop != NULL) {
    g_main_loop_unref(process->loop);
    process->loop = NULL;
  }
  if (process->connection != NULL) {
    g_object_unref(process->connection);
    process->connection = NULL;
  }
}

int
main(int argc, char **argv)
{
  Process process;
  GError *error = NULL;
  const char *address;
  const char *source;
  const char *reason;
  int check_rc;
  int register_rc;
  int exit_code;

  setvbuf(stderr, NULL, _IONBF, 0);
  memset(&process, 0, sizeof process);

  if (argc != 2 || argv[1][0] == '\0') {
    fprintf(stderr, "kullanım: crd-netmon <dbus-adresi>\n");
    return 2;
  }
  address = argv[1];
  if (address_is_real_session_bus(address)) {
    fprintf(stderr, "gerçek oturum veriyolu reddedildi\n");
    return 3;
  }

  check_rc = netmon_check();
  if (check_rc == -ENODEV) {
    fprintf(stderr, "ağ kaynağı kullanılamaz: %s\n",
            netmon_error() != NULL ? netmon_error() : "kaynak yok");
    netmon_unregister();
    return 4;
  }
  if (check_rc != 0) {
    fprintf(stderr, "arayüz okunamadı: %s\n",
            netmon_error() != NULL ? netmon_error() : "belge yok");
    netmon_unregister();
    return 4;
  }
  source = netmon_monitor_name();
  if (source == NULL) {
    fprintf(stderr, "ağ kaynağı kullanılamaz: kaynak yok\n");
    netmon_unregister();
    return 4;
  }
  fprintf(stderr, "ağ kaynağı: %s\n", source);

  process.connection = g_dbus_connection_new_for_address_sync(
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL,
      NULL,
      &error);
  if (process.connection == NULL) {
    fprintf(stderr, "veriyoluna bağlanılamadı: %s\n", error->message);
    g_clear_error(&error);
    netmon_unregister();
    return 4;
  }
  g_dbus_connection_set_exit_on_close(process.connection, FALSE);
  g_signal_connect(process.connection, "closed", G_CALLBACK(on_closed), &process);

  register_rc = netmon_register(process.connection);
  if (register_rc != 0) {
    reason = netmon_error();
    fprintf(stderr, "nesne yayınlanamadı: %s\n", reason != NULL ? reason : "kayıt yapılamadı");
    shutdown_process(&process);
    return 4;
  }

  process.loop = g_main_loop_new(NULL, FALSE);
  process.own_id = g_bus_own_name_on_connection(process.connection,
                                                NETMON_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                                on_name_acquired,
                                                on_name_lost,
                                                &process,
                                                NULL);
  process.sigterm_id = g_unix_signal_add(SIGTERM, on_unix_signal, &process);
  process.sigint_id = g_unix_signal_add(SIGINT, on_unix_signal, &process);
  g_main_loop_run(process.loop);

  exit_code = process.exit_code;
  shutdown_process(&process);
  return exit_code;
}
