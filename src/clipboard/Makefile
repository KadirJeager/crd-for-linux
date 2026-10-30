CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
CFLAGS += $(shell pkg-config --cflags wayland-client)
LDFLAGS += $(shell pkg-config --libs wayland-client)

EXT_XML := /usr/share/wayland-protocols/staging/ext-data-control/ext-data-control-v1.xml
WLR_XML := /usr/share/qt6/wayland/protocols/wlr-data-control/wlr-data-control-unstable-v1.xml

GEN := gen/ext-data-control-v1-client-protocol.h gen/ext-data-control-v1-protocol.c \
       gen/wlr-data-control-unstable-v1-client-protocol.h gen/wlr-data-control-unstable-v1-protocol.c

.PHONY: all clean test kanit probe-get-fail

all: libcrd_clipboard.a pano_test

libcrd_clipboard.a: clipboard.o gen/ext-data-control-v1-protocol.o gen/wlr-data-control-unstable-v1-protocol.o
	ar rcs $@ $^

pano_test: pano_test.o libcrd_clipboard.a
	$(CC) $(CFLAGS) -o $@ pano_test.o -L. -lcrd_clipboard $(LDFLAGS)

kanit: pano_test
	./pano_test

probe-get-fail: pano_test
	./pano_test --probe-get-fail

gen/ext-data-control-v1-client-protocol.h: $(EXT_XML)
	wayland-scanner client-header $< $@

gen/ext-data-control-v1-protocol.c: $(EXT_XML)
	wayland-scanner private-code $< $@

gen/wlr-data-control-unstable-v1-client-protocol.h: $(WLR_XML)
	wayland-scanner client-header $< $@

gen/wlr-data-control-unstable-v1-protocol.c: $(WLR_XML)
	wayland-scanner private-code $< $@

clipboard.o: clipboard.c clipboard.h clipboard_internal.h $(GEN)
	$(CC) $(CFLAGS) -I. -Igen -c clipboard.c -o $@

pano_test.o: pano_test.c clipboard.h clipboard_internal.h
	$(CC) $(CFLAGS) -I. -c pano_test.c -o $@

gen/ext-data-control-v1-protocol.o: gen/ext-data-control-v1-protocol.c gen/ext-data-control-v1-client-protocol.h
	$(CC) $(CFLAGS) -Igen -c gen/ext-data-control-v1-protocol.c -o $@

gen/wlr-data-control-unstable-v1-protocol.o: gen/wlr-data-control-unstable-v1-protocol.c gen/wlr-data-control-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) -Igen -c gen/wlr-data-control-unstable-v1-protocol.c -o $@

clean:
	rm -f *.o gen/*.o libcrd_clipboard.a pano_test
