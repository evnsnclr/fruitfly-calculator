#!/bin/sh
# Wait for the calculator to appear on USB, save the log of its last run,
# then install the current program and metadata and verify them.
#   usage: tools/sync_when_connected.sh [label] [max-wait-seconds]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NLINK="$ROOT/build/nlink/nlink"
CALC="$ROOT/build/calc"
OUT="$ROOT/build/device/${1:-previous-run}"
MAX="${2:-21600}"
mkdir -p "$OUT"
waited=0
until "$NLINK" info >/dev/null 2>&1; do
    waited=$((waited + 5))
    [ "$waited" -ge "$MAX" ] && { echo "calculator did not appear on USB within $MAX s"; exit 1; }
    sleep 5
done
echo "calculator connected after ${waited}s"
"$NLINK" info | grep -E "^os|^ram|^storage"
if "$NLINK" get /fly/flylog.tns "$OUT/flylog.txt" >/dev/null 2>&1; then
    echo "=== log of the previous run ==="
    cat "$OUT/flylog.txt"
else
    echo "no flylog.tns on the calculator (the program never wrote one)"
fi
echo "=== installing current build ==="
status=0
for f in drosophila flymeta; do
    "$NLINK" put "$CALC/$f.tns" "/fly/$f.tns" || status=1
    "$NLINK" get "/fly/$f.tns" "$OUT/$f.readback" >/dev/null 2>&1 && cmp -s "$CALC/$f.tns" "$OUT/$f.readback" \
        && echo "$f.tns verified on calculator" || { echo "$f.tns verification FAILED"; status=1; }
done
"$NLINK" ls /fly
"$NLINK" ls /ndless 2>/dev/null
exit $status
