# derleme: make -C src/portal
# src/eis, src/olay, src/giris, src/clipboard ve src/capture burada derlenmez.
CC ?= gcc
OLAY_A := ../olay/libcrd_olay.a
EIS_A := ../eis/libcrd_eis.a
GIRIS_A := ../giris/libcrd_giris.a
CLIP_A := ../clipboard/libcrd_clipboard.a
CAP_A := ../capture/libcrd_capture.a
NETMON_A := ../netmon/libcrd_netmon.a
CIKTI_A := ../cikti/libcrd_cikti.a
PKG_CFLAGS := $(shell pkg-config --cflags gio-unix-2.0 libeis-1.0)
PKG_LIBS := $(shell pkg-config --libs gio-unix-2.0 libeis-1.0 wayland-client xkbcommon xkbcommon-x11 x11 xcb xcb-xkb) -lX11-xcb -pthread
PW_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS := $(shell pkg-config --libs libpipewire-0.3) -lm
EI_CFLAGS := $(shell pkg-config --cflags libei-1.0)
EI_LIBS := $(shell pkg-config --libs libei-1.0)
WL_CFLAGS := $(shell pkg-config --cflags wayland-client)
WL_LIBS := $(shell pkg-config --libs wayland-client)
XKB_CFLAGS := $(shell pkg-config --cflags xkbcommon)
XKB_LIBS := $(shell pkg-config --libs xkbcommon)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE $(PKG_CFLAGS) -I../olay -I../giris -I../eis -I../clipboard -I../capture -I../netmon

.PHONY: all clean

all: libcrd_portal.a crd-portal eis-probe clip-io frame-al

portal.o: portal.c portal.h olay_kanit.h ../olay/olay.h ../clipboard/clipboard.h ../capture/capture.h ../netmon/netmon.h
	$(CC) $(CFLAGS) $(WL_CFLAGS) -c -o $@ portal.c

olay_kanit.o: olay_kanit.c olay_kanit.h ../olay/olay.h ../giris/giris.h
	$(CC) $(CFLAGS) $(WL_CFLAGS) -c -o $@ olay_kanit.c

crd-portal.o: crd-portal.c portal.h
	$(CC) $(CFLAGS) -c -o $@ crd-portal.c

libcrd_portal.a: portal.o olay_kanit.o
	ar rcs $@ portal.o olay_kanit.o

# --wrap, eis_server.o içindeki eis_get_event çağrısını eis_kuyruk.c'ye bağlar.
# Olay arşivi EIS sunucusunu bir kez açar; ikinci eis_server_init yok.
../cikti/libcrd_cikti.a:
	$(MAKE) -C ../cikti

crd-portal: crd-portal.o libcrd_portal.a $(OLAY_A) $(EIS_A) $(GIRIS_A) $(CLIP_A) $(CAP_A) $(NETMON_A) $(CIKTI_A)
	$(CC) -o $@ crd-portal.o libcrd_portal.a -Wl,--wrap=eis_get_event -Wl,--whole-archive $(OLAY_A) -Wl,--no-whole-archive $(GIRIS_A) $(EIS_A) $(CLIP_A) $(CAP_A) $(NETMON_A) $(CIKTI_A) $(PKG_LIBS) $(PW_LIBS)

eis-probe: eis_probe.c
	$(CC) $(CFLAGS) $(EI_CFLAGS) $(XKB_CFLAGS) -o $@ eis_probe.c $(EI_LIBS) $(XKB_LIBS)

clip-io: clip_io.c ../clipboard/clipboard.h $(CLIP_A)
	$(CC) $(CFLAGS) $(WL_CFLAGS) -o $@ clip_io.c $(CLIP_A) $(WL_LIBS)

frame-al: frame_al.c
	$(CC) $(CFLAGS) $(PW_CFLAGS) -o $@ frame_al.c $(PW_LIBS) -pthread

clean:
	rm -f portal.o olay_kanit.o crd-portal.o libcrd_portal.a crd-portal eis-probe clip-io frame-al
