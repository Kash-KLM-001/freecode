CC ?= cc
CFLAGS ?= -O2
WARN = -Wall -Wextra -Wpedantic
DEFS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
STD = -std=c11
LIBS =
COMMON = util.c json.c http.c catalog.c provider.c
APP_SRC = main.c $(COMMON) config.c
MODEL_SRC = kmodels.c $(COMMON)
HEADERS = util.h json.h http.h catalog.h provider.h provider_kind.h config.h

# Host detection. The same source builds against glibc (Linux) and bionic
# (Android/Termux); the feature macros above are honored by both.
HOST_GLIBC := $(shell $(CC) -dM -E -x c /dev/null 2>/dev/null | grep -q __GLIBC__ && echo yes)

# Native bionic host: BIONIC_CC is just CC. On a glibc host set BIONIC_CC to an
# Android NDK clang wrapper, e.g. BIONIC_CC=aarch64-linux-android24-clang.
BIONIC_CC ?= $(CC)
BIONIC_CFLAGS = $(STD) $(CFLAGS) $(WARN) $(DEFS)
BIONIC_LIBS =

ifeq ($(HOST_GLIBC),yes)
# Native glibc: no sysroot or custom linker invocations needed.
GLIBC_CC = $(CC)
GLIBC_CFLAGS = $(STD) $(CFLAGS) $(WARN) $(DEFS)
GLIBC_CRT_BEGIN =
GLIBC_CRT_END =
GLIBC_LIBS = -lm
else
# Cross glibc (e.g. Termux bionic host with the glibc sysroot package).
GLIBC_ARCH := $(shell uname -m)
ifeq ($(GLIBC_ARCH),aarch64)
GLIBC_TRIPLE ?= aarch64-unknown-linux-gnu
GLIBC_DYNLINK ?= ld-linux-aarch64.so.1
else ifeq ($(GLIBC_ARCH),x86_64)
GLIBC_TRIPLE ?= x86_64-unknown-linux-gnu
GLIBC_DYNLINK ?= ld-linux-x86-64.so.2
else
GLIBC_TRIPLE ?= $(GLIBC_ARCH)-unknown-linux-gnu
GLIBC_DYNLINK ?= ld-linux-$(GLIBC_ARCH).so.1
endif
GLIBC_SYSROOT ?= $(firstword $(wildcard /data/data/com.termux/files/usr/glibc /usr/glibc))
ifeq ($(GLIBC_SYSROOT),)
GLIBC_SYSROOT := /usr
endif
GLIBC_CC ?= clang
GLIBC_LD ?= lld
GLIBC_CRT_DIR ?= $(GLIBC_SYSROOT)/lib
GLIBC_CFLAGS = --target=$(GLIBC_TRIPLE) --sysroot=$(GLIBC_SYSROOT) \
	-fuse-ld=$(GLIBC_LD) -nostdlib -fPIC $(STD) $(CFLAGS) $(WARN) $(DEFS)
GLIBC_CRT_BEGIN = $(GLIBC_CRT_DIR)/crt1.o $(GLIBC_CRT_DIR)/crti.o
GLIBC_CRT_END = $(GLIBC_CRT_DIR)/crtn.o
GLIBC_LIBS = -L$(GLIBC_CRT_DIR) -l:libc.so.6 -l:libm.so.6 \
	-Wl,-dynamic-linker,$(GLIBC_CRT_DIR)/$(GLIBC_DYNLINK) \
	-Wl,-rpath,$(GLIBC_CRT_DIR)
endif

.PHONY: all bionic glibc both kmodels libcurl models test clean

all: freecode kmodels
	@chmod +x scripts/select-free-model.sh

# Host build (whatever libc CC targets).
freecode: $(APP_SRC) $(HEADERS)
	$(CC) $(STD) $(CFLAGS) $(WARN) $(DEFS) $(APP_SRC) -o $@ $(LIBS)

kmodels: $(MODEL_SRC) $(HEADERS)
	$(CC) $(STD) $(CFLAGS) $(WARN) $(DEFS) $(MODEL_SRC) -o $@ $(LIBS)

# Explicit bionic build.
bionic: freecode-bionic kmodels-bionic
	@chmod +x scripts/select-free-model.sh

freecode-bionic: $(APP_SRC) $(HEADERS)
	$(BIONIC_CC) $(BIONIC_CFLAGS) $(APP_SRC) -o $@ $(BIONIC_LIBS)

kmodels-bionic: $(MODEL_SRC) $(HEADERS)
	$(BIONIC_CC) $(BIONIC_CFLAGS) $(MODEL_SRC) -o $@ $(BIONIC_LIBS)

# Explicit glibc build.
glibc: freecode-glibc kmodels-glibc
	@chmod +x scripts/select-free-model.sh

freecode-glibc: $(APP_SRC) $(HEADERS)
	$(GLIBC_CC) $(GLIBC_CFLAGS) $(GLIBC_CRT_BEGIN) $(APP_SRC) $(GLIBC_CRT_END) -o $@ $(GLIBC_LIBS)

kmodels-glibc: $(MODEL_SRC) $(HEADERS)
	$(GLIBC_CC) $(GLIBC_CFLAGS) $(GLIBC_CRT_BEGIN) $(MODEL_SRC) $(GLIBC_CRT_END) -o $@ $(GLIBC_LIBS)

both: bionic glibc

# Optional build that links libcurl for synchronous requests. The default
# build has no linked third-party dependencies.
libcurl: clean
	$(CC) $(STD) $(CFLAGS) $(WARN) $(DEFS) -DUSE_LIBCURL $(APP_SRC) -o freecode -lcurl
	$(CC) $(STD) $(CFLAGS) $(WARN) $(DEFS) -DUSE_LIBCURL $(MODEL_SRC) -o kmodels -lcurl
	@chmod +x scripts/select-free-model.sh

models: kmodels
	./scripts/select-free-model.sh --list 5

test: all
	./scripts/select-free-model.sh --list 1
	@printf 'build ok\n'

clean:
	rm -f freecode kmodels freecode-bionic kmodels-bionic \
		freecode-glibc kmodels-glibc freecode-asan
