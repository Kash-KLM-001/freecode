#!/bin/sh
# Build freecode for both glibc and bionic, then show which linker each binary uses.
# Override toolchains with BIONIC_CC / GLIBC_CC / GLIBC_SYSROOT if needed.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

make -C "$ROOT" clean
make -C "$ROOT" bionic
make -C "$ROOT" glibc

file "$ROOT/freecode-bionic" "$ROOT/kmodels-bionic" \
     "$ROOT/freecode-glibc" "$ROOT/kmodels-glibc"

# Offline smoke tests; neither command needs a network connection.
"$ROOT/kmodels-bionic" --help >/dev/null
"$ROOT/kmodels-glibc" --help >/dev/null
printf 'glibc + bionic builds ok\n'
