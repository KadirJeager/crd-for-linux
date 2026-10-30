CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
CFLAGS += $(shell pkg-config --cflags wayland-client)

PROTO := /usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml
GEN_H := gen/xdg-output-unstable-v1-client-protocol.h
GEN_C := gen/xdg-output-unstable-v1-protocol.c

.PHONY: all clean

all: libcrd_cikti.a

libcrd_cikti.a: cikti.o gen/xdg.o
	ar rcs $@ $^

cikti.o: cikti.c cikti.h $(GEN_H)
	$(CC) $(CFLAGS) -Igen -c -o $@ cikti.c

gen/xdg.o: $(GEN_C) $(GEN_H)
	$(CC) $(CFLAGS) -Igen -Wno-unused-parameter -c -o $@ $(GEN_C)

$(GEN_H): $(PROTO)
	wayland-scanner client-header $< $@

$(GEN_C): $(PROTO)
	wayland-scanner private-code $< $@

clean:
	rm -f cikti.o gen/xdg.o libcrd_cikti.a
