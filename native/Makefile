CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 json-glib-1.0 libdrm)
LDLIBS += $(shell $(PKG_CONFIG) --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 json-glib-1.0 libdrm)
PLUGIN_DIR ?= /usr/local/lib/gstreamer-1.0
ENGINE_PACKAGES = gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0 gio-unix-2.0 json-glib-1.0
PREFIX ?= /usr/local

.PHONY: all install clean
all: libgstadmiralholepunch.so signage-video-engine

SOURCES = gst-admiral-holepunch.c gst-admiral-videoplane.c

libgstadmiralholepunch.so: $(SOURCES) gst-admiral-videoplane.h drm-broker.h video-ledger.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -fPIC -shared -o $@ $(SOURCES) $(LDLIBS)

signage-video-engine: signage-video-engine.c
	$(CC) $(shell $(PKG_CONFIG) --cflags $(ENGINE_PACKAGES)) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs $(ENGINE_PACKAGES))

install: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 0755 libgstadmiralholepunch.so $(DESTDIR)$(PLUGIN_DIR)/
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 signage-video-engine $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f libgstadmiralholepunch.so signage-video-engine
