# Run from the repository root with: make check
# The side module uses the same flags php-wasm uses for extensions.

# Resolve node through the shell: the emsdk image puts /emsdk (which holds a
# node/ directory) first on PATH, and make would try to execute that directory.
NODE ?= $(shell command -v node)

SIDE_FLAGS = -sSIDE_MODULE=1 -sWASM_BIGINT=1 -sASYNCIFY=1 '-sASYNCIFY_IMPORTS=*'
MAIN_FLAGS = -sMAIN_MODULE=1 -sWASM_BIGINT=1 -sASYNCIFY=1 -sMODULARIZE=1 -sEXPORT_NAME=TestRuntime \
	-sENVIRONMENT=node -sSTACK_SIZE=1MB -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM \
	-sEXPORTED_RUNTIME_METHODS='["callMain","ccall","UTF8ToString","lengthBytesUTF8","stringToUTF8","getValue","setValue","HEAPU8"]'

all: build/main.js

build:
	mkdir -p build

build/api.so: api.c ../build/libcurl.a | build
	emcc -O2 -fPIC -flto -I../include $(SIDE_FLAGS) -Wl,--whole-archive ../build/libcurl.a -Wl,--no-whole-archive api.c -o $@

build/main.js: main.c build/api.so
	emcc -O2 -flto $(MAIN_FLAGS) --embed-file build/api.so@/api.so main.c -o $@

check: build/main.js
	$(NODE) run.mjs

clean:
	rm -rf build

.PHONY: all check clean
