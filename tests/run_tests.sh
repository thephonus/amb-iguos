#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Host tests of Mini-Grain: the engine on the computer, the sample link on a simulated flash, the screens as PNGs.
#   tests/run_tests.sh          (cc, python3 with numpy and Pillow; the screens land in build/ui/)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/ui build/gen
python3 tools/gen_grain.py build/gen/grain_gen.h
[ -f build/gen/ui_fonts.h ] || python3 tools/gen_aa_font.py build/gen/ui_fonts.h --preset inter-tight
[ -f build/gen/ui_keycaps.h ] || python3 tools/gen_aa_keycaps.py build/gen/ui_keycaps.h
[ -f build/gen/ui_palettes.h ] || python3 tools/gen_ui_palettes.py build/gen/ui_palettes.h
CC="cc -O1 -Wall -Wno-unused-function -Wno-unused-variable -fno-tree-loop-distribute-patterns -Ifirmware/src -Ibuild/gen"
$CC tests/host_engine.c -o build/host_engine -lm
$CC tests/host_fx.c -o build/host_fx -lm
$CC tests/host_link.c -o build/host_link -lm
$CC tests/host_ui.c -o build/host_ui -lm
$CC tests/host_seq.c -o build/host_seq -lm
$CC tests/host_linkd.c -o build/host_linkd -lm
$CC tests/host_pitch.c -o build/host_pitch -lm
$CC tests/host_gendump.c -o build/host_gendump -lm
$CC tests/host_ambdump.c -o build/host_ambdump -lm
$CC tests/host_ambclick.c -o build/host_ambclick -lm
echo "== engine: parameters, generators, grains, filters"; build/host_engine
echo "== pitch (a sine sample played on notes 48..84)"
for n in 48 60 72 84; do build/host_pitch $n > build/p.raw; python3 - $n <<'PY'
import sys, numpy as np
n = int(sys.argv[1]); d = np.fromfile('build/p.raw', dtype=np.int16).astype(float)
sp = np.abs(np.fft.rfft(d * np.hanning(len(d)))); fr = np.fft.rfftfreq(len(d), 1 / 44100)
f = fr[np.argmax(sp[5:]) + 5]; want = 261.6256 * 2 ** ((n - 60) / 12)
print(f"note {n}: {f:.1f} Hz (want {want:.1f})")
sys.exit(0 if abs(f / want - 1) < 0.01 else 1)
PY
done
echo "== the tuned generators sound at C4 (261.6 Hz)"
python3 - <<'PY'
import subprocess, sys, numpy as np
def gen(t, *p):
    o = subprocess.run(['build/host_gendump', str(t), *[f'{i}={v}' for i, v in p]], capture_output=True).stdout
    return np.frombuffer(o, dtype=np.int16).astype(float)
def f0(x, lo=150, hi=520):
    seg = x[12000:12000 + 16384]; sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); fr = np.fft.rfftfreq(len(seg), 1 / 22050)
    m = (fr > lo) & (fr < hi); f = fr[m][np.argmax(sp[m])]; return f, sp[m].max() / np.sqrt((sp[m] ** 2).sum())
bad = 0
for name, t, p in [("wavetable", 7, []), ("bowed", 9, [(0, 0)]), ("flute", 9, [(0, 32768)]), ("clarinet", 9, [(0, 65535)])]:
    f, tone = f0(gen(t, *p)); cents = 1200 * np.log2(f / 261.63)
    print(f"{name:10} {f:6.1f} Hz ({cents:+.0f} cents), tone {tone:.2f}")
    bad += abs(cents) > 60 or tone < 0.3
sys.exit(1 if bad else 0)
PY
echo "== the Drone and Pad parts"; python3 tests/host_amb.py
echo "== effects"; build/host_fx
echo "== sequencer"; build/host_seq
echo "== sample link"; python3 tools/grain_sample.py frames 3 tests/tone.wav build/frames.txt; build/host_link build/frames.txt
echo "== screens"; build/host_ui build/ui; python3 tests/ui_png.py build/ui
echo "== the sample page (web/index.html), against the firmware's link"
if command -v node >/dev/null; then node web/test_link.mjs; else echo "no node: skipped"; fi
python3 web/test_page.py
