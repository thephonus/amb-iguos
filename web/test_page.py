#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Runs web/index.html in headless Chrome against the firmware's own sample link (build/host_linkd, gr_link.c):
a fake Web MIDI port sends the page's SysEx frames to the C code and brings its replies back. The page is driven as a
user would: connect, drop a file, send it, load it into a part, drop two more, erase one.

  cc -O1 -Ifirmware/src -Ibuild/gen tests/host_linkd.c -o build/host_linkd -lm && python3 web/test_page.py
Needs Chrome or Chromium."""
import http.server
import json
import shutil
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
chrome = next((c for c in ("google-chrome", "chromium", "chromium-browser", "google-chrome-stable") if shutil.which(c)), None)
if not chrome:
    sys.exit("no Chrome / Chromium: skipped")
daemon = subprocess.Popen([str(ROOT / "build" / "host_linkd")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
lock = threading.Lock()
frames_seen = []


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"])).decode()
        with lock:
            frames_seen.append(body.split()[3] if len(body.split()) > 3 else "?")
            daemon.stdin.write(body + "\n")
            reply = daemon.stdout.readline().strip()
        self.send_response(200)
        self._cors()
        self.end_headers()
        self.wfile.write(reply.encode())


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

MOCK = f"""<script>
// a fake Web MIDI: output.send(bytes) goes to the C code over HTTP, its reply comes back as an input message
(() => {{
  const input = {{ name: 'Mini-Grain MIDI 1', onmidimessage: null }};
  const output = {{ name: 'Mini-Grain MIDI 1', send(bytes) {{
    const inner = Array.from(bytes).slice(1, -1).map((b) => b.toString(16).padStart(2, '0')).join(' ');
    fetch('http://127.0.0.1:{port}/frame', {{ method: 'POST', body: inner }}).then((r) => r.text()).then((t) => {{
      if (t && t !== '-') setTimeout(() => input.onmidimessage && input.onmidimessage({{ data: new Uint8Array(t.split(' ').map((h) => parseInt(h, 16))) }}), 1);
    }});
  }} }};
  navigator.requestMIDIAccess = async () => ({{ inputs: new Map([['i', input]]), outputs: new Map([['o', output]]), onstatechange: null }});
}})();
</script>
<script src="file://{ROOT}/web/grainlink.js"></script>"""

TEST = r"""<pre id="selftest">RUNNING</pre>
<script>
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(f, what, ms = 15000) { const t0 = Date.now(); while (!f()) { if (Date.now() - t0 > ms) throw new Error('timeout: ' + what); await sleep(20); } }
function wavFile(name, seconds, rate, freq, channels) {
  const n = Math.floor(seconds * rate), data = new DataView(new ArrayBuffer(44 + n * 2 * channels));
  const w = (o, s) => { for (let i = 0; i < s.length; i++) data.setUint8(o + i, s.charCodeAt(i)); };
  w(0, 'RIFF'); data.setUint32(4, 36 + n * 2 * channels, true); w(8, 'WAVEfmt '); data.setUint32(16, 16, true); data.setUint16(20, 1, true);
  data.setUint16(22, channels, true); data.setUint32(24, rate, true); data.setUint32(28, rate * 2 * channels, true);
  data.setUint16(32, 2 * channels, true); data.setUint16(34, 16, true); w(36, 'data'); data.setUint32(40, n * 2 * channels, true);
  for (let i = 0; i < n; i++) for (let c = 0; c < channels; c++)
    data.setInt16(44 + 2 * (i * channels + c), Math.round(12000 * Math.sin(2 * Math.PI * freq * i / rate) * Math.exp(-3 * i / n)), true);
  return new File([data.buffer], name, { type: 'audio/wav' });
}
(async () => {
  const out = [];
  const ok = (c, m) => { out.push((c ? 'ok   ' : 'FAIL ') + m); if (!c) throw new Error(m); };
  try {
    confirm = () => true;
    document.getElementById('connect').click();
    await until(() => link.info, 'the FM-1 answers INFO');
    ok(link.info.slots === 7 && link.info.maxFrames === 43008, 'INFO: ' + JSON.stringify(link.info));
    await until(() => link.list.length === 7, 'the slot list');
    ok(link.list.every((s) => !s.used), 'seven empty slots');

    await takeFiles([wavFile('test tone.wav', 0.9, 44100, 330, 1)]);
    await until(() => ed.buf, 'the editor opens');
    ok(document.getElementById('name').value === 'test tone', 'the name comes from the file');
    ok(document.getElementById('slot').value === '0', 'the first free slot is offered');
    document.getElementById('send').click();
    await until(() => link.list[0] && link.list[0].used, 'slot 1 is in use', 20000);
    ok(link.list[0].name === 'test tone' && link.list[0].rate === 44100 && Math.abs(link.list[0].frames - 39690) < 3, 'slot 1: ' + JSON.stringify(link.list[0]));

    link.busy = false;
    const part1 = Array.from(document.querySelectorAll('#slots tr'))[0].querySelector('button');
    part1.click();
    await until(() => document.getElementById('log').textContent.includes('loaded into part 1'), 'load into part 1');
    ok(true, 'LOAD into part 1 answered ok');

    await takeFiles([wavFile('a stereo 48k.wav', 1.6, 48000, 220, 2), wavFile('long 3 s.wav', 3.0, 44100, 110, 1)]);
    await until(() => link.list[1].used && link.list[2].used, 'two files into slots 2 and 3', 30000);
    ok(link.list[1].name === 'a stereo 48k' && link.list[1].rate === 22050, 'stereo 48 kHz 1.6 s -> ' + link.list[1].rate + ' Hz');
    ok(link.list[2].name === 'long 3 s' && link.list[2].rate === 11025, '3 s -> ' + link.list[2].rate + ' Hz, ' + link.list[2].frames + ' samples');

    const erase = Array.from(document.querySelectorAll('#slots tr'))[1].querySelector('button.danger');
    erase.click();
    await until(() => !link.list[1].used, 'slot 2 erased');
    ok(link.list[0].used && !link.list[1].used && link.list[2].used, 'erase takes only its slot');
    out.push('SELFTEST PASS');
  } catch (e) { out.push('SELFTEST FAIL: ' + e.message); }
  document.getElementById('selftest').textContent = out.join('\n');
  document.title = 'DONE ' + out.join(' | ');
})();
</script>"""

html = (ROOT / "web" / "index.html").read_text()
html = html.replace('<script src="grainlink.js"></script>', MOCK).replace("</body>", TEST + "\n</body>")
(ROOT / "build" / "page_test.html").write_text(html)
import tempfile
import time
import urllib.request
prof = tempfile.mkdtemp()
import socket
_s = socket.socket(); _s.bind(("127.0.0.1", 0)); cport = _s.getsockname()[1]; _s.close()      # a free port for each run
proc = subprocess.Popen([chrome, "--headless=new", "--disable-gpu", "--no-sandbox", "--autoplay-policy=no-user-gesture-required",
                         "--allow-file-access-from-files", f"--remote-debugging-port={cport}", f"--user-data-dir={prof}",
                         f"file://{ROOT}/build/page_test.html"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
title = ""
t0 = time.time()
while time.time() - t0 < 90:                         # the page puts its result in its title; /json lists the tabs
    try:
        tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{cport}/json", timeout=2))
        title = next((t["title"] for t in tabs if t.get("title", "").startswith("DONE")), "")
        if title:
            break
    except Exception:
        pass
    time.sleep(0.3)
proc.terminate()
try:
    proc.wait(timeout=10)
except subprocess.TimeoutExpired:
    proc.kill()
shutil.rmtree(prof, ignore_errors=True)
daemon.stdin.close()
print("\n".join(title[5:].split(" | ")) if title else "no result from the page (timeout)")
sys.exit(0 if "SELFTEST PASS" in title else 1)
