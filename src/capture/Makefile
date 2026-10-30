CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -D_GNU_SOURCE -std=c11 -pthread
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client libpipewire-0.3)
PKG_LIBS := $(shell pkg-config --libs wayland-client libpipewire-0.3)
CFLAGS += $(PKG_CFLAGS) -I../cikti
LIBS := $(PKG_LIBS) -lpthread
CIKTI_A := ../cikti/libcrd_cikti.a

PROTO_COPY := /usr/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
PROTO_SRC := /usr/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml

GEN_COPY_H := gen/ext-image-copy-capture-v1-client-protocol.h
GEN_COPY_C := gen/ext-image-copy-capture-v1-protocol.c
GEN_SRC_H := gen/ext-image-capture-source-v1-client-protocol.h
GEN_SRC_C := gen/ext-image-capture-source-v1-protocol.c
GEN_KWIN_H := gen/zkde-screencast-unstable-v1-client-protocol.h
GEN_KWIN_C := gen/zkde-screencast-unstable-v1-protocol.c

.PHONY: all clean

all: libcrd_capture.a olc

libcrd_capture.a: capture.o cursor_theme.o gen/copy.o gen/source.o gen/foreign.o gen/kwin.o
	ar rcs $@ $^

../cikti/libcrd_cikti.a:
	$(MAKE) -C ../cikti

olc: olc.o libcrd_capture.a $(CIKTI_A)
	$(CC) $(CFLAGS) -o $@ olc.o libcrd_capture.a $(CIKTI_A) $(LIBS)

capture.o: capture.c capture.h cursor_theme.h ../cikti/cikti.h $(GEN_COPY_H) $(GEN_SRC_H) $(GEN_KWIN_H)
	$(CC) $(CFLAGS) -Igen -c -o $@ capture.c

cursor_theme.o: cursor_theme.c cursor_theme.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags hyprcursor cairo) -c -o $@ cursor_theme.c

olc.o: olc.c capture.h
	$(CC) $(CFLAGS) -c -o $@ olc.c

gen/copy.o: $(GEN_COPY_C) $(GEN_COPY_H) $(GEN_SRC_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_COPY_C)

$(GEN_KWIN_H): proto/zkde-screencast-unstable-v1.xml
	wayland-scanner client-header $< $@

$(GEN_KWIN_C): proto/zkde-screencast-unstable-v1.xml
	wayland-scanner private-code $< $@

gen/kwin.o: $(GEN_KWIN_C) $(GEN_KWIN_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_KWIN_C)

gen/source.o: $(GEN_SRC_C) $(GEN_SRC_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_SRC_C)

gen/foreign.o: gen/ext-foreign-toplevel-stub.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEN_COPY_H): $(PROTO_COPY)
	wayland-scanner client-header $< $@

$(GEN_COPY_C): $(PROTO_COPY)
	wayland-scanner private-code $< $@

$(GEN_SRC_H): $(PROTO_SRC)
	wayland-scanner client-header $< $@

$(GEN_SRC_C): $(PROTO_SRC)
	wayland-scanner private-code $< $@

clean:
	rm -f capture.o cursor_theme.o olc.o olc libcrd_capture.a gen/copy.o gen/source.o gen/foreign.o gen/kwin.o
