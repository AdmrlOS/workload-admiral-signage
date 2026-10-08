CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
PACKAGES = wpe-webkit-2.0 wpebackend-fdo-1.0 egl gbm libdrm gio-unix-2.0 json-glib-1.0 libinput libudev xkbcommon
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))
PREFIX ?= /usr/local

.PHONY: all install clean
all: signage-browser

signage-browser: signage-browser.c input.c input.h ../native/drm-broker.c ../native/drm-broker.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ signage-browser.c input.c ../native/drm-broker.c $(LDLIBS)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 signage-browser $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f signage-browser
