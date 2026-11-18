// SPDX-License-Identifier: GPL-3.0-only
// The page's encoder against the Python tool: the same WAV must give byte-identical SysEx frames.
//   python3 tools/grain_sample.py frames 3 tests/tone.wav build/frames.txt && node web/test_link.mjs
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const G = createRequire(import.meta.url)('./grainlink.js');

const wav = readFileSync(new URL('../tests/tone.wav', import.meta.url));
let p = 12, fmt = null, data = null;
while (p + 8 <= wav.length) {                       // RIFF chunks
  const id = wav.toString('ascii', p, p + 4), n = wav.readUInt32LE(p + 4);
  if (id === 'fmt ') fmt = { ch: wav.readUInt16LE(p + 10), rate: wav.readUInt32LE(p + 12), bits: wav.readUInt16LE(p + 22) };
  if (id === 'data') data = wav.subarray(p + 8, p + 8 + n);
  p += 8 + n + (n & 1);
}
if (!fmt || fmt.ch !== 1 || fmt.bits !== 16) throw new Error('tests/tone.wav: expected 16-bit mono');
const x = new Float64Array(data.length / 2);
for (let i = 0; i < x.length; i++) x[i] = data.readInt16LE(2 * i);
const samples = G.toInt16(x, 30000);
const { frames } = G.buildFrames(2, samples, fmt.rate, 'tone');

const want = readFileSync(new URL('../build/frames.txt', import.meta.url), 'utf8').trim().split('\n')
  .map((l) => l.trim().split(/\s+/).map((h) => parseInt(h, 16)));
let bad = 0;
if (frames.length !== want.length) { console.log(`FAIL: ${frames.length} frames, the Python tool made ${want.length}`); bad++; }
for (let i = 0; i < Math.min(frames.length, want.length); i++)
  if (frames[i].length !== want[i].length || frames[i].some((b, k) => b !== want[i][k])) { console.log(`FAIL: frame ${i} differs`); bad++; if (bad > 3) break; }
// the round trip through the decoder, as the page's preview hears it
const back = G.imaDecode(G.imaEncode(samples), samples.length);
let e = 0, s = 0;
for (let i = 0; i < samples.length; i++) { e += (back[i] - samples[i]) ** 2; s += samples[i] ** 2; }
const snr = 10 * Math.log10(s / e);
console.log(`${frames.length} frames identical to the Python tool: ${bad ? 'NO' : 'yes'}; ADPCM round trip ${snr.toFixed(1)} dB`);
if (snr < 25) { console.log('FAIL: SNR'); bad++; }
const fit = G.fitRate(2.6, 44100, 43008, 0);
console.log('fitRate(2.6 s):', JSON.stringify(fit));
if (fit.rate !== 16000 || fit.cut) { console.log('FAIL: fitRate'); bad++; }
process.exit(bad ? 1 : 0);
