#!/bin/sh
# Pull the run log and the tour snapshots the demo left on the calculator
# and convert the snapshots to PNG.   usage: tools/fetch_device.sh [label]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NLINK="$ROOT/build/nlink/nlink"
OUT="$ROOT/build/device/${1:-latest}"
mkdir -p "$OUT"
"$NLINK" get /fly/flylog.tns "$OUT/flylog.txt"
"$NLINK" ls /fly | awk '{print $NF}' | grep '^flyshot[0-9][0-9]\.tns$' | while read -r name; do
    "$NLINK" get "/fly/$name" "$OUT/${name%.tns}.rgb565" >/dev/null
done
"$ROOT/.venv/bin/python" - "$OUT" <<'PY'
import sys
from pathlib import Path

import numpy as np
from PIL import Image

out = Path(sys.argv[1])
for raw in sorted(out.glob("flyshot*.rgb565")):
    px = np.frombuffer(raw.read_bytes(), "<u2")
    if px.size != 320 * 240:
        print(f"{raw.name}: unexpected size {px.size * 2} bytes")
        continue
    px = px.reshape(320, 240).T.astype(np.uint32)  # stored one screen column per line
    rgb = np.dstack([(px >> 11) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31]).astype(np.uint8)
    Image.fromarray(rgb).resize((640, 480), Image.NEAREST).save(raw.with_suffix(".png"))
    print("converted", raw.with_suffix(".png").name)
PY
echo "--- $OUT/flylog.txt ---"
cat "$OUT/flylog.txt"
