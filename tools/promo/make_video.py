#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mini-Grain presentation video: the real engine's audio and the real screens (tools/promo/host_demo_video.c), titles,
captions and a spectrum, put together with ffmpeg.

    python3 tools/promo/make_video.py [out.mp4] [1|2]   (needs cc, numpy, Pillow, ffmpeg; about two minutes to render)
    1: the tour (default).  2: the ambient parts and a demo song.  3: the demo song with a long VA sound, then the granular engine
"""
import os, subprocess, sys, wave
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.path.join(ROOT, "build", "promo")
STORY = int(sys.argv[2]) if len(sys.argv) > 2 else 1
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", {1: "mini-grain-presentation.mp4", 2: "mini-grain-ambient.mp4", 3: "mini-grain-song-granular.mp4"}[STORY])
FS, FPS, W, H = 44100, 30, 1280, 720
FONT = os.path.join(ROOT, "assets", "fonts", "InterTight[wght].ttf")

BG = (14, 13, 22)
TEXT = (238, 232, 222)
MUTED = (150, 144, 160)
ACC = {1: (226, 190, 128), 2: (120, 205, 205), 3: (186, 160, 235), 4: (150, 224, 180)}


def font(size, weight=500):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([weight])
    except Exception:
        pass
    return f


def build():
    """compile and run the host programs: one audio file and one frame file per scene"""
    cc = ["cc", "-O1", "-Wno-parentheses", "-I" + os.path.join(ROOT, "firmware/src"), "-I" + os.path.join(ROOT, "build/gen"),
          "-I" + os.path.join(ROOT, "tools/promo")]
    os.makedirs(WORK, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools/gen_grain.py"), os.path.join(ROOT, "build/gen/grain_gen.h")], check=True, stdout=subprocess.DEVNULL)
    exe = os.path.join(ROOT, "build", "host_demo_video")
    subprocess.run(cc + [os.path.join(ROOT, "tools/promo/host_demo_video.c"), "-o", exe, "-lm"], check=True)
    jobs = []
    for sg in SEGS:
        t = sg["tag"]
        j = ("gen", int(t[3:])) if t.startswith("gen") else (t, 0)
        if j not in jobs:
            jobs.append(j)
    for name, arg in jobs:
        tag = name if name != "gen" else "gen%d" % arg
        a, v = os.path.join(WORK, "v_%s.raw" % tag), os.path.join(WORK, "v_%s.rgb" % tag)
        if not (os.path.exists(a) and os.path.exists(v)) or os.path.getmtime(a) < os.path.getmtime(exe):
            subprocess.run([exe, name, str(arg), a, v], check=True)


def load_audio(tag):
    d = np.fromfile(os.path.join(WORK, "v_%s.raw" % tag), dtype=np.int16).astype(np.float64) / 32768.0
    return d.reshape(-1, 2)


def load_frames(tag):
    return np.fromfile(os.path.join(WORK, "v_%s.rgb" % tag), dtype=np.uint8).reshape(-1, 240, 240, 3)


GENS = [("VA", "Virtual analogue: two oscillators, filter and envelope"), ("Vowel", "Formant voice: vowels that morph"),
        ("Pluck", "Karplus-Strong strings, with patterns"), ("FM", "Two-operator FM bells and tones"),
        ("Additive", "Additive partials"), ("Texture", "Noise textures and clouds"), ("FX", "Sound-effect material"),
        ("Wavetable", "Wavetable scans"), ("Drums", "Synthesised drums and hits"), ("Bow / Wind", "Bowed and blown physical models"),
        ("Dust", "Dust, crackle and sparse grains")]

# (tag, start in the scene, length, accent part, [(from, heading, [lines])], audio gain cap in dB, fade in/out)
SEGS = []


def seg(tag, start, length, acc, caps, fade=(0.15, 0.2)):
    SEGS.append(dict(tag=tag, start=start, length=length, acc=acc, caps=caps, fade=fade))


def story1():
    seg("drone", 2.0, 4.5, 3, [(0, "Mini-Grain", ["An ambient granular firmware", "for the M-Vave FM1"])], fade=(1.2, 0.4))
    seg("granular", 0.5, 14.0, 1, [
        (0, "Granular engine", ["Two parts, each with its own sound", "Position, Size, Density, Spread, Scan"]),
        (5, "Play the sound as a cloud", ["Up to dozens of grains at once,", "shaped by jitter, pitch and pan"]),
        (8, "Delay and reverb", ["A ping-pong delay and a plate reverb", "shared by every part"]),
        (11, "Filter and two LFOs", ["24 LFO shapes, five filter types,", "any parameter as a destination"])], fade=(0.4, 0.8))
    for i, (n, d) in enumerate(GENS):
        seg("gen%d" % i, 0.0, 2.6, 1, [(0, "Sound generators  %d/11" % (i + 1), [n, d])], fade=(0.04, 0.25))
    seg("drone", 0.0, 10.0, 3, [
        (0, "Part 3: Drone", ["Four voices of morphing wavetable", "oscillators, in two structures"]),
        (5.5, "Harmonic morph", ["32 waves, a harmonic position", "that moves, detune and balance"])], fade=(0.6, 0.8))
    seg("pad", 0.0, 10.0, 4, [
        (0, "Part 4: Pad", ["Wide, slow, three oscillators", "a voice, with two structures"]),
        (5.5, "Voice modes", ["Poly, Mono, Legato and Unison,", "glide, pan and modulation shapes"])], fade=(0.6, 0.8))
    seg("seq", 0.0, 27.0, 1, [
        (0, "Sequencer", ["64 steps a part, each part with its", "own length and division"]),
        (7, "Gliding parameter locks", ["A lock can glide to the next one:", "sweeps over bars, even recorded by hand"]),
        (11, "Long steps", ["Divisions up to 8 bars, dotted and triplets,", "gates to 400 %, the cycle time on screen"]),
        (15, "A drone that follows the chords", ["Key hold and a release that dies away", "or cuts: your choice"]),
        (20, "Four parts, layered", ["Granular, granular, drone and pad", "in one preset of sixteen"])], fade=(0.3, 2.0))
    seg("pad", 11.0, 5.0, 2, [(0, "Open source", ["GPL-3.0, built on Felucca's Mini-Tone", "16 presets, USB and TRS MIDI, M-UPGRADE update"])], fade=(0.4, 2.0))




def story2():
    seg("dronedemo", 8.0, 5.0, 3, [(0, "Ambient parts", ["The drone and the pad of Mini-Grain,", "and a song made of the four parts"])], fade=(1.2, 0.3))
    seg("dronedemo", 0.0, 24.0, 3, [
        (0, "Part 3: Drone", ["Four voices of wavetable oscillators:", "a held chord, the Harmonic morphing"]),
        (6, "Structure 2", ["Two structures change how the", "oscillators share the sound"]),
        (12, "Unison", ["Voice mode Unison: four copies a note,", "spread in the stereo field"]),
        (18, "Modulation", ["A MOD LFO with 24 shapes moves", "the detune and the harmonics"])], fade=(0.3, 0.5))
    seg("paddemo", 0.0, 24.0, 4, [
        (0, "Part 4: Pad", ["Three oscillators a voice, wide and slow", "a chord that breathes"]),
        (6, "Structure 2", ["The second structure: two sounds in", "one group, one in the other"]),
        (12, "Unison", ["Unison, with the copies drifting apart", "as the detune opens"]),
        (18, "Modulation", ["Slow shapes on the harmonics:", "the chord keeps moving"])], fade=(0.3, 0.5))
    seg("granular", 0.5, 12.0, 1, [
        (0, "And the granular parts", ["Two parts that cut a sound into grains:", "position, size, density, scan"]),
        (6, "Eleven sound generators", ["Or load your own samples over USB", "from the web page"])], fade=(0.3, 0.6))
    seg("song", 0.0, 94.0, 3, [
        (0, "A demo song", ["Played by the sequencer, four parts", "A minor, 60 BPM, four bars a chord"]),
        (8, "Gliding locks", ["The Harmonic glides from one lock to the next:", "a sweep over thirty-two bars"]),
        (16, "The Pad joins", ["Part 4 takes the chords an octave and", "a half above, wide and slow"]),
        (32, "A granular texture", ["Part 2 scans a noise texture,", "its position drifting by itself"]),
        (46, "A sparse pluck", ["Part 1 plays a 32-step pattern", "with a size lock that glides"]),
        (58, "The Tools page", ["Locks, glide and cycle time", "on the part's own steps"]),
        (68, "Everything together", ["Delay opens on the pluck, the pad", "changes wave and grows"]),
        (84, "It ends by itself", ["The sequencer stops; Release lets the", "voices die away into the reverb"])], fade=(0.5, 3.0))


def story3():
    seg("songva", 0.0, 94.0, 3, [
        (0, "A demo song", ["Played by the sequencer, four parts", "A minor, 60 BPM, four bars a chord"]),
        (8, "Gliding locks", ["The Harmonic glides from one lock to the next:", "a sweep over thirty-two bars"]),
        (16, "The Pad joins", ["Part 4 takes the chords an octave and", "a half above, wide and slow"]),
        (32, "A granular texture", ["Part 2 scans a noise texture,", "its position drifting by itself"]),
        (46, "A long VA sound", ["Part 1: a generated VA sound, two seconds", "of slowly closing filter, long grains"]),
        (58, "The Tools page", ["Locks, glide and cycle time", "on the part's own steps"]),
        (68, "Everything together", ["Delay opens on part 1, the pad", "changes wave and grows"]),
        (84, "It ends by itself", ["The sequencer stops; Release lets the", "voices die away into the reverb"])], fade=(0.5, 3.0))
    seg("gransh", 0.0, 57.0, 1, [
        (0, "The granular engine", ["One generated sound, one held chord:", "Position picks where the grains are taken"]),
        (6, "Size", ["Tiny grains buzz, long ones melt", "together into a smooth tone"]),
        (12, "Density", ["From sparse sparkles to a dense cloud"]),
        (18, "Spread and jitter", ["Grains scatter across the stereo", "field, and in time"]),
        (24, "Pitch random", ["Every grain gets its own random pitch"]),
        (29, "Scan", ["The position moves through the", "sound by itself"]),
        (34, "Reverse", ["A share of the grains play backwards"]),
        (38, "Filter and LFO", ["An LFO sweeps the filter cutoff"]),
        (44, "Another generator", ["Vowel: the same engine, a choir"]),
        (50, "Release", ["The chord lets go,", "the reverb carries on"])], fade=(0.4, 3.5))


{1: story1, 2: story2, 3: story3}[STORY]()


def seg_audio(s):
    a = load_audio(s["tag"])
    i0, i1 = int(s["start"] * FS), int((s["start"] + s["length"]) * FS)
    x = a[i0:i1].copy()
    n = len(x)
    # level: the quiet scenes up to a common loudness, never more than +12 dB
    rms = np.sqrt((x ** 2).mean()) + 1e-9
    g = min(10 ** (12 / 20), 10 ** (-19 / 20) / rms)
    x *= g
    fi, fo = int(s["fade"][0] * FS), int(s["fade"][1] * FS)
    env = np.ones(n)
    if fi: env[:fi] = np.linspace(0, 1, fi) ** 2
    if fo: env[-fo:] = np.linspace(1, 0, fo) ** 2
    return x * env[:, None]


def make_audio():
    parts = [seg_audio(s) for s in SEGS]
    x = np.concatenate(parts)
    x = np.tanh(x * 1.1) / np.tanh(1.1) * 0.89                                       # a gentle limiter
    pk = np.abs(x).max()
    if pk > 0.89:
        x *= 0.89 / pk
    path = os.path.join(WORK, "audio.wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes((x * 32767).astype(np.int16).tobytes())
    return x, path


def fit(d, text, size, weight, width):
    while size > 14:
        f = font(size, weight)
        if d.textlength(text, font=f) <= width:
            return f
        size -= 1
    return font(14, weight)


def caption_layer(head, lines, acc):
    im = Image.new("RGBA", (520, 300), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((0, 8, 6, 62), fill=acc)
    d.text((22, 0), head, font=fit(d, head, 46, 650, 490), fill=TEXT)
    y = 82
    for ln in lines:
        d.text((22, y), ln, font=fit(d, ln, 26, 450, 490), fill=MUTED)
        y += 38
    return im


def background():
    g = np.zeros((H, W, 3), np.uint8)
    for c in range(3):
        g[:, :, c] = np.linspace(BG[c] + 8, BG[c] - 4, H)[:, None]
    return Image.fromarray(g)


def spectrum(mono, t, prev):
    n = 2048
    c = int(t * FS)
    w = mono[max(0, c - n // 2): c + n // 2]
    if len(w) < n:
        w = np.pad(w, (0, n - len(w)))
    sp = np.abs(np.fft.rfft(w * np.hanning(n))) / n
    fr = np.fft.rfftfreq(n, 1 / FS)
    edges = np.geomspace(70, 12000, 49)
    v = np.array([sp[(fr >= edges[i]) & (fr < edges[i + 1])].max(initial=0) for i in range(48)])
    v = np.clip((20 * np.log10(v + 1e-7) + 78) / 52, 0, 1)
    return np.maximum(v, prev * 0.86)


def main():
    build()
    audio, apath = make_audio()
    mono = audio.mean(axis=1)
    total = len(audio) / FS
    nframes = int(total * FPS)
    print("video: %.1f s, %d frames" % (total, nframes))
    bg = background()
    ffm = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", str(FPS), "-i", "-",
                            "-i", apath, "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
                            "-shortest", OUT], stdin=subprocess.PIPE)
    starts, t0 = [], 0.0
    for s in SEGS:
        starts.append(t0)
        t0 += s["length"]
    cache = {}
    prev = np.zeros(48)
    panel = Image.new("RGBA", (640, 640), (0, 0, 0, 0))
    ImageDraw.Draw(panel).rounded_rectangle((0, 0, 639, 639), radius=26, fill=(26, 24, 38, 255), outline=(52, 48, 72, 255), width=2)
    small, brand = font(20, 600), font(20, 600)
    for f in range(nframes):
        t = f / FPS
        k = max(i for i in range(len(SEGS)) if starts[i] <= t + 1e-6)
        s = SEGS[k]
        lt = t - starts[k]
        key = s["tag"]
        if key not in cache:
            cache = {key: load_frames(key)}
        frames = cache[key]
        fi = min(int((s["start"] + lt) * FPS), len(frames) - 1)
        acc = ACC[s["acc"]]
        im = bg.copy().convert("RGBA")
        im.alpha_composite(panel, (610, 40))
        ui = Image.fromarray(frames[fi]).resize((600, 600), Image.LANCZOS).convert("RGBA")
        im.alpha_composite(ui, (630, 60))
        # the caption of the moment, fading in
        ci = max(i for i, c in enumerate(s["caps"]) if c[0] <= lt + 1e-6)
        cstart, head, lines = s["caps"][ci]
        ck = (k, ci)
        if ck not in cache.get("_caps", {}):
            cache.setdefault("_caps", {})[ck] = caption_layer(head, lines, acc)
        layer = cache["_caps"][ck].copy()
        a = min(1.0, (lt - cstart) / 0.45)
        layer.putalpha(layer.getchannel("A").point(lambda v, a=a: int(v * a)))
        im.alpha_composite(layer, (60, 170 + int((1 - a) * 14)))
        d = ImageDraw.Draw(im)
        d.text((60, 56), "MINI-GRAIN", font=brand, fill=acc)
        d.text((60, 82), "M-Vave FM1 firmware", font=small, fill=MUTED)
        # the spectrum
        prev = spectrum(mono, t, prev)
        for i in range(48):
            h = int(6 + prev[i] * 150)
            x = 60 + i * 11
            d.rounded_rectangle((x, 660 - h, x + 7, 660), radius=3, fill=tuple(int(c * (0.45 + 0.55 * prev[i])) for c in acc))
        # progress
        d.rectangle((0, H - 4, int(W * t / total), H), fill=acc)
        ffm.stdin.write(im.convert("RGB").tobytes())
    ffm.stdin.close()
    ffm.wait()
    print("wrote", OUT)


if __name__ == "__main__":
    main()
