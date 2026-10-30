#include "olay.h"

#include "eis_kuyruk.h"
#include "eis_server.h"
#include "giris.h"

#include <libeis.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int motion_n;
static atomic_int key_n;
static atomic_int fail_rc;
static int inited;
static char err[256];

static void on_eis(struct eis_event *event, void *user)
{
	enum eis_event_type type;
	int rc;

	(void)user;
	type = eis_event_get_type(event);
	rc = giris_handle_ei_event(event);
	if (rc != 0) {
		atomic_store(&fail_rc, rc);
	}
	if (type == EIS_EVENT_POINTER_MOTION_ABSOLUTE) {
		atomic_fetch_add(&motion_n, 1);
	} else if (type == EIS_EVENT_KEYBOARD_KEY) {
		atomic_fetch_add(&key_n, 1);
	}
}

int olay_init(void)
{
	int rc;

	if (inited) {
		return -EBUSY;
	}
	err[0] = '\0';
	atomic_store(&motion_n, 0);
	atomic_store(&key_n, 0);
	atomic_store(&fail_rc, 0);
	eis_kuyruk_set_handler(on_eis, NULL);

	rc = giris_init();
	if (rc != 0) {
		snprintf(err, sizeof(err), "giris_init: %s", giris_init_error());
		eis_kuyruk_seal();
		return rc;
	}
	if (giris_set_live_send(0) != 0 || giris_live_send_enabled() != 0) {
		snprintf(err, sizeof(err), "canlı kapı baştan kapalı değil");
		eis_kuyruk_seal();
		giris_fini();
		return -EPERM;
	}
	/* Ölçüm kapıyı kapalı görür. Telefon bu oturumu sürsün denince
	   CRD_CANLI_GIRIS=1 ile açılır; varsayılan hâlâ kapalı. */
	if (getenv("CRD_CANLI_GIRIS") &&
	    strcmp(getenv("CRD_CANLI_GIRIS"), "1") == 0) {
		if (giris_set_live_send(1) != 0 || !giris_live_send_enabled()) {
			snprintf(err, sizeof(err), "canlı kapı açılmadı");
			eis_kuyruk_seal();
			giris_fini();
			return -EPERM;
		}
	}
	rc = eis_server_init();
	if (rc != 0) {
		snprintf(err, sizeof(err), "eis_server_init: %d", rc);
		eis_kuyruk_seal();
		giris_fini();
		return rc;
	}
	inited = 1;
	return 0;
}

const char *olay_init_error(void)
{
	return err;
}

int olay_client_fd(void)
{
	if (!inited) {
		return -EINVAL;
	}
	return eis_client_fd();
}

int olay_open_client_fd(void)
{
	if (!inited) {
		return -EINVAL;
	}
	return eis_open_client_fd();
}

int olay_geldi(void)
{
	int rc = atomic_load(&fail_rc);

	if (rc != 0) {
		return rc;
	}
	if (atomic_load(&motion_n) >= 1 && atomic_load(&key_n) >= 1) {
		return 1;
	}
	return 0;
}

void olay_kopru_kapat(void)
{
	eis_kuyruk_seal();
}

void olay_fini(void)
{
	eis_kuyruk_seal();
	if (!inited) {
		return;
	}
	giris_fini();
	inited = 0;
}
