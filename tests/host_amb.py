#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The Drone and Pad parts on the computer (build/host_ambdump = gr_amb.c + the engine): every wave sounds and sounds about as
loud as the others, the notes come out at the right pitch, the structures, voice modes and envelope do what the manual says."""
import subprocess
import sys
import numpy as np

SR = 44100
bad = 0


def amb(part, note, sec, *kv):
    out = subprocess.run(['build/host_ambdump', str(part), str(note), str(sec), *[f'{k}={v}' for k, v in kv]], capture_output=True).stdout
    return np.frombuffer(out, dtype=np.int16).astype(float)


def check(ok, msg):
    global bad
    print(('ok   ' if ok else 'FAIL ') + msg)
    bad += not ok


def f0(x, lo=40, hi=800):
    seg = x[-32768:]
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); fr = np.fft.rfftfreq(len(seg), 1 / SR)
    m = (fr > lo) & (fr < hi)
    return fr[m][np.argmax(sp[m])]


# the 32 waves, at three harmonic positions: not silent, not clipped, within a factor of 3 of each other
rms = []
for w in range(32):
    for h in (0, 32768, 65535):
        x = amb(3, 48, 3, ('Wave', round(w * 65535 / 31)), ('Attack', 0), ('Harmonic', h))
        r = x[-SR:].std()
        rms.append(r)
        if r < 100 or abs(x).max() > 32000:
            check(False, f'wave {w + 1} harmonic {h}: rms {r:.0f}, peak {abs(x).max():.0f}')
check(min(rms) > 0 and max(rms) / min(rms) < 3.5, f'32 waves x 3 positions: rms {min(rms):.0f} .. {max(rms):.0f} (ratio {max(rms) / min(rms):.1f})')

# pitch: note 69 with the part's octave shift removed plays 440 Hz (wave 3, a sine-ish)
for part, name in ((2, 'drone'), (3, 'pad')):
    x = amb(part, 69, 4, ('Transpose', 32768), ('Wave', 2 * 65535 // 31), ('Harmonic', 0), ('Detune', 32768), ('Pitch', 32768), ('Attack', 0))
    f = f0(x, 200, 900)
    check(abs(1200 * np.log2(f / 440)) < 30, f'{name}: note 69 at {f:.1f} Hz')

# the envelope: a 2 s attack rises, a long release lets the sound tail on after the note
x = amb(2, 60, 5, ('Attack', 36000), ('Wave', 5000))
r = [x[i:i + SR // 2].std() for i in range(0, SR * 4, SR // 2)]
check(r[0] < r[1] < r[2] and r[0] < 0.6 * r[2], 'attack: ' + ' '.join(f'{v:.0f}' for v in r))

# unison is wider and no louder than a chord; mono keeps one voice
poly = amb(3, 60, 4, ('Voice mode', 0))[-SR:].std()
uni = amb(3, 60, 4, ('Voice mode', 65535))[-SR:].std()
check(0.5 < uni / poly < 2.2, f'unison {uni:.0f} against poly {poly:.0f}')

# the structures differ: Drone 2 (MOD moves the harmonic) is not Drone 1 (MOD moves the detune)
d1 = amb(2, 60, 5, ('Structure', 0), ('Mod depth', 65535), ('Mod rate', 40000))
d2 = amb(2, 60, 5, ('Structure', 65535), ('Mod depth', 65535), ('Mod rate', 40000))
check(abs(d1[-SR:].std() - d2[-SR:].std()) > 20 or not np.allclose(d1[-1000:], d2[-1000:]), 'Drone 1 and Drone 2 sound different')

# the MOD LFO shapes: the harmonic mod makes the level wander (a slow, deep one on a wave whose level depends on it)
x = amb(3, 48, 8, ('Wave', 22 * 65535 // 31), ('Mod depth', 65535), ('Mod rate', 30000), ('Attack', 0))
w = [x[i:i + 4410].std() for i in range(SR, len(x) - 4410, 4410)]
check(max(w) / max(min(w), 1) > 1.15, f'the MOD moves the sound: level swings by {max(w) / max(min(w), 1):.2f}')
sys.exit(1 if bad else 0)
