#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Send WAV files to Mini-Grain's stored samples over USB-MIDI (SysEx F0 7D 47 52 ..; firmware/src/gr_link.c).

  grain_sample.py list
  grain_sample.py send SLOT file.wav [NAME]     SLOT 1..7: the file as mono IMA ADPCM, at most 43008 samples
  grain_sample.py erase SLOT
  grain_sample.py load SLOT PART                PART 1 or 2: load a stored sample into a part
  grain_sample.py frames SLOT file.wav OUT.txt  (no device: the SysEx frames, one hex line each, for the tests)

A file longer than a part holds (43008 samples) is resampled down to fit, to no less than 8 kHz, and cut after
that; stereo files are mixed to mono. Needs mido and python-rtmidi for the device commands. The playback pitch
follows the rate stored with the sample: a sample is at its original pitch on MIDI note 60.
"""
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sampleio as sio  # noqa: E402

HDR = [0x7D, 0x47, 0x52]
MAX_FRAMES = 43008
NSLOT = 7
ERR = {1: "bad argument", 2: "too big", 3: "flash", 4: "CRC", 5: "empty", 6: "out of order"}


def pack8to7(data):
    """8-bit bytes -> 7-bit, LSB first (ota_pack7)"""
    out, acc, nb = [], 0, 0
    for b in data:
        acc |= b << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return out


def u32(v):
    return [(v >> (7 * i)) & 0x7F for i in range(5)]


def rd32(b):
    return sum(b[i] << (7 * i) for i in range(5))


def prepare(path):
    """-> (int16 samples, rate): mono, short enough for a part"""
    sr, x = sio.read_any_wav(Path(path))
    rate = sr
    if len(x) > MAX_FRAMES:
        rate = max(8000, min(sr, int(sr * MAX_FRAMES / len(x))))
        x = sio.resample(x, sr, rate)
        x = x[:MAX_FRAMES]
    if len(x) < 64:
        raise ValueError(f"{path}: too short")
    return sio.to_int16(x), rate


def frames_for(slot, samples, rate, name):
    """the SysEx frames (the data between F0 and F7) of one upload"""
    adp, _ = sio.ima_encode(samples, 0)
    n = len(samples)
    nm = list(name.encode("ascii", "replace")[:15]) + [0] * 16
    nm = [c & 0x7F for c in nm[:16]]
    out = [HDR + [3, slot] + nm + u32(n) + u32(rate) + u32(len(adp))]
    for off in range(0, len(adp), 256):
        out.append(HDR + [4, slot] + u32(off) + pack8to7(adp[off:off + 256]))
    out.append(HDR + [5, slot] + u32(zlib.crc32(adp)))
    return out


class Link:
    def __init__(self):
        try:
            import mido
        except ImportError:
            sys.exit("needs mido: pip install mido python-rtmidi")
        self.mido = mido
        outs = [n for n in mido.get_output_names() if "amb-iguos" in n or "Mini-Grain" in n]
        ins = [n for n in mido.get_input_names() if "amb-iguos" in n or "Mini-Grain" in n]
        if not outs or not ins:
            sys.exit("no amb-iguos MIDI port found: connect the FM-1 (running amb-iguos) by USB")
        self.o = mido.open_output(outs[0])
        self.i = mido.open_input(ins[0])
        for _ in range(3):                          # the first frame after opening the port can be lost
            try:
                self.req(HDR + [1], 1.0)
                break
            except TimeoutError:
                pass

    def req(self, frame, timeout=5.0):
        for _ in self.i.iter_pending():
            pass
        self.o.send(self.mido.Message("sysex", data=frame))
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.i.iter_pending():
                if m.type == "sysex" and list(m.data[:4]) == frame[:4]:
                    return list(m.data[4:])
            time.sleep(0.002)
        raise TimeoutError(f"no reply to command {frame[3]}")


def check(r, what):
    if r[0]:
        sys.exit(f"{what} failed: {ERR.get(r[0], r[0])}")


def main():
    a = sys.argv[1:]
    if not a or a[0] not in ("list", "send", "erase", "load", "frames"):
        sys.exit(__doc__)
    if a[0] == "frames":
        samples, rate = prepare(a[2])
        Path(a[3]).write_text("\n".join(" ".join(f"{b:02x}" for b in f) for f in frames_for(int(a[1]) - 1, samples, rate, Path(a[2]).stem)) + "\n")
        return
    link = Link()
    if a[0] == "list":
        r = link.req(HDR + [2])
        check(r, "list")
        p = 1
        for k in range(NSLOT):
            used = r[p]
            name = bytes(r[p + 1:p + 17]).split(b"\0")[0].decode("ascii", "replace")
            frames, rate = rd32(r[p + 17:p + 22]), rd32(r[p + 22:p + 27])
            p += 27
            print(f"{k + 1:2}: " + (f"{name:15} {frames:6} samples at {rate} Hz ({frames / rate:.2f} s)" if used else "empty"))
        return
    slot = int(a[1]) - 1
    if not 0 <= slot < NSLOT:
        sys.exit(f"SLOT is 1..{NSLOT}")
    if a[0] == "erase":
        check(link.req(HDR + [6, slot], 10), "erase")
        print("erased")
    elif a[0] == "load":
        check(link.req(HDR + [7, slot, int(a[2]) - 1], 10), "load")
        print("loaded")
    else:
        samples, rate = prepare(a[2])
        name = a[3] if len(a) > 3 else Path(a[2]).stem
        fr = frames_for(slot, samples, rate, name)
        print(f"slot {slot + 1}: {len(samples)} samples at {rate} Hz, {len(fr) - 2} blocks")
        check(link.req(fr[0], 10), "begin")
        t0 = time.time()
        for i, f in enumerate(fr[1:-1]):
            check(link.req(f), f"write {i}")
            print(f"\r  {i + 1} / {len(fr) - 2}", end="", flush=True)
        check(link.req(fr[-1], 10), "end")
        print(f"\n  {time.time() - t0:.1f} s, stored")


if __name__ == "__main__":
    try:
        main()
    except TimeoutError as e:
        sys.exit(f"{e}: is amb-iguos running, and no other app using its MIDI port?")
