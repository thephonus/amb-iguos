// SPDX-License-Identifier: GPL-3.0-only
// Mini-Grain sample link: the SysEx protocol of firmware/src/gr_link.c, in plain functions (no DOM), so the page and
// the Node test (web/test_link.mjs) share it. Frames are F0 7D 47 52 cmd args.. F7; numbers are 5 bytes of 7 bits, LSB
// first; bytes are packed 8 to 7 bits.
(function (root) {
  'use strict';
  const HDR = [0x7D, 0x47, 0x52];
  const CMD = { INFO: 1, LIST: 2, BEGIN: 3, WRITE: 4, END: 5, ERASE: 6, LOAD: 7 };
  const ERR = { 1: 'bad argument', 2: 'too big', 3: 'flash error', 4: 'CRC mismatch', 5: 'slot is empty', 6: 'out of order' };

  const IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767];
  const IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8];

  const u32 = (v) => [0, 1, 2, 3, 4].map((i) => Math.floor(v / 2 ** (7 * i)) & 0x7F);
  const rd32 = (b, at) => b[at] + b[at + 1] * 128 + b[at + 2] * 16384 + b[at + 3] * 2097152 + (b[at + 4] & 15) * 268435456;

  function pack8to7(data) {
    const out = [];
    let acc = 0, nb = 0;
    for (const b of data) {
      acc |= b << nb;
      nb += 8;
      while (nb >= 7) { out.push(acc & 0x7F); acc >>= 7; nb -= 7; }
    }
    if (nb) out.push(acc & 0x7F);
    return out;
  }

  let crcTable = null;
  function crc32(bytes) {
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; crcTable[n] = c >>> 0; }
    }
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 255] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  // IMA ADPCM, 4 bit, the first sample in the low nibble, from predictor 0 / index 0 (same as tools/sampleio.py)
  function imaEncode(samples) {
    let pred = 0, idx = 0;
    const nib = [];
    for (let n = 0; n < samples.length; n++) {
      const step = IMA_STEP[idx];
      let diff = samples[n] - pred, code = 0;
      if (diff < 0) { code = 8; diff = -diff; }
      let vd = step >> 3;
      if (diff >= step) { code |= 4; diff -= step; vd += step; }
      if (diff >= step >> 1) { code |= 2; diff -= step >> 1; vd += step >> 1; }
      if (diff >= step >> 2) { code |= 1; vd += step >> 2; }
      pred = code & 8 ? pred - vd : pred + vd;
      pred = Math.max(-32768, Math.min(32767, pred));
      idx = Math.max(0, Math.min(88, idx + IMA_IDX[code & 7]));
      nib.push(code);
    }
    if (nib.length & 1) nib.push(0);
    const out = new Uint8Array(nib.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = nib[2 * i] | (nib[2 * i + 1] << 4);
    return out;
  }

  function imaDecode(bytes, frames) {
    let pred = 0, idx = 0;
    const out = new Int16Array(frames);
    for (let n = 0; n < frames; n++) {
      const code = (bytes[n >> 1] >> ((n & 1) * 4)) & 15, step = IMA_STEP[idx];
      let vd = step >> 3;
      if (code & 4) vd += step;
      if (code & 2) vd += step >> 1;
      if (code & 1) vd += step >> 2;
      pred = Math.max(-32768, Math.min(32767, code & 8 ? pred - vd : pred + vd));
      idx = Math.max(0, Math.min(88, idx + IMA_IDX[code & 7]));
      out[n] = pred;
    }
    return out;
  }

  // float samples (any scale) -> int16 with the peak at `target`; truncating like the Python tool
  function toInt16(x, target) {
    let pk = 1e-9;
    for (let i = 0; i < x.length; i++) pk = Math.max(pk, Math.abs(x[i]));
    const out = new Int16Array(x.length);
    for (let i = 0; i < x.length; i++) out[i] = Math.max(-32768, Math.min(32767, Math.trunc(x[i] / pk * target)));
    return out;
  }

  // the SysEx frames (the bytes between F0 and F7) of one upload
  function buildFrames(slot, samples, rate, name) {
    const adp = imaEncode(samples);
    const nm = Array.from(name).map((c) => c.charCodeAt(0)).filter((c) => c >= 32 && c < 127).slice(0, 15);
    while (nm.length < 16) nm.push(0);
    const frames = [[...HDR, CMD.BEGIN, slot, ...nm, ...u32(samples.length), ...u32(rate), ...u32(adp.length)]];
    for (let off = 0; off < adp.length; off += 256) frames.push([...HDR, CMD.WRITE, slot, ...u32(off), ...pack8to7(adp.subarray(off, off + 256))]);
    frames.push([...HDR, CMD.END, slot, ...u32(crc32(adp))]);
    return { frames, adpcm: adp };
  }

  // reply bytes (without F0 / F7) of INFO and LIST
  function parseInfo(r) { return { version: r[5], slots: r[6], maxFrames: rd32(r, 7), maxBytes: rd32(r, 12) }; }
  function parseList(r) {
    const out = [];
    for (let p = 5, k = 0; p + 27 <= r.length; p += 27, k++) {
      const name = String.fromCharCode(...r.slice(p + 1, p + 17).filter((c) => c)).trim();
      out.push({ used: !!r[p], name, frames: rd32(r, p + 17), rate: rd32(r, p + 22) });
    }
    return out;
  }

  // how many samples and at what rate a clip of `seconds` seconds goes into a part: the highest rate that fits
  const RATES = [44100, 32000, 22050, 16000, 11025, 8000];
  function fitRate(seconds, sourceRate, maxFrames, wanted) {
    if (wanted) return { rate: wanted, frames: Math.min(maxFrames, Math.floor(seconds * wanted)), cut: seconds * wanted > maxFrames };
    for (const r of RATES) if (r <= sourceRate && seconds * r <= maxFrames) return { rate: r, frames: Math.floor(seconds * r), cut: false };
    const r = RATES[RATES.length - 1];
    return { rate: r, frames: Math.min(maxFrames, Math.floor(seconds * r)), cut: seconds * r > maxFrames };
  }

  const api = { HDR, CMD, ERR, pack8to7, u32, rd32, crc32, imaEncode, imaDecode, toInt16, buildFrames, parseInfo, parseList, fitRate, RATES };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.GrainLink = api;
})(typeof self !== 'undefined' ? self : globalThis);
