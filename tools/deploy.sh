#!/bin/sh
# Copy the demo (and optionally the Ndless installer) to a connected
# TI-Nspire CX II.   usage: tools/deploy.sh [--with-ndless] [--program-only]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NLINK="$ROOT/build/nlink/nlink"
CALC="$ROOT/build/calc"
NDLESS="$ROOT/vendor/ndless-release/r2023"
DEST=/fly

with_ndless=0
program_only=0
for arg in "$@"; do
    case "$arg" in
        --with-ndless) with_ndless=1 ;;
        --program-only) program_only=1 ;;
        *) echo "unknown option $arg" >&2; exit 2 ;;
    esac
done

put() { # local remote
    [ -f "$1" ] || { echo "missing $1 (build it first)" >&2; exit 1; }
    "$NLINK" put "$1" "$2"
}

"$NLINK" info | sed -n '1,3p;6,7p'
"$NLINK" mkdir "$DEST" 2>/dev/null || true
if [ "$with_ndless" = 1 ]; then
    "$NLINK" mkdir /ndless 2>/dev/null || true
    put "$NDLESS/ndless_resources.tns" /ndless/ndless_resources.tns
    put "$NDLESS/ndless_installer_4.5.5-6.2.0-6.4.0.tns" /ndless/ndless_installer_4.5.5-6.2.0-6.4.0.tns
fi
put "$CALC/drosophila.tns" "$DEST/drosophila.tns"
if [ "$program_only" = 0 ]; then
    put "$CALC/flymeta.tns" "$DEST/flymeta.tns"
    put "$CALC/flycore.tns" "$DEST/flycore.tns"
    put "$CALC/flyweak.tns" "$DEST/flyweak.tns"
fi
echo "--- $DEST on the calculator ---"
"$NLINK" ls "$DEST"
[ "$with_ndless" = 1 ] && "$NLINK" ls /ndless
"$NLINK" info | grep storage
