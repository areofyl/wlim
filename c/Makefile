PKG_CONFIG := /usr/bin/pkg-config
CC := /usr/bin/gcc
CFLAGS := -O2 -Wall $(shell $(PKG_CONFIG) --cflags gtk4 atspi-2 gtk4-layer-shell-0)
LDFLAGS := $(shell $(PKG_CONFIG) --libs gtk4 atspi-2 gtk4-layer-shell-0) -lm

wlim: wlim.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f wlim

.PHONY: clean
