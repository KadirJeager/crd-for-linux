#define _POSIX_C_SOURCE 200809L

#include "clipboard.h"
#include "clipboard_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_evidence(FILE *f, int exit_code)
{
	fclose(f);
	return exit_code;
}

static bool blob_equal(const void *a, size_t an, const void *b, size_t bn)
{
	if (an != bn)
		return false;
	if (an == 0)
		return true;
	return memcmp(a, b, an) == 0;
}

struct pano_tour_result {
	bool original_read_ok;
	unsigned set_call_count;
	int rc;
};

/*
 * Real clipboard round-trip: only writes when the initial text/plain read
 * succeeded; restores original bytes (or empty selection) at the end.
 */
static int pano_tour(FILE *out, struct pano_tour_result *result)
{
	void *saved = NULL;
	size_t saved_len = 0;
	void *probe = NULL;
	size_t probe_len = 0;
	void *roundtrip = NULL;
	size_t roundtrip_len = 0;
	void *restored = NULL;
	size_t restored_len = 0;
	const char test_payload[] = "crd-clipboard-pano-test-2026-10-29";
	size_t i;
	int rc = 0;
	bool original_read_ok = false;

	if (result) {
		result->original_read_ok = false;
		result->set_call_count = 0;
		result->rc = 1;
	}

	fprintf(out, "protocol: %s\n", clipboard_protocol_name());

	if (clipboard_get("text/plain", &saved, &saved_len) != 0) {
		fprintf(out, "original text/plain: read FAIL\n");
		fprintf(out, "original read: FAIL (skipped all clipboard_set)\n");
		rc = 1;
		goto done;
	}

	original_read_ok = true;
	if (saved_len == 0)
		fprintf(out, "original text/plain: (empty)\n");
	else
		fprintf(out, "original text/plain: %.*s\n", (int)saved_len, (const char *)saved);

	fprintf(out, "selection mime types:\n");
	for (i = 0; i < clipboard_selection_mime_count(); i++)
		fprintf(out, "  - %s\n", clipboard_selection_mime_at(i));

	if (clipboard_set("text/plain", test_payload, sizeof(test_payload) - 1) != 0) {
		fprintf(out, "set test payload: FAIL\n");
		rc = 1;
		goto restore;
	}

	if (clipboard_get("text/plain", &roundtrip, &roundtrip_len) != 0) {
		fprintf(out, "read back test payload: FAIL\n");
		rc = 1;
		goto restore;
	}

	fprintf(out, "roundtrip test payload: %.*s\n", (int)roundtrip_len, (const char *)roundtrip);
	if (roundtrip_len != sizeof(test_payload) - 1 ||
	    memcmp(roundtrip, test_payload, roundtrip_len) != 0) {
		fprintf(out, "roundtrip match: FAIL\n");
		rc = 1;
	} else {
		fprintf(out, "roundtrip match: OK\n");
	}

restore:
	if (!original_read_ok)
		goto done;

	if (saved_len > 0 && saved) {
		if (clipboard_set("text/plain", saved, saved_len) != 0) {
			fprintf(out, "restore original: FAIL\n");
			rc = 1;
		}
	} else {
		if (clipboard_set(NULL, NULL, 0) != 0) {
			fprintf(out, "restore empty selection: FAIL\n");
			rc = 1;
		}
	}

	if (saved_len > 0 && saved) {
		if (clipboard_get("text/plain", &restored, &restored_len) != 0) {
			fprintf(out, "verify restore read: FAIL\n");
			rc = 1;
		} else if (!blob_equal(saved, saved_len, restored, restored_len)) {
			fprintf(out, "verify restore match: FAIL\n");
			rc = 1;
		} else {
			fprintf(out, "verify restore match: OK\n");
		}
	} else {
		if (clipboard_get("text/plain", &probe, &probe_len) == 0) {
			fprintf(out, "verify empty restore: FAIL (still has text/plain)\n");
			rc = 1;
			free(probe);
		} else {
			fprintf(out, "verify empty restore: OK\n");
		}
	}

done:
	free(saved);
	free(roundtrip);
	free(restored);

	if (result) {
		result->original_read_ok = original_read_ok;
		result->set_call_count = clipboard_test_set_call_count();
		result->rc = rc;
	}
	return rc;
}

static int run_get_fail_probe(bool need_init)
{
	struct pano_tour_result tour = { 0 };
	FILE *null_out;
	int rc;

	if (need_init && clipboard_init() != 0)
		return -1;

	clipboard_test_reset();
	clipboard_test_inject_get_fail(1);

	null_out = fopen("/dev/null", "w");
	if (!null_out)
		return -1;

	rc = pano_tour(null_out, &tour);
	fclose(null_out);

	printf("get_fail_probe: original_read_ok=%s\n",
	       tour.original_read_ok ? "true" : "false");
	printf("get_fail_probe: clipboard_set_call_count=%u\n", tour.set_call_count);

	if (tour.original_read_ok || tour.set_call_count != 0 || rc == 0)
		return 1;
	return 0;
}

int main(int argc, char **argv)
{
	FILE *out;
	struct pano_tour_result tour = { 0 };
	int rc;
	int probe_rc = 0;

	if (argc >= 2 && strcmp(argv[1], "--probe-get-fail") == 0)
		return run_get_fail_probe(true);

	out = fopen("kanit/pano.txt", "w");
	if (!out)
		return 1;

	if (clipboard_init() != 0) {
		fprintf(out, "init: FAIL\n");
		return write_evidence(out, 1);
	}

	clipboard_test_reset();
	rc = pano_tour(out, &tour);
	fprintf(out, "clipboard_set_call_count: %u\n", tour.set_call_count);

	probe_rc = run_get_fail_probe(false);
	fprintf(out, "get_fail_probe: clipboard_set_on_read_fail=%s\n",
		probe_rc == 0 ? "no" : "yes");

	rc = write_evidence(out, rc);
	if (probe_rc != 0 && rc == 0)
		rc = 2;

	printf("read_fail_clipboard_set_called=%s\n",
	       probe_rc == 0 ? "no" : "yes");

	return rc;
}
