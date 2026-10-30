#include "olay_kanit.h"

#include "giris.h"
#include "olay.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include <glib.h>

static char kanit_path[4096];
static guint kanit_timer;
static int kanit_armed;
static int kanit_wrote;

static int
count_kind(enum giris_rec_kind kind)
{
	int count = 0;
	size_t index;

	for (index = 0; index < giris_record_count(); index++) {
		const struct giris_rec *rec = giris_record_at(index);

		if (rec != NULL && rec->kind == kind)
			count++;
	}
	return count;
}

static const struct giris_rec *
first_kind(enum giris_rec_kind kind)
{
	size_t index;

	for (index = 0; index < giris_record_count(); index++) {
		const struct giris_rec *rec = giris_record_at(index);

		if (rec != NULL && rec->kind == kind)
			return rec;
	}
	return NULL;
}

static void
kanit_write(void)
{
	FILE *out;
	char tmp[sizeof kanit_path + 4];
	const struct giris_rec *motion;
	const struct giris_rec *key;
	int motion_n;
	int key_n;
	int geldi;
	int pass = 1;
	int n;

	if (!kanit_armed || kanit_wrote || kanit_path[0] == '\0')
		return;

	n = snprintf(tmp, sizeof tmp, "%s.tmp", kanit_path);
	if (n < 0 || (size_t) n >= sizeof tmp)
		return;
	out = fopen(tmp, "w");
	if (out == NULL) {
		fprintf(stderr, "olay kanıtı yazılamadı: %s\n", strerror(errno));
		return;
	}

	geldi = olay_geldi();
	motion_n = count_kind(GIRIS_REC_MOTION_ABSOLUTE);
	key_n = count_kind(GIRIS_REC_KEY);
	motion = first_kind(GIRIS_REC_MOTION_ABSOLUTE);
	key = first_kind(GIRIS_REC_KEY);

	fprintf(out, "init=ok\n");
	fprintf(out, "wayland_display=%s\n", giris_wayland_display());
	fprintf(out, "olay_geldi=%d\n", geldi);
	fprintf(out, "kayit_motion_absolute=%d\n", motion_n);
	if (motion != NULL) {
		fprintf(out, "kayit_motion=x=%u y=%u x_extent=%u y_extent=%u\n",
		        motion->x, motion->y, motion->x_extent, motion->y_extent);
	} else {
		fprintf(out, "kayit_motion=(yok)\n");
		pass = 0;
	}
	fprintf(out, "kayit_key=%d\n", key_n);
	if (key != NULL) {
		fprintf(out, "kayit_key_ayrinti=evdev=%u state=%u\n", key->key, key->state);
	} else {
		fprintf(out, "kayit_key_ayrinti=(yok)\n");
		pass = 0;
	}
	fprintf(out, "wl_giris_istegi=%llu\n",
	        (unsigned long long) giris_wl_input_requests());
	fprintf(out, "canli_kapi=%s\n", giris_live_send_enabled() ? "acik" : "kapali");

	if (geldi != 1)
		pass = 0;
	if (motion_n != 1 || motion == NULL || motion->x_extent == 0 || motion->y_extent == 0 ||
	    motion->x != motion->x_extent / 2 || motion->y != motion->y_extent / 2)
		pass = 0;
	if (key_n != 1 || key == NULL || key->key == 0 || key->key > KEY_MAX ||
	    key->state != WL_KEYBOARD_KEY_STATE_PRESSED)
		pass = 0;
	if (giris_live_send_enabled() != 0 || giris_wl_input_requests() != 0)
		pass = 0;

	fprintf(out, "overall=%s\n", pass ? "pass" : "fail");
	if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
		fclose(out);
		unlink(tmp);
		fprintf(stderr, "olay kanıtı diske inmedi: %s\n", strerror(errno));
		return;
	}
	fclose(out);
	if (rename(tmp, kanit_path) != 0) {
		unlink(tmp);
		fprintf(stderr, "olay kanıtı yerine konamadı: %s\n", strerror(errno));
		return;
	}
	kanit_wrote = 1;
}

static gboolean
kanit_timer_cb(gpointer user_data)
{
	(void) user_data;
	if (olay_geldi() != 1)
		return G_SOURCE_CONTINUE;
	kanit_timer = 0;
	/* Kayıt okunurken çeviriciye yeni olay gitmesin. */
	olay_kopru_kapat();
	kanit_write();
	return G_SOURCE_REMOVE;
}

int
olay_kanit_arm(const char *path)
{
	size_t len;

	if (path == NULL || path[0] == '\0')
		return 0;
	len = strlen(path);
	if (len == 0 || len >= sizeof kanit_path)
		return -1;
	memcpy(kanit_path, path, len + 1);
	kanit_armed = 1;
	kanit_wrote = 0;
	kanit_timer = g_timeout_add(50, kanit_timer_cb, NULL);
	if (kanit_timer == 0) {
		kanit_armed = 0;
		kanit_path[0] = '\0';
		return -1;
	}
	return 0;
}

void
olay_kanit_flush(void)
{
	if (!kanit_armed || kanit_wrote)
		return;
	if (kanit_timer != 0) {
		g_source_remove(kanit_timer);
		kanit_timer = 0;
	}
	olay_kopru_kapat();
	kanit_write();
}
