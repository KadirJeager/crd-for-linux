/* clipboard_get / clipboard_set ölçümü. Portal kaynaklarına dokunmaz. */

#include "clipboard.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
write_file(const char *path, const void *data, size_t len)
{
	FILE *handle;
	size_t wrote;

	handle = fopen(path, "wb");
	if (!handle)
		return -1;
	wrote = len == 0 ? 0 : fwrite(data, 1, len, handle);
	if (fclose(handle) != 0 || wrote != len)
		return -1;
	return 0;
}

static int
read_file(const char *path, unsigned char **data, size_t *len)
{
	FILE *handle;
	long size;
	unsigned char *buf;

	handle = fopen(path, "rb");
	if (!handle)
		return -1;
	if (fseek(handle, 0, SEEK_END) != 0) {
		fclose(handle);
		return -1;
	}
	size = ftell(handle);
	if (size < 0) {
		fclose(handle);
		return -1;
	}
	if (fseek(handle, 0, SEEK_SET) != 0) {
		fclose(handle);
		return -1;
	}
	buf = size == 0 ? NULL : malloc((size_t) size);
	if (size > 0 && buf == NULL) {
		fclose(handle);
		return -1;
	}
	if (size > 0 && fread(buf, 1, (size_t) size, handle) != (size_t) size) {
		free(buf);
		fclose(handle);
		return -1;
	}
	fclose(handle);
	*data = buf;
	*len = (size_t) size;
	return 0;
}

static int
cmd_get(const char *path)
{
	void *data = NULL;
	size_t len = 0;

	if (clipboard_init() != 0) {
		fprintf(stderr, "clipboard_init başarısız\n");
		return 1;
	}
	if (clipboard_get("text/plain", &data, &len) != 0) {
		printf("empty=1\n");
		fflush(stdout);
		free(data);
		return 0;
	}
	if (write_file(path, data, len) != 0) {
		fprintf(stderr, "dosya yazılamadı: %s\n", strerror(errno));
		free(data);
		return 1;
	}
	printf("bytes=%zu\n", len);
	fflush(stdout);
	free(data);
	return 0;
}

static int
cmd_clear(void)
{
	if (clipboard_init() != 0) {
		fprintf(stderr, "clipboard_init başarısız\n");
		return 1;
	}
	if (clipboard_set(NULL, NULL, 0) != 0) {
		fprintf(stderr, "clipboard_set clear başarısız\n");
		return 1;
	}
	printf("clear=ok\n");
	fflush(stdout);
	return 0;
}

static int
cmd_hold(const char *path)
{
	unsigned char *data = NULL;
	size_t len = 0;

	if (read_file(path, &data, &len) != 0) {
		fprintf(stderr, "dosya okunamadı\n");
		return 1;
	}
	if (clipboard_init() != 0) {
		fprintf(stderr, "clipboard_init başarısız\n");
		free(data);
		return 1;
	}
	if (clipboard_set("text/plain", len > 0 ? data : NULL, len) != 0) {
		fprintf(stderr, "clipboard_set başarısız\n");
		free(data);
		return 1;
	}
	free(data);
	printf("ready\n");
	fflush(stdout);

	for (;;) {
		struct pollfd pfd;
		void *current = NULL;
		size_t current_len = 0;
		int ready;

		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		pfd.revents = 0;
		ready = poll(&pfd, 1, 50);
		if (ready > 0) {
			char drain[256];
			ssize_t got = read(STDIN_FILENO, drain, sizeof drain);
			if (got <= 0)
				break;
		} else if (ready < 0 && errno != EINTR) {
			break;
		}

		if (clipboard_get("text/plain", &current, &current_len) == 0)
			free(current);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "kullanım: clip-io get|hold|clear ...\n");
		return 2;
	}
	if (strcmp(argv[1], "get") == 0 && argc == 3)
		return cmd_get(argv[2]);
	if (strcmp(argv[1], "hold") == 0 && argc == 3)
		return cmd_hold(argv[2]);
	if (strcmp(argv[1], "clear") == 0 && argc == 2)
		return cmd_clear();
	fprintf(stderr, "kullanım: clip-io get|hold|clear ...\n");
	return 2;
}
