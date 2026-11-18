# Mini-Grain

A granular synthesizer firmware for the M-Vave FM-1.

Two polyphonic granular parts, a drone and a pad (after the Drone and Pad layers of the Sonicware LIVEN Ambient Ø),
each with a master filter, two LFOs and a step sequencer, a shared ping-pong delay and
plate reverb, seven sample generators, samples sent over USB, 16 presets, and USB / TRS MIDI (with clock). The sound engine, the parameters and
the look of the screens come from the Granular Synth for ESP32-S3; the FM-1's own encoders
replace its single knob, so every page has four dials under your fingers. The platform (USB-MIDI, USB audio, TRS
MIDI, flash storage, the update entry) is Felucca / Mini-Tone by Leo Kuroshita (Hügelton Instruments).

> **Status: not yet run on a real FM-1.** The engine, the effects, the sequencer, the sample transfer and the screens are tested
> on the computer (`tests/run_tests.sh`) and the firmware builds, links and passes the build's safety checks. Nothing
> has been flashed. Read "Limits and what is missing" before you do.

## The panel

| Control | What it does |
|---|---|
| **K1 K2 K3 K4** | the four dials of the active row (a coral frame shows the one you turn; turn fast for bigger steps) |
| **ALGORITHM** | the active row of dials on the page (the row's cells are lit) |
| **SELECT** | the next / previous page |
| **PRESETS** | the preset slot (1-16) |
| **HOME** Grain · **ENV** Voice · **FX** Effects · **LFO** Filter + LFO · **EDIT** Generate · **SCL** Samples · **SAVE** Presets · **GLO** Info | the page's button |
| **ARP** | the next part, 1 to 4 (the pages edit this part, the keys play it; the button lights for the drone and the pad) |
| **OCT- / OCT+** | the octave of the keys (±3); hold both for 5 s to enter the update mode |
| **Keys** (F3 to G5) | play the selected part; velocity is fixed |
| **PLAY** | start / stop the sequencer. On Generate it makes the sound; on Samples and Presets it loads. |
| **REC** | arm / stop recording into the selected part's pattern. On Samples it erases the slot (twice); on Presets it resets everything (twice). |
| **SAVE** | on Presets: save the slot (press twice to overwrite). On Generate: store the sound in a free sample slot. |
| **SEQ** | the sequencer's pages: Steps, then Tools, then Sequencer. Hold it for a fill. |
| **MASTER** | the output level |

## Reading the screen

The top bar shows the part (sand-coloured 1, aqua 2, lilac 3, mint 4), the page, the part's sound, how many grains play and the CPU as
six dots (red above 85 %); a small coral dot flickers when MIDI arrives. The dots at the bottom show the page.

A dial's ring of dots shows its value, and a white dot inside it shows where an LFO has moved it. The picture
at the top of the Grain page is the sound itself, stippled like sand: the line is Position, the lit area Spread, and
the glowing grains are the ones playing now. Voice shows the envelope, Effects the echoes and the reverb cloud.

## Pages

| Page | Row 1 | Row 2 | Row 3 |
|---|---|---|---|
| **Grain** | Position, Size, Density, Spread | Scan, Jitter, Pitch rnd, Pan | |
| **Voice** | Attack, Release, Reverse, Transpose | Level, Master, MIDI ch, Key hold | |
| *Oscillator, Voice (parts 3 and 4)* | Wave, Harmonic, Structure, Pitch; Attack, Release, Transpose, Level | Detune, Balance, Mod rate, Mod depth; Pan, Voice mode, Glide, Uni detune | Mod shape, MIDI ch, Master, Key hold |
| **Effects** | Delay time, feedback, damping, send | Reverb decay, damping, send | |
| **Filter + LFO** | Filter, Cutoff, Reso, Drive | LFO 1 wave, rate, depth, dest | LFO 2 wave, rate, depth, dest |
| **Generate** | Type, Length, PLAY, SAVE | the algorithm's parameters | |
| **Samples** | a list of seven slots (K1 picks) | | |
| **Steps** | Step, Note, Force, Length | Ratchet, Slide, Nudge, Plays | |
| **Tools** | Step, Lock, Lock value, Clear locks | Copy step, Move step, Delete step, Clear track | Undo, Locks used, Quantize, Glide |
| **Sequencer** | Tempo, Swing, Click, Rec start | Length, Division, Track swing, Gate | Sync, MIDI out, Stop, Rec delay |
| **Presets** | sixteen grains: the filled ones hold a preset | | |
| **Info** | CPU, grains, cap, notes, MIDI messages | | |

Position is where in the sound the grains start, Size their length, Density how many start per second, Spread how far
around Position they start, Scan how fast the position moves (+1.00x is the speed of the sound), Jitter how
irregular the timing is. Size, Density and similar settings apply to grains that start afterwards.

**Filters:** Vintage 24 is a 24 dB/octave ladder (warm, saturating, self-oscillating); Res LP, BP and HP are 12 dB/octave
resonant filters. Drive pushes the input; Vintage 24 reacts most.
**LFOs:** 24 shapes (see Four parts); they can move any continuous parameter of their part, the shared effects, or each other.
Depth works on the parameter's range, and two LFOs on one parameter add.

## Four parts

Parts 1 and 2 are granular; **part 3 is the Drone and part 4 the Pad**. They play MIDI channels 1 to 4 from USB or the
TRS jack, and the keys play the selected part (ARP picks it; the badge in the status bar shows its number, in sand,
aqua, lilac or mint). Give parts one channel (Voice, MIDI ch) to play them as a layer. Each part has its own sound, envelope,
filter, LFOs, level and sends and its own pattern; the delay, the reverb and the master level are shared.

| Separate for each part | Shared by all |
|---|---|
| Sound, grain or oscillator settings, envelope, transpose | Delay time, feedback and damping |
| Filter and both LFOs | Reverb decay and damping |
| Level, delay send and reverb send | Master volume, the tempo and the sequencer's settings |
| Notes, pitch bend, sustain, MIDI channel, pattern | The grain budget and the oscillator budget |

**Sharing grains:** a granular part that needs less than half of the grain limit gets all it needs, and the other can use
the rest; if both need more, they get half each. Excess grains fade out quickly when the balance changes. A part with no
notes and no output uses almost no CPU. When the load is high the drone and the pad also give up oscillators for new notes.

**LFOs:** they can target anything in their own part, plus the shared settings. If two LFOs target the same shared setting, their
movement adds together. There are 24 shapes: Sine, Square, Triangle, Saw, Reverse saw, Random, Smooth random, Log,
Reverse log, Pulse 10/25/75/90, Steps 2 to 7, Ramp up and down, and slow Sine, Triangle and Smooth random.

## The Drone and the Pad

Two wavetable parts built from the Sonicware LIVEN Ambient Ø's user's manual (rev. 3): the **Drone** (part 3, an octave
below the notes at -24 st, a slow attack and a very long release) and the **Pad** (part 4, -12 st). They are made after its
Drone and Pad layers, and **are not copies of them**: the 32 waves and their harmonic tables are Sonicware's and are not
published, so each wave here is made from the manual's one-line description of it (a wavefolded sine, a pulse whose width
moves, even harmonics morphing to odd, a formant, noise ...). The layout, the controls and what they do follow the
manual; the sound is its own.

**Oscillator page** (the picture shows one cycle of the wave at the harmonic position):

| Dial | What it does |
|---|---|
| **Wave** | one of 32: SAW.S SQR.D SIN.S SIN.D SQR.S TRI.S SQ.SN EV.OD OCTV SIN.M SIN.F TRI.F SIN.C FM.CR FM.OD SYNC HRM.1 HRM.2 CL.FM FRML FRM.1 FRM.2 27.PS FM.WF FM.PF AD.MP BCHL SQR.R RNDM S.RND TRI.N WH.NS |
| **Harmonic** | 0-127: the position in the wave's four tables, morphing between them |
| **Structure** | Drone 1 / Drone 2 on the drone, Pad 1 / Pad 2 on the pad (below) |
| **Pitch** | the pitch of the second oscillator, -24 to +24 semitones |
| **Detune** | the detune of the second oscillator, -50 to +50 cents |
| **Balance** | between the oscillators (or groups of them) |
| **Mod rate, Mod depth** | the speed and the depth of the MOD LFO, which restarts at every note |

The **structures** set the oscillators and what the MOD moves. A "Yang" oscillator's harmonic position moves up with
the MOD and a "Yin" one's down.

| Structure | Oscillators | MOD moves |
|---|---|---|
| Drone 1 | 2 Yang; PITCH and DETUNE apply to the second | the second one's detune |
| Drone 2 | 1 Yang, 1 Yin; PITCH and DETUNE apply to the Yin | the harmonic position |
| Pad 1 | 1 Yang, 2 Yin (PITCH on one, DETUNE on the other); Balance between Yang and the Yin pair | the harmonic position |
| Pad 2 | 2 Yang, 1 Yin (DETUNE on the Yin, PITCH on the third); Balance between the first two and the third | the harmonic position |

**Voice page:** Attack (up to 10 s), Release (up to 20 s), Transpose, Level, Pan, **Voice mode** (Poly; Mono, each note
retriggers; Legato, notes do not retrigger; Unison, four detuned copies), Glide (Mono and Legato), Uni detune,
**Mod shape** (the 24 shapes) and the MIDI channel. The Filter + LFO page is the one of the granular parts (a low-pass,
band-pass or high-pass, and two LFOs), with the destinations of these parts: Pitch, Harmonic, Balance, Detune, LFO 1 rate and
depth, Mod rate and depth, Cutoff, Reso, Pan, Level and the two sends. A part holds four notes and runs up to 16
oscillators (a note of a three-oscillator structure costs 3, in Unison 4 times that); a new note takes the place of the oldest.
The Generate and Samples pages belong to the granular parts. The drone's and the pad's MIDI CCs are in the table below.
Not ported: the Ambient Ø's arpeggiator, its Atmos structures and Noise layer, and the shimmer reverb.

## Sounds

The FM-1 has no SD card and no PSRAM, so a part holds one sound in RAM: up to 43008 samples (1.95 s at 22.05 kHz, 0.97 s at
44.1 kHz). A sound is one of:

- **Generated** on the Generate page, at 22.05 kHz: VA, Vowel, Pluck (Karplus-Strong), FM, Additive, Texture, FX,
  Wavetable, Drums, Bow/Wind and Dust (below).
  Choose the type with K1 on the first row, the length with K2, the parameters on the next rows, then press PLAY. The
  part goes quiet while it works (a fraction of a second) and a test note follows. The same settings always give the
  same sound, so presets keep only the settings. Tuned sounds are at C4, so MIDI note 60 plays them at their pitch.
  SAVE stores a generated sound in a free sample slot.
  | Type | What it makes | Main parameters |
  |---|---|---|
  | **Wavetable** | an evolving tone: eight frames of a table, the position swept and wobbled, two detuned oscillators and a sub | Table (Saw-Square, Dark-Bright, Formant, Organ, Digital), Position, Sweep, Motion, Rate, Detune, Sub |
  | **Drums** | a drum hit, or a few of them in a row | Kind (Kick, Snare, Hat, Open hat, Clap, Tom, Cowbell, Rim), Tune, Decay, Tone, Snap, Hits, Humanize |
  | **Bow/Wind** | a sustained bowed string, flute or clarinet, as waveguide models | Mode, Pressure, Bright, Position, Noise, Vibrato, Attack |
  | **Dust** | sparse random clicks through resonant bodies, with a bed of hiss: rain, fire, crackle | Density, Body, Decay, Spread, Dynamics, Clump, Bed |

  In Bow/Wind, Position is the bow's place on the string, the flute's jet distance or the clarinet's reed stiffness;
  a flute that is blown too softly stays breathy and one that is blown too hard can jump to a higher note.
- **Stored**: seven slots of flash, 4-bit IMA ADPCM, at most 24 KiB each. Pick one on the Samples page (K1) and press
  PLAY to load it into the selected part, or put new ones there from your computer:

### Putting samples on the FM-1

Open **`web/index.html`** in Chrome, Edge or Opera (Firefox asks permission first; Safari cannot do it), with the FM-1
connected by USB and running Mini-Grain, and press **Connect**.

- **Drop an audio file** on the page (WAV, MP3, AAC, FLAC, OGG: anything your browser plays), or on a slot to choose it.
  The editor shows the waveform: drag its two edges to **trim**, pick the slot and the name, and the quality (Automatic
  picks the best rate that fits: a part holds 43008 samples, so 0.97 s at 44.1 kHz, 1.95 s at 22.05 kHz, 5.4 s at 8 kHz).
  **Preview** plays what the FM-1 will play, after the conversion. **Send** puts it in the slot.
- **Several files at once** go to the next free slots with the automatic settings.
- The slot list has **Part 1 / Part 2** (load that slot into a part of the FM-1) and **Erase**.
- The file is converted in your browser (mono, resampled, normalised, softened at the ends) and nothing leaves your
  computer. Playback pitch follows the stored rate; MIDI note 60 is the sample's original pitch.

The page needs no installation: it is two files (`index.html`, `grainlink.js`) that you can also put on any web server.
For scripts there is a command line tool, with the same result:

```
python3 tools/grain_sample.py list
python3 tools/grain_sample.py send 3 kick.wav "Kick 01"       # slot 3; mono, shortened or resampled to fit a part
python3 tools/grain_sample.py load 3 1                         # slot 3 into part 1
python3 tools/grain_sample.py erase 3
```

(`pip install mido python-rtmidi`; the FM-1 shows up as the MIDI port "Mini-Grain".)

## Sequencer

One pattern per part (the selected part's, the other keeps playing its own), up to 64 steps, each of its own **Length**
(1-64) and **Division**, from 1/32 up to **8 bars** a step (with dotted and triplet values: 1/16, 1/16., 1/16T … 1/2, 1/2T, 1 bar, 2, 3, 4 and 8 bars), so patterns of
different lengths drift against each other and come back in phase, and a 64-step pattern can last hours. The status bar of the Steps, Tools
and Sequencer pages shows the time the pattern takes to come round (**Cycle 3:20**). **Tempo** is 20-300 BPM; **Swing** (shared) and **Track swing** delay the odd
steps up to a 75/25 feel (triplet divisions stay straight); **Gate** is how long a note lasts in its step, from 5 % to **400 %**: over 100 % the note rings into the following steps (a held
drone chord that overlaps the next one).

**Steps page** (a piano roll of 16 steps, the cursor's step lit, the playing step marked on top):

- **K1** moves the cursor (so does the **PRESETS** knob). **K2** is the note: on an empty step it enters the last note,
  after that the whole chord moves. **K3** is the force (ghost, soft, normal, hard, accent). **K4** is the length: turn
  right to tie the note over the following empty steps.
- **ALGORITHM** shows the second row: **Ratchet** (the step plays x1 to x4), **Slide** (the next note glides from this
  one, on one voice), **Nudge** (the step's timing, in 1/64 of a step, early or late), **Plays** (always, only during a
  fill, or never during one). A dot in a step's corner marks a nudge or a lock.
- **Keys** only sound the part. To enter notes with the keys, use K2, or record (below).

**Recording** happens only when you ask for it with REC; playing keys never changes a pattern otherwise.

- **REC** arms (the button lights; press again to disarm). What starts the recording is **Rec start**: *First note* (your
  first key starts the pattern and records), *Count-in* (press PLAY: four clicks, then it records) or *Step* (the sequencer
  stays stopped and each chord you play and let go of is written into the step under the cursor, which then moves on).
- While playing, REC starts and stops recording over the pattern.
- **REC + SEQ together** starts recording from the top after **Rec delay** seconds (0-10, on the Sequencer page): the
  number of seconds left counts down in the status bar, so you can get to the keys. Any of PLAY, REC or SEQ+REC cancels it.
  With a delay of 0 it starts at once.

Notes land on the nearest step (**Quantize** off keeps your timing as nudge), a held note becomes ties, and several notes
in a step make a chord (the fourth gives way to a fifth). **Click** is the metronome (off, recording, always).
Everything is undone in one step with **Undo** on the Tools page (twice brings it back).

**Parameter locks:** on the Tools page, put the cursor on a step, pick a parameter with **Lock** (K2) and turn **Lock
value** (K3): from then on, that step plays with the parameter at that value, and it returns to normal at the next step
that has no lock. Position, Size, Density, Pitch, Cutoff and so on can be locked (up to 48 locks a part). **Clear
locks** removes the step's. **Copy step** copies it onto the next and moves there, **Move step** swaps it with its
neighbour (locks go along), **Delete step** and **Clear track** empty them (Clear track asks twice). These actions
happen when you turn their knob; turn once and wait a moment before turning again.

**Gliding locks:** turn **Glide** (Tools, last dial) on a step's lock and the parameter no longer jumps: it moves in a straight line from
that lock to the next lock of the same parameter (round the end of the pattern too), whatever lies between. Two locks 32 steps
apart at one bar a step make a sweep of about a minute (at 120 BPM). **Recording moves:** while recording (REC, playing), turning a dial on the Grain, Voice,
Effects, Filter or Oscillator pages writes a gliding lock at the step playing; keep turning and the sweep is recorded, then plays back
every loop.

**Stop:** with **Stop** on *Release* (Sequencer page) stopping the sequencer lets the notes die away with their release (a drone
fades out); *Cut* silences them at once. **Key hold** (Voice page, per part): letting go of a panel key leaves its note sounding, pressing the
key again lets it go; turning the setting off lets all of them go. Play a chord on a drone, take your hands away, and go on turning dials.

**Fills:** hold SEQ while playing: steps set to Fill only play, steps set to No fill drop out.

**MIDI:** set **Sync** to MIDI clock and the sequencer follows the incoming clock (USB or TRS): Start, Continue and Stop
work, the pattern runs at the clock's tempo, and if the clock stops for half a second the pattern carries on at the Tempo setting. **MIDI out**
sends the pattern's notes on each part's channel, optionally with clock and transport, over USB. (Not ported from the
Granular synth: sections, chains and the song mode.)

## Saved settings and presets

The current state (all parameters of the four parts, the MIDI channels, the sequencer's settings, the patterns with their
locks, the generator settings and which stored sample each granular part uses) is saved in flash about 3 seconds after your last change, while no note sounds (a flash erase stops
the audio for a moment), and restored at power-on. Sixteen presets hold the same: pick a slot with the PRESETS knob,
load with PLAY, save with SAVE on the Presets page. A stored sample is kept by its slot number, so replace it with
care; a preset whose slot is empty falls back to the generator.

## MIDI

USB MIDI and the TRS jack feed the same engine, on each part's own channel. Note 60 is the sound's original pitch,
pitch bend is ±2 semitones, CC 64 is sustain, CC 120 all sound off, CC 123 all notes off.
CCs for part settings affect every part on the message's channel; CCs for shared settings work from any part's channel.

| CC | Parameter | CC | Parameter |
|---|---|---|---|
| 1 | Position | 20 | LFO 1 rate |
| 7 | Part level | 21 | LFO 1 depth |
| 12 | Delay time | 22 | LFO 2 rate |
| 13 | Delay feedback | 23 | LFO 2 depth |
| 14 | Delay damping | 24 | Master volume |
| 15 | Reverb decay | 70 | Spread |
| 16 | Reverb damping | 71 | Filter resonance |
| 17 | Size | 72 | Release |
| 18 | Density | 73 | Attack |
| 19 | Filter drive | 74 | Filter cutoff |
| 75 | Scan | 76 | Jitter |
| 77 | Pitch random | 78 | Pan spread |
| 79 | Reverse probability | 91 | Reverb send |
| 93 | Delay send | | |

**The drone and the pad (parts 3 and 4)** answer the shared CCs above (7 level, 12 to 16 delay and reverb, 24 master, 91 and 93 sends)
and: 10 pan, 19 drive, 20 to 23 the LFOs, 71 resonance, 72 release, 73 attack, 74 cutoff, 85 wave, 86 harmonic, 87 pitch, 88 detune,
89 balance, 90 mod rate, 92 mod depth, 94 glide.

USB also carries the audio: the FM-1 is a class-compliant USB sound card (44.1 and 48 kHz) playing the master output.

## Limits and what is missing

- The Granular synth's sections, chains and song mode are not ported.
- PLAY and REC do the page's own job on the Generate, Samples and Presets pages; start the sequencer from another page.
- The CDC console and the web editor of Mini-Tone are gone; the M-UPGRADE update entry stays.
- Memory is the limit: a granular sound is 1.95 s at most, the delay is 0.7 s at half the sample rate (a darker, tape-like echo),
  and the grain pool is 28.
- The CPU of the FM-1 is not measured yet: the grain cap adapts to it, and Info shows the load. If the sound breaks up
  at once, lower Density or Size; please report the load.

## Building

See [BUILDING.md](BUILDING.md). Licence and credits: [LICENSING.md](LICENSING.md).
