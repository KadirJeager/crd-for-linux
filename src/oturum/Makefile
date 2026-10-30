CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CFLAGS += $(shell pkg-config --cflags wayland-server)
LIBS := $(shell pkg-config --libs wayland-server)

PROTO := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

.PHONY: all clean

all: gtk-ekran

gen/xdg-shell-server.h: $(PROTO)
	mkdir -p gen
	wayland-scanner server-header $< $@

gen/xdg-shell-protocol.c: $(PROTO)
	mkdir -p gen
	wayland-scanner private-code $< $@

gtk-ekran: gtk-ekran.c gen/xdg-shell-server.h gen/xdg-shell-protocol.c
	$(CC) $(CFLAGS) -Igen -o $@ gtk-ekran.c gen/xdg-shell-protocol.c $(LIBS)

clean:
	rm -f gtk-ekran
