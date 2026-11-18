#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate build/gen/grain_gen.h for Mini-Grain: the lookup tables of the fixed-point granular engine and the
parameter table (one source of truth: the list PARAMS below).

The Granular synth (ESP32) keeps every parameter as a float 0..1 "norm" plus a real value.  The FM-1 has no FPU, so
here a parameter is a Q16 norm (0..65535) and its real value an integer scaled by `mul` (Size 5..1000 ms with
mul 16 is 80..16000).  Exp curves are min * 2^(norm * log2(max/min)).
"""
import math
import sys

FS = 44100

# name, label, min, max, default, curve(L/E), unit, steps, cc, mul, [choice labels]
# units: pct (shown as 100*v/max %), ms, hz, persec, speed, semi, plain, bipolar, choice, dest, chan, whole
LFO_SHAPES = ['Sine', 'Square', 'Triangle', 'Saw', 'R.Saw', 'Random', 'Smooth rnd', 'Log', 'R.Log', 'Pulse 10', 'Pulse 25', 'Pulse 75', 'Pulse 90', 'Steps 2', 'Steps 3', 'Steps 4', 'Steps 5', 'Steps 6', 'Steps 7', 'Ramp up', 'Ramp down', 'Slow sine', 'Slow tri', 'Slow smooth']

# the step lengths of the sequencer: label, ticks (1536 to the beat), triplet. From eight bars (as the Ambient O's
# sequencer) down to a 32nd.
DIVS = [("8 bars", 49152, 0), ("4 bars", 24576, 0), ("3 bars", 18432, 0), ("2 bars", 12288, 0), ("1 bar", 6144, 0),
        ("1/2", 3072, 0), ("1/4.", 2304, 0), ("1/2T", 2048, 1), ("1/4", 1536, 0), ("1/8.", 1152, 0), ("1/4T", 1024, 1),
        ("1/8", 768, 0), ("1/16.", 576, 0), ("1/8T", 512, 1), ("1/16", 384, 0), ("1/16T", 256, 1), ("1/32", 192, 0)]

PARAMS = [
    # ---- per part: grain
    ("POSITION", "Position", 0, 1, .25, "L", "pct", 0, 1, 1000),
    ("SIZE", "Size", 5, 1000, 100, "E", "ms", 0, 17, 16),
    ("DENSITY", "Density", 1, 200, 40, "E", "persec", 0, 18, 256),
    ("SPREAD", "Spread", 0, 1, .05, "L", "pct", 0, 70, 1000),
    ("SCAN", "Scan", -2, 2, 0, "L", "speed", 0, 75, 1000),
    ("JITTER", "Jitter", 0, 1, .3, "L", "pct", 0, 76, 1000),
    ("PITCHRND", "Pitch rnd", 0, 12, 0, "L", "semi", 0, 77, 256),
    ("PAN", "Pan", 0, 1, .5, "L", "pct", 0, 78, 1000),
    # ---- voice
    ("ATTACK", "Attack", 1, 5000, 20, "E", "ms", 0, 73, 16),
    ("RELEASE", "Release", 5, 8000, 400, "E", "ms", 0, 72, 16),
    ("REVERSE", "Reverse", 0, 1, 0, "L", "pct", 0, 79, 1000),
    ("TRANSPOSE", "Transpose", -24, 24, 0, "L", "semi", 48, 255, 256),
    ("LEVEL", "Level", 0, 1, .8, "L", "pct", 0, 7, 1000),
    ("VOLUME", "Master", 0, 1, .7, "L", "pct", 0, 24, 1000),
    # ---- effects
    ("DLYTIME", "Dly time", 10, 700, 375, "E", "ms", 0, 12, 16),
    ("DLYFB", "Dly fdbk", 0, .9, .45, "L", "pct", 0, 13, 1000),
    ("DLYDAMP", "Dly damp", 0, 1, .3, "L", "pct", 0, 14, 1000),
    ("DLYSEND", "Dly send", 0, 1, .2, "L", "pct", 0, 93, 1000),
    ("REVDECAY", "Rev decay", .1, .95, .7, "L", "plain", 0, 15, 1000),
    ("REVDAMP", "Rev damp", 0, .9, .35, "L", "pct", 0, 16, 1000),
    ("REVSEND", "Rev send", 0, 1, .25, "L", "pct", 0, 91, 1000),
    # ---- filter, LFOs
    ("FTYPE", "Filter", 0, 4, 0, "L", "choice", 4, 255, 1, ["Off", "Vintage 24", "Res LP", "Res BP", "Res HP"]),
    ("CUTOFF", "Cutoff", 30, 18000, 2000, "E", "hz", 0, 74, 16),
    ("RESO", "Reso", 0, 1, .25, "L", "pct", 0, 71, 1000),
    ("DRIVE", "Drive", 0, 1, .2, "L", "pct", 0, 19, 1000),
    ("L1WAVE", "L1 wave", 0, 23, 0, "L", "choice", 23, 255, 1, LFO_SHAPES),
    ("L1RATE", "L1 rate", .01, 40, .5, "E", "hz", 0, 20, 256),
    ("L1DEPTH", "L1 depth", -1, 1, .3, "L", "bipolar", 0, 21, 1000),
    ("L1DEST", "L1 dest", 0, 28, 0, "L", "dest", 28, 255, 1),
    ("L2WAVE", "L2 wave", 0, 23, 2, "L", "choice", 23, 255, 1, LFO_SHAPES),
    ("L2RATE", "L2 rate", .01, 40, .15, "E", "hz", 0, 22, 256),
    ("L2DEPTH", "L2 depth", -1, 1, .3, "L", "bipolar", 0, 23, 1000),
    ("L2DEST", "L2 dest", 0, 28, 0, "L", "dest", 28, 255, 1),
    ("MIDICH", "MIDI ch", 1, 16, 1, "L", "chan", 15, 255, 1),
    # ---- the sequencer: per part, then shared
    ("SEQLEN", "Length", 1, 64, 16, "L", "num", 63, 255, 1),
    ("SEQDIV", "Division", 0, len(DIVS) - 1, [d[0] for d in DIVS].index("1/16"), "L", "choice", len(DIVS) - 1, 255, 1,
     [d[0] for d in DIVS]),
    ("SEQSWING", "Track swing", 0, 100, 0, "L", "pct", 0, 255, 1),
    ("SEQGATE", "Gate", 5, 400, 80, "L", "num", 0, 255, 1),
    ("TEMPO", "Tempo", 20, 300, 120, "L", "num", 280, 255, 1),
    ("SEQGSWING", "Swing", 0, 100, 0, "L", "pct", 0, 255, 1),
    ("SEQCLICK", "Click", 0, 2, 1, "L", "choice", 2, 255, 1, ["Off", "Recording", "On"]),
    ("SEQRECSTART", "Rec start", 0, 2, 0, "L", "choice", 2, 255, 1, ["First note", "Count-in", "Step"]),
    ("SEQDELAY", "Rec delay", 0, 10, 3, "L", "num", 10, 255, 1),
    ("SEQSTOP", "Stop", 0, 1, 0, "L", "choice", 1, 255, 1, ["Release", "Cut"]),
    ("LATCH", "Key hold", 0, 1, 0, "L", "choice", 1, 255, 1, ["Off", "Latch"]),
    # ---- the Drone and Pad parts (3 and 4): after the layers of the Sonicware LIVEN Ambient O
    ("AWAVE", "Wave", 1, 32, 1, "L", "num", 31, 255, 1),
    ("AHARM", "Harmonic", 0, 127, 32, "L", "num", 127, 255, 1),
    ("ASTRUCT", "Structure", 0, 1, 0, "L", "choice", 1, 255, 1, ["1", "2"]),
    ("APITCH", "Pitch", -24, 24, 0, "L", "semi", 480, 255, 10),
    ("ADETUNE", "Detune", -50, 50, 8, "L", "num", 100, 255, 1),
    ("ABAL", "Balance", -63, 63, 0, "L", "num", 126, 255, 1),
    ("AMODRATE", "Mod rate", .02, 20, .4, "E", "hz", 0, 255, 1000),
    ("AMODDEPTH", "Mod depth", 0, 1, .3, "L", "pct", 0, 255, 1000),
    ("AMODSHAPE", "Mod shape", 0, 23, 0, "L", "choice", 23, 255, 1, LFO_SHAPES),
    ("AVOICE", "Voice mode", 0, 3, 0, "L", "choice", 3, 255, 1, ["Poly", "Mono", "Legato", "Unison"]),
    ("AGLIDE", "Glide", 0, 10000, 0, "L", "ms", 0, 255, 1),
    ("AUNIDET", "Uni detune", 0, 20, 8, "L", "num", 20, 255, 1),
    ("AATK", "Attack", 1, 10000, 2000, "E", "ms", 0, 255, 16),
    ("AREL", "Release", 5, 20000, 4000, "E", "ms", 0, 255, 16),
    ("APAN", "Pan", 0, 1, .5, "L", "pan", 0, 255, 1000),
    ("SEQQUANT", "Quantize", 0, 1, 1, "L", "choice", 1, 255, 1, ["Off", "On"]),
    ("SEQSYNC", "Sync", 0, 1, 0, "L", "choice", 1, 255, 1, ["Internal", "MIDI clock"]),
    ("SEQOUT", "MIDI out", 0, 2, 0, "L", "choice", 2, 255, 1, ["Off", "Notes", "Notes+clock"]),
]
SFX = {"TEMPO": " BPM", "SEQGATE": "%", "SEQDELAY": " s", "APITCH": "", "ADETUNE": " ct", "AUNIDET": " ct"}

# what an LFO can control (index 0 = Off); the same list as the Granular synth
LFO_DESTS = ["NONE", "CUTOFF", "RESO", "DRIVE", "POSITION", "SIZE", "DENSITY", "SPREAD", "SCAN", "JITTER", "PITCHRND",
             "PAN", "REVERSE", "TRANSPOSE", "ATTACK", "RELEASE", "LEVEL", "VOLUME", "DLYTIME", "DLYFB", "DLYDAMP",
             "DLYSEND", "REVDECAY", "REVDAMP", "REVSEND", "L1RATE", "L1DEPTH", "L2RATE", "L2DEPTH"]

# what an LFO of the Drone and Pad parts can control (padded with Off to the same length)
ALFO_DESTS = ["NONE", "APITCH", "AHARM", "ABAL", "ADETUNE", "L1RATE", "L1DEPTH", "AMODRATE", "AMODDEPTH", "CUTOFF", "RESO",
              "APAN", "LEVEL", "REVSEND", "DLYSEND", "ATRANS"]
# the MIDI CCs of the Drone and Pad parts (the shared parameters keep theirs)
ACC = [(73, "AATK"), (72, "AREL"), (74, "CUTOFF"), (71, "RESO"), (19, "DRIVE"), (7, "LEVEL"), (10, "APAN"), (85, "AWAVE"),
       (86, "AHARM"), (87, "APITCH"), (88, "ADETUNE"), (89, "ABAL"), (90, "AMODRATE"), (92, "AMODDEPTH"), (94, "AGLIDE"),
       (20, "L1RATE"), (21, "L1DEPTH"), (22, "L2RATE"), (23, "L2DEPTH"), (12, "DLYTIME"), (13, "DLYFB"), (14, "DLYDAMP"),
       (15, "REVDECAY"), (16, "REVDAMP"), (24, "VOLUME"), (91, "REVSEND"), (93, "DLYSEND")]


# sample generators: label, min, max, default, curve, unit, steps, mul, suffix[, choice labels]
WAVES = ["Saw", "Square", "Triangle", "Sine"]
PATTERNS = ["Single", "Octaves", "Major", "Minor", "Pentatonic"]
VARIATION = ("Variation", 0, 99, 1, "L", "num", 99, 1, "")
GENS = [
    ("VA", 1, [
        ("Wave 1", 0, 3, 0, "L", "choice", 3, 1, "", WAVES), ("Wave 2", 0, 3, 1, "L", "choice", 3, 1, "", WAVES),
        ("Interval", -24, 24, 7, "L", "num", 48, 1, " st"), ("Detune", 0, 50, 8, "L", "num", 0, 1, " ct"),
        ("Osc mix", 0, 1, .45, "L", "num", 0, 1000, ""), ("Cutoff", 60, 16000, 1800, "E", "hz", 0, 1, ""),
        ("Reso", 0, 1, .3, "L", "num", 0, 1000, ""), ("Filt env", -1, 1, .35, "L", "num", 0, 1000, "")]),
    ("Vowel", 1, [
        ("Vowel 1", 0, 4, 0, "L", "choice", 4, 1, "", ["A", "E", "I", "O", "U"]),
        ("Vowel 2", 0, 4, 2, "L", "choice", 4, 1, "", ["A", "E", "I", "O", "U"]),
        ("Morphs", 0, 8, 1, "L", "num", 16, 1000, ""), ("Formant", .7, 1.5, 1, "L", "num", 0, 1000, "x"),
        ("Vibrato", 0, 1, .3, "L", "num", 0, 1000, ""), ("Breath", 0, 1, .1, "L", "num", 0, 1000, ""),
        ("Bright", 0, 1, .5, "L", "num", 0, 1000, ""), VARIATION]),
    ("Pluck", 0, [
        ("Pattern", 0, 4, 4, "L", "choice", 4, 1, "", PATTERNS), ("Notes", 1, 16, 8, "L", "num", 15, 1, ""),
        ("Sustain", 0, 1, .8, "L", "num", 0, 1000, ""), ("Bright", 0, 1, .7, "L", "num", 0, 1000, ""),
        ("Damping", 0, 1, .4, "L", "num", 0, 1000, ""), VARIATION]),
    ("FM", 0, [
        ("Pattern", 0, 4, 0, "L", "choice", 4, 1, "", PATTERNS), ("Notes", 1, 16, 4, "L", "num", 15, 1, ""),
        ("Ratio", .5, 9, 3.5, "L", "num", 34, 1000, "x"), ("Index", 0, 12, 4, "L", "num", 0, 1000, ""),
        ("Index env", 0, 1, .7, "L", "num", 0, 1000, ""), ("Feedback", 0, 1, .1, "L", "num", 0, 1000, ""),
        ("Decay", .02, 1, .5, "E", "num", 0, 1000, "")]),
    ("Additive", 1, [
        ("Partials", 2, 32, 16, "L", "num", 30, 1, ""), ("Tilt", .3, 3, 1, "L", "num", 0, 1000, ""),
        ("Even", 0, 1, .7, "L", "num", 0, 1000, ""), ("Stretch", 1, 1.05, 1, "L", "num", 0, 1000, "x"),
        ("Motion", 0, 1, .5, "L", "num", 0, 1000, ""), VARIATION]),
    ("Texture", 1, [
        ("Color", 100, 8000, 700, "E", "hz", 0, 1, ""), ("Width", 0, 1, .5, "L", "num", 0, 1000, ""),
        ("Sweep", 0, 1, .5, "L", "num", 0, 1000, ""), ("Speed", .02, 2, .15, "E", "hz", 0, 1000, ""),
        ("Hum", 0, 1, .3, "L", "num", 0, 1000, ""), ("Crackle", 0, 1, .1, "L", "num", 0, 1000, ""), VARIATION]),
    ("FX", 0, [
        ("Kind", 0, 4, 1, "L", "choice", 4, 1, "", ["Riser", "Laser", "Impact", "Siren", "Glitch"]),
        ("Pitch", 40, 4000, 300, "E", "hz", 0, 1, ""), ("Range", 0, 6, 3, "L", "num", 0, 1000, " oct"),
        ("Rate", .2, 30, 4, "E", "hz", 0, 1000, ""), ("Noise", 0, 1, .15, "L", "num", 0, 1000, ""),
        ("Crush", 0, 1, 0, "L", "num", 0, 1000, ""), VARIATION]),
    ("Wavetable", 1, [
        ("Table", 0, 4, 0, "L", "choice", 4, 1, "", ["Saw-Square", "Dark-Bright", "Formant", "Organ", "Digital"]),
        ("Position", 0, 1, .2, "L", "num", 0, 1000, ""), ("Sweep", -1, 1, .5, "L", "num", 0, 1000, ""),
        ("Motion", 0, 1, .15, "L", "num", 0, 1000, ""), ("Rate", .05, 8, .7, "E", "hz", 0, 1000, ""),
        ("Detune", 0, 40, 12, "L", "num", 0, 1, " ct"), ("Sub", 0, 1, .25, "L", "num", 0, 1000, ""), VARIATION]),
    ("Drums", 0, [
        ("Kind", 0, 7, 0, "L", "choice", 7, 1, "", ["Kick", "Snare", "Hat", "Open hat", "Clap", "Tom", "Cowbell", "Rim"]),
        ("Tune", -12, 12, 0, "L", "num", 24, 1, " st"), ("Decay", 0, 1, .5, "L", "num", 0, 1000, ""),
        ("Tone", 0, 1, .5, "L", "num", 0, 1000, ""), ("Snap", 0, 1, .5, "L", "num", 0, 1000, ""),
        ("Hits", 1, 8, 4, "L", "num", 7, 1, ""), ("Humanize", 0, 1, .3, "L", "num", 0, 1000, ""), VARIATION]),
    ("Bow/Wind", 1, [
        ("Mode", 0, 2, 0, "L", "choice", 2, 1, "", ["Bowed", "Flute", "Clarinet"]),
        ("Pressure", 0, 1, .6, "L", "num", 0, 1000, ""), ("Bright", 0, 1, .6, "L", "num", 0, 1000, ""),
        ("Position", 0, 1, .5, "L", "num", 0, 1000, ""), ("Noise", 0, 1, .2, "L", "num", 0, 1000, ""),
        ("Vibrato", 0, 1, .3, "L", "num", 0, 1000, ""), ("Attack", .02, 1, .2, "E", "num", 0, 1000, " s"), VARIATION]),
    ("Dust", 1, [
        ("Density", 1, 400, 40, "E", "num", 0, 10, " /s"), ("Body", 100, 8000, 1800, "E", "hz", 0, 1, ""),
        ("Decay", .005, .5, .05, "E", "num", 0, 1000, " s"), ("Spread", 0, 1, .4, "L", "num", 0, 1000, ""),
        ("Dynamics", 0, 1, .6, "L", "num", 0, 1000, ""), ("Clump", 0, 1, .3, "L", "num", 0, 1000, ""),
        ("Bed", 0, 1, .1, "L", "num", 0, 1000, ""), VARIATION]),
]
GEN_LENGTH = ("Length", .25, 2.0, 1.5, "E", "num", 0, 1000, " s")      # at 22.05 kHz, 2 s fill a part


def pline(label, mn, mx, df, cv, unit, steps, cc, mul, sfx, labs):
    mni, mxi = int(round(mn * mul)), int(round(mx * mul))
    lg = int(round(65536 * math.log2(mx / mn))) if cv == "E" else 0
    dn = int(round(65535 * ((math.log(df / mn) / math.log(mx / mn)) if cv == "E" else (df - mn) / (mx - mn))))
    return (f'    {{"{label}", {mni}, {mxi}, {dn}, {lg}, {mul}, {steps}, {cc}, GR_{cv}, GR_U_{unit.upper()}, {labs}, '
            f'"{sfx}"}},')


def arr(name, ctype, vals, per=12):
    L = [f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per):
        L.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    L.append("};")
    return L


def main(path):
    L = ["/* generated by tools/gen_grain.py: do not edit */", f"#define FS {FS}", ""]
    # ---- tables
    L += arr("GR_SINE", "int16_t", [int(round(32767 * math.sin(2 * math.pi * i / 1024))) for i in range(1025)], 16)
    L += arr("GR_HANN", "uint16_t", [int(round(65535 * (0.5 - 0.5 * math.cos(2 * math.pi * i / 1024)))) >> 1
                                      for i in range(1024)], 16)                       # Q15 window
    L += arr("GR_EXP2", "uint32_t", [int(round(65536 * 2 ** (i / 256))) for i in range(257)], 8)   # 2^frac, Q16
    # g = tan(pi f / FS) as Q24 for the cutoff parameter's 257 norm points (30 Hz .. 18 kHz, exponential)
    cut = [p for p in PARAMS if p[0] == "CUTOFF"][0]
    fl = [cut[2] * (cut[3] / cut[2]) ** (i / 256) for i in range(257)]
    L += arr("GR_TAN", "uint32_t", [int((1 << 24) * math.tan(math.pi * min(f, 0.45 * FS) / FS)) for f in fl], 8)
    # 2^(i/12) for the 128 MIDI note distances is done by GR_EXP2; the pan law quarter wave is GR_SINE
    # IMA ADPCM (the stored samples): the step sizes and the index moves
    import sys as _s
    _s.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
    import sampleio as sio
    L += arr("GR_IMA_STEP", "uint16_t", list(sio.IMA_STEP), 12)
    L += arr("GR_IMA_IDX", "int8_t", list(sio.IMA_IDX), 8)
    # ---- the icons of the choice cells: 26 points of the shape, -32 .. 32 (Screen.h iconShape)
    def shape(k, t):
        if k == 0: return math.sin(2 * math.pi * t)
        if k == 1: return 4 * t if t < .25 else (2 - 4 * t if t < .75 else 4 * t - 4)
        if k == 2: return 4 * t - 1 if t < .5 else 4 * t - 3
        if k == 3: return .8 if t < .5 else -.8
        if k == 4: return [.3, -.7, .8, -.2, .5][int(t * 4.99)]
        if k == 5: return .65 * math.sin(2 * math.pi * t * 1.5) * math.cos(math.pi * t * .7) + .25 * math.sin(2 * math.pi * t * 3.3)
        if k == 6: return 0
        if k == 7:
            x = t - .55; b = .8 * math.exp(-x * x * 90)
            return .1 + b if t < .55 else .1 + b - (t - .55) * 3.2
        if k == 8:
            x = t - .6; b = .5 * math.exp(-x * x * 60)
            return .2 + b if t < .6 else .2 + b - (t - .6) * 2.2
        if k == 9:
            x = t - .5; return -.8 + 1.7 * math.exp(-x * x * 40)
        x = t - .4; b = .5 * math.exp(-x * x * 60)
        return .2 + b if t > .4 else .2 + b - (.4 - t) * 2.2
    ic = [max(-32, min(32, int(round(32 * shape(k, i / 25))))) for k in range(11) for i in range(26)]
    L += arr("GR_ICON", "int8_t", ic, 26)
    L.append("enum { IC_SINE, IC_TRI, IC_SAW, IC_SQUARE, IC_STEPS, IC_SMOOTH, IC_FLAT, IC_LADDER, IC_LP, IC_BP, IC_HP };")
    L.append("")
    # ---- parameters
    L.append("enum {")
    for p in PARAMS:
        L.append(f"    P_{p[0]},")
    L.append("    P_COUNT")
    L.append("};")
    L.append("enum {")
    for i, d in enumerate(LFO_DESTS):
        L.append(f"    D_{d} = {i},")
    L.append("    D_COUNT")
    L.append("};")
    L.append(f"#define GR_NDIV {len(DIVS)}")
    L.append("static const uint16_t GR_DIV_TICKS[GR_NDIV] = {" + ", ".join(str(d[1] if d[1] < 65536 else d[1]) for d in DIVS) + "};")
    L.append("static const uint8_t GR_DIV_TRIP[GR_NDIV] = {" + ", ".join(str(d[2]) for d in DIVS) + "};")
    L.append("static const uint8_t GR_LFO_DEST[D_COUNT] = {")
    L.append("    " + ", ".join("P_COUNT" if d == "NONE" else "P_" + d for d in LFO_DESTS) + ",")
    L.append("};")
    ad = ALFO_DESTS + ["NONE"] * (len(LFO_DESTS) - len(ALFO_DESTS))
    L.append("static const uint8_t GR_ALFO_DEST[D_COUNT] = {")
    L.append("    " + ", ".join("P_COUNT" if d in ("NONE", "ATRANS") else "P_" + d for d in ad) + ",")
    L.append("};")
    L.append(f"#define GR_ACC_N {len(ACC)}")
    L.append("static const struct { uint8_t cc, param; } GR_ACC[GR_ACC_N] = {")
    L.append("    " + ", ".join(f"{{{cc}, P_{n}}}" for cc, n in ACC) + ",")
    L.append("};")
    L.append("")
    for p in PARAMS:
        if len(p) > 10:
            labs = ", ".join(f'"{s}"' for s in p[10])
            L.append(f"static const char *const GR_CH_{p[0]}[] = {{{labs}}};")
    L.append("")
    L.append("static const gr_pinfo_t GR_PINFO[P_COUNT] = {")
    for p in PARAMS:
        name, label, mn, mx, df, cv, unit, steps, cc, mul = p[:10]
        L.append(pline(label, mn, mx, df, cv, unit, steps, cc, mul, SFX.get(name, ""), f"GR_CH_{name}" if len(p) > 10 else "0"))
    L.append("};")
    L.append("")
    ident = lambda g: "".join(ch for ch in g[0] if ch.isalnum())
    L.append(f"#define GEN_COUNT {len(GENS)}")
    L.append("enum { " + ", ".join(f"GT_{ident(g).upper()}" for g in GENS) + " };")
    for g in GENS:
        for k, q in enumerate(g[2]):
            if len(q) > 9:
                labs = ", ".join(f'"{x}"' for x in q[9])
                L.append(f"static const char *const GR_GL_{ident(g)}_{k}[] = {{{labs}}};")
        L.append(f"static const gr_pinfo_t GR_GP_{ident(g)}[] = {{")
        for k, q in enumerate(g[2]):
            L.append(pline(q[0], q[1], q[2], q[3], q[4], q[5], q[6], 255, q[7], q[8], f"GR_GL_{ident(g)}_{k}" if len(q) > 9 else "0"))
        L.append("};")
    L.append("static const gr_gentype_t GR_GENTYPES[GEN_COUNT] = {")
    for g in GENS:
        L.append(f'    {{"{g[0]}", GR_GP_{ident(g)}, {len(g[2])}, {g[1]}}},')
    L.append("};")
    L.append(pline(*GEN_LENGTH[:7], 255, *GEN_LENGTH[7:], "0").replace("{", "static const gr_pinfo_t GR_GENLEN = {", 1).rstrip(",") + ";")
    open(path, "w").write("\n".join(L) + "\n")
    print(f"grain tables + {len(PARAMS)} parameters -> {path}")


if __name__ == "__main__":
    main(sys.argv[1])
