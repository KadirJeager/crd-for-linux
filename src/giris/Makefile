CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client libeis-1.0 xkbcommon xkbcommon-x11)
PKG_LIBS := $(shell pkg-config --libs wayland-client libeis-1.0 libei-1.0 xkbcommon xkbcommon-x11 x11 xcb xcb-xkb) -lX11-xcb -pthread
CFLAGS += $(PKG_CFLAGS)

EIS_A := ../eis/libcrd_eis.a
CIKTI_A := ../cikti/libcrd_cikti.a

PROTO_PTR := proto/wlr-virtual-pointer-unstable-v1.xml
PROTO_KBD := proto/virtual-keyboard-unstable-v1.xml
GEN_PTR_H := gen/wlr-virtual-pointer-unstable-v1-client-protocol.h
GEN_PTR_C := gen/wlr-virtual-pointer-unstable-v1-protocol.c
GEN_KBD_H := gen/virtual-keyboard-unstable-v1-client-protocol.h
GEN_KBD_C := gen/virtual-keyboard-unstable-v1-protocol.c
PROTO_FAKE := proto/fake-input.xml
GEN_FAKE_H := gen/fake-input-client-protocol.h
GEN_FAKE_C := gen/fake-input-protocol.c

.PHONY: all clean test keymap-yedek

all: libcrd_giris.a giris-olc

libcrd_giris.a: giris.o gen/pointer.o gen/keyboard.o gen/fake.o
	$(AR) rcs $@ $^

../cikti/libcrd_cikti.a:
	$(MAKE) -C ../cikti

../eis/libcrd_eis.a:
	$(MAKE) -C ../eis libcrd_eis.a

giris-olc: olc.o libcrd_giris.a $(EIS_A) $(CIKTI_A)
	$(CC) $(CFLAGS) -o $@ olc.o libcrd_giris.a $(EIS_A) $(CIKTI_A) $(PKG_LIBS)

giris.o: giris.c giris.h $(GEN_PTR_H) $(GEN_KBD_H) $(GEN_FAKE_H) ../eis/eis_keymap.h ../cikti/cikti.h
	$(CC) $(CFLAGS) -Igen -I../eis -I../cikti -c -o $@ giris.c

olc.o: olc.c giris.h
	$(CC) $(CFLAGS) -I../eis -c -o $@ olc.c

gen/pointer.o: $(GEN_PTR_C) $(GEN_PTR_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_PTR_C)

gen/keyboard.o: $(GEN_KBD_C) $(GEN_KBD_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_KBD_C)

gen/fake.o: $(GEN_FAKE_C) $(GEN_FAKE_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_FAKE_C)

$(GEN_FAKE_H): $(PROTO_FAKE)
	wayland-scanner client-header $< $@

$(GEN_FAKE_C): $(PROTO_FAKE)
	wayland-scanner private-code $< $@

$(GEN_PTR_H): $(PROTO_PTR)
	wayland-scanner client-header $< $@

$(GEN_PTR_C): $(PROTO_PTR)
	wayland-scanner private-code $< $@

$(GEN_KBD_H): $(PROTO_KBD)
	wayland-scanner client-header $< $@

$(GEN_KBD_C): $(PROTO_KBD)
	wayland-scanner private-code $< $@

keymap-yedek: giris.c giris.h $(GEN_PTR_H) $(GEN_KBD_H) $(GEN_FAKE_H) gen/pointer.o gen/keyboard.o gen/fake.o $(EIS_A) $(CIKTI_A)
	$(CC) $(CFLAGS) -DGIRIS_KEYMAP_PROBE -Igen -I../eis -I../cikti -o $@ giris.c gen/pointer.o gen/keyboard.o gen/fake.o $(EIS_A) $(CIKTI_A) $(PKG_LIBS)

test: giris-olc
	@mkdir -p kanit
	./giris-olc

clean:
	rm -f giris.o olc.o giris-olc libcrd_giris.a gen/pointer.o gen/keyboard.o gen/fake.o keymap-yedek
