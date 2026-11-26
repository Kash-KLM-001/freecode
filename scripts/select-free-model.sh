#!/bin/sh
# Select the best currently available free, anonymous Kilo Gateway model.
# No account, no API key, and no authentication header are used.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/kmodels"

if [ ! -x "$BIN" ]; then
    make -C "$ROOT" kmodels >/dev/null
fi

exec "$BIN" "$@"
