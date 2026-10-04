# Builds libcurl.a (the libcurl API over fetch) with Emscripten.
#   make                         -> build/libcurl.a and build/libcurl.pc
#   make install PREFIX=/src/lib
#   make check                   -> C API tests under Node (needs emcc and node)

ifeq ($(origin CC),default)
CC = emcc
endif
ifeq ($(origin AR),default)
AR = emar
endif
PREFIX ?= /usr/local
CFLAGS ?= -O2
LIBCURL_FETCH_CFLAGS = -fPIC -Iinclude -DCURL_DISABLE_TYPECHECK -DCURL_NO_OLDIES -Wall -Wno-deprecated-declarations
VERSION = $(shell sed -n 's/^\#define LIBCURL_VERSION "\(.*\)"/\1/p' include/curl/curlver.h)

SOURCES = src/easy.c src/perform.c src/multi.c src/mime.c src/version.c src/fetch_js.c
OBJECTS = $(patsubst src/%.c,build/%.o,$(SOURCES))

all: build/libcurl.a build/libcurl.pc

build:
	mkdir -p build

build/%.o: src/%.c src/internal.h | build
	$(CC) $(CFLAGS) $(LIBCURL_FETCH_CFLAGS) -c $< -o $@

# EM_JS bodies are only found in native objects: an LTO (bitcode) object in an
# archive leaves its JavaScript functions undefined when linking executables.
build/fetch_js.o: src/fetch_js.c src/internal.h | build
	$(CC) $(filter-out -flto,$(CFLAGS)) $(LIBCURL_FETCH_CFLAGS) -c $< -o $@

build/libcurl.a: $(OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

build/libcurl.pc: libcurl.pc.in include/curl/curlver.h | build
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@

install: all
	mkdir -p $(PREFIX)/lib/pkgconfig $(PREFIX)/include/curl
	cp build/libcurl.a $(PREFIX)/lib/libcurl.a
	cp build/libcurl.pc $(PREFIX)/lib/pkgconfig/libcurl.pc
	cp include/curl/*.h $(PREFIX)/include/curl/

check: all
	$(MAKE) -C test check

clean:
	rm -rf build

.PHONY: all install check clean
