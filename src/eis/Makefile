CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11
CFLAGS += $(shell pkg-config --cflags libeis-1.0)
LIBS_EIS := $(shell pkg-config --libs libeis-1.0)
LIBS_EI := $(shell pkg-config --libs libei-1.0)
LIBS_XKB := $(shell pkg-config --libs xkbcommon)
LIBS_WL := $(shell pkg-config --libs wayland-client)
CIKTI_A := ../cikti/libcrd_cikti.a

.PHONY: all clean test

all: libcrd_eis.a test_eis_client

libcrd_eis.a: eis_server.o eis_keymap.o
	$(AR) rcs $@ $^

eis_server.o: eis_server.c eis_server.h eis_keymap.h ../cikti/cikti.h ../capture/capture.h
	$(CC) $(CFLAGS) -pthread -I../cikti -I../capture -c eis_server.c -o $@

eis_keymap.o: eis_keymap.c eis_keymap.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags wayland-client xkbcommon) -c eis_keymap.c -o $@

../cikti/libcrd_cikti.a:
	$(MAKE) -C ../cikti

test_eis_client: test_eis_client.c libcrd_eis.a $(CIKTI_A)
	$(CC) $(CFLAGS) -pthread -I../cikti test_eis_client.c -o $@ -L. -lcrd_eis $(CIKTI_A) $(LIBS_EIS) $(LIBS_EI) $(LIBS_XKB) $(LIBS_WL) -pthread

test: test_eis_client
	@mkdir -p kanit
	./test_eis_client

clean:
	rm -f *.o libcrd_eis.a test_eis_client
