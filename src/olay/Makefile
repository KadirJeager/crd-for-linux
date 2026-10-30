CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -pthread
CFLAGS += $(shell pkg-config --cflags libeis-1.0 libei-1.0 wayland-client xkbcommon)
LIBS := $(shell pkg-config --libs wayland-client libeis-1.0 libei-1.0 xkbcommon xkbcommon-x11 x11 xcb xcb-xkb) -lX11-xcb -pthread

EIS_A := ../eis/libcrd_eis.a
GIRIS_A := ../giris/libcrd_giris.a
CIKTI_A := ../cikti/libcrd_cikti.a

.PHONY: all test clean

all: libcrd_olay.a olay-olc

libcrd_olay.a: olay.o eis_kuyruk.o
	$(AR) rcs $@ $^

# src/eis/Makefile çalıştırılmaz. Yeni dosya burada derlenir; mevcut .a aynı kalır.
eis_kuyruk.o: ../eis/eis_kuyruk.c ../eis/eis_kuyruk.h
	$(CC) $(CFLAGS) -c -o $@ ../eis/eis_kuyruk.c

olay.o: olay.c olay.h ../eis/eis_kuyruk.h ../eis/eis_server.h ../giris/giris.h
	$(CC) $(CFLAGS) -I../eis -I../giris -c -o $@ olay.c

olc.o: olc.c olay.h ../giris/giris.h
	$(CC) $(CFLAGS) -I../giris -c -o $@ olc.c

# --wrap, eis_server.o içindeki eis_get_event çağrısını eis_kuyruk.c'ye bağlar.
../cikti/libcrd_cikti.a:
	$(MAKE) -C ../cikti

olay-olc: olc.o libcrd_olay.a $(EIS_A) $(GIRIS_A) $(CIKTI_A)
	$(CC) $(CFLAGS) -o $@ olc.o -Wl,--wrap=eis_get_event -Wl,--whole-archive libcrd_olay.a -Wl,--no-whole-archive $(GIRIS_A) $(EIS_A) $(CIKTI_A) $(LIBS)

test: olay-olc
	mkdir -p kanit
	./olay-olc

clean:
	rm -f olay.o eis_kuyruk.o olc.o libcrd_olay.a olay-olc
