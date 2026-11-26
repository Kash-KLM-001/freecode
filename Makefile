CC ?= cc
CFLAGS ?= -O2
WARN = -Wall -Wextra -Wpedantic
DEFS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
STD = -std=c11
LIBS =
COMMON = util.c json.c http.c catalog.c provider.c
APP_SRC = main.c $(COMMON) config.c
MODEL_SRC = kmodels.c $(COMMON)

.PHONY: all kmodels libcurl models test clean

all: freecode kmodels
	@chmod +x scripts/select-free-model.sh

freecode: $(APP_SRC) util.h json.h http.h catalog.h provider.h provider_kind.h config.h
	$(CC) $(STD) $(CFLAGS) $(WARN) $(DEFS) $(APP_SRC) -o $@ $(LIBS)

kmodels: $(MODEL_SRC) util.h json.h http.h catalog.h provider.h provider_kind.h
	$(CC) $(STD) $(CFLAGS) $(WARN) $(DEFS) $(MODEL_SRC) -o $@ $(LIBS)

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
	rm -f freecode kmodels freecode-asan
