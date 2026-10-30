#include "eis_kuyruk.h"

#include <libeis.h>

#include <pthread.h>

static pthread_mutex_t kuyruk_mu = PTHREAD_MUTEX_INITIALIZER;
static eis_kuyruk_handler kuyruk_handler;
static void *kuyruk_user;
static int kuyruk_sealed;

struct eis_event *__real_eis_get_event(struct eis *eis);

static int giris_ilgili(enum eis_event_type type)
{
	switch (type) {
	case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
	case EIS_EVENT_POINTER_MOTION:
	case EIS_EVENT_BUTTON_BUTTON:
	case EIS_EVENT_SCROLL_DISCRETE:
	case EIS_EVENT_SCROLL_DELTA:
	case EIS_EVENT_SCROLL_STOP:
	case EIS_EVENT_SCROLL_CANCEL:
	case EIS_EVENT_KEYBOARD_KEY:
	case EIS_EVENT_TEXT_KEYSYM:
	case EIS_EVENT_TEXT_UTF8:
	case EIS_EVENT_FRAME:
		return 1;
	default:
		return 0;
	}
}

void eis_kuyruk_set_handler(eis_kuyruk_handler handler, void *user)
{
	pthread_mutex_lock(&kuyruk_mu);
	kuyruk_handler = handler;
	kuyruk_user = user;
	pthread_mutex_unlock(&kuyruk_mu);
}

void eis_kuyruk_seal(void)
{
	pthread_mutex_lock(&kuyruk_mu);
	kuyruk_sealed = 1;
	pthread_mutex_unlock(&kuyruk_mu);
}

struct eis_event *__wrap_eis_get_event(struct eis *eis)
{
	struct eis_event *event;
	enum eis_event_type type;

	event = __real_eis_get_event(eis);
	if (!event) {
		return NULL;
	}
	type = eis_event_get_type(event);
	if (!giris_ilgili(type)) {
		return event;
	}
	if (pthread_mutex_lock(&kuyruk_mu) != 0) {
		return event;
	}
	if (!kuyruk_sealed && kuyruk_handler) {
		kuyruk_handler(event, kuyruk_user);
	}
	pthread_mutex_unlock(&kuyruk_mu);
	return event;
}
