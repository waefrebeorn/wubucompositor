#!/usr/bin/env bash
# wcc_test.sh -- compositor self-test: boot, capture, assert real desktop.
#
# Runs the compositor with --screenshot and checks the PPM came out as a
# real desktop (high color variance, dominated by the Win98 palette), not a
# mono/blank frame. Requires a DISPLAY (x11 backend).
set -u
D="${DISPLAY:-:0}"
OUT="${1:-/tmp/wcc_test.ppm}"
rm -f "$OUT" /run/user/$(id -u)/wayland-* 2>/dev/null
pkill -f 'wubucompositor$' 2>/dev/null || true
pkill -f 'hosted/wubu' 2>/dev/null || true

export DISPLAY="$D"
timeout 25 ./wubucompositor --screenshot "$OUT" >/tmp/wcc_test.log 2>&1
rc=$?
if [ ! -f "$OUT" ]; then echo "FAIL: no screenshot written (rc=$rc)"; cat /tmp/wcc_test.log; exit 1; fi

python3 - "$OUT" <<'PY' || exit 1
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load(); colors = set(); n = 0
for y in range(0, im.size[1], 5):
    for x in range(0, im.size[0], 5):
        colors.add(px[x, y]); n += 1
uniq = len(colors)

# Accept either shipped theme: the dark Win11 shell (win_face 0x2B2B2B, teal
# wallpaper) or the classic Win98 one (silver 0xC0C0C0, navy nav highlight).
dark_face = (43, 43, 43) in colors
classic_face = bool(colors & {(190,190,190),(191,191,191),(192,192,192)})
navy = bool(colors & {(0,0,128),(0,0,129)})
teal = any(r < 40 and 90 < g < 170 and 100 < b < 180
           for (r, g, b) in colors)

print(f"wcc_test: {im.size[0]}x{im.size[1]} sampled-color-count={uniq} "
      f"dark-face={'yes' if dark_face else 'no'} "
      f"classic-face={'yes' if classic_face else 'no'} "
      f"teal={'yes' if teal else 'no'} navy={'yes' if navy else 'no'}")

ok = uniq > 20 and (dark_face or classic_face) and (teal or navy)
print("wcc_test:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
