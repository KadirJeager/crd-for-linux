# derleme: make -C src/netmon
# libcrd_netmon.a çağıranın GDBus bağlantısına arayüzü kaydeder.
# crd-netmon bu kütüphaneyi özel veriyolunda kullanır.
CC ?= gcc
PKG_CFLAGS := $(shell pkg-config --cflags gio-2.0)
PKG_LIBS := $(shell pkg-config --libs gio-2.0)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE $(PKG_CFLAGS)

.PHONY: all test clean

all: libcrd_netmon.a crd-netmon

netmon.o: netmon.c netmon.h
	$(CC) $(CFLAGS) -c -o $@ netmon.c

crd-netmon.o: crd-netmon.c netmon.h
	$(CC) $(CFLAGS) -c -o $@ crd-netmon.c

libcrd_netmon.a: netmon.o
	ar rcs $@ netmon.o

crd-netmon: crd-netmon.o libcrd_netmon.a
	$(CC) -o $@ crd-netmon.o libcrd_netmon.a $(PKG_LIBS)

test: all
	./olc.sh

clean:
	rm -f netmon.o crd-netmon.o crd-netmon libcrd_netmon.a
