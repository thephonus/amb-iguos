#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make amb-iguos's web site (GitHub Pages) from a built package, after Felucca's web/make_site.py:

  index.html               a redirect to the installer
  installer/               web/installer/ (index.html, fm1ota.js, fm1pkg.js)
  muestras/                web/index.html + web/grainlink.js: the sample page
  firmware/amb-iguos.fwsc  the package, version.txt, LICENSE, LICENSING.md, LICENSES/

  tools/make_site.py build/amb-iguos.fwsc VERSION OUT_DIR
"""
import re
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def product_of(raw):
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if product != "FM-1_946":
        raise SystemExit(f"{pkg}: identity {product!r}, not amb-iguos's FM-1_946")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no Felucca loader in it (only tools/build.py packages are published)")
    inst, smp, fw = out / "installer", out / "muestras", out / "firmware"
    for d in (inst, smp, fw / "LICENSES"):
        d.mkdir(parents=True, exist_ok=True)
    for f in ("index.html", "fm1ota.js", "fm1pkg.js"):
        shutil.copy(SRC / "web" / "installer" / f, inst / f)
    shutil.copy(SRC / "web" / "index.html", smp / "index.html")
    shutil.copy(SRC / "web" / "grainlink.js", smp / "grainlink.js")
    shutil.copy(pkg, fw / "amb-iguos.fwsc")
    (fw / "version.txt").write_text(version + "\n")
    for doc in ("LICENSE", "LICENSING.md"):
        shutil.copy(SRC / doc, fw / doc)
    for f in sorted((SRC / "LICENSES").glob("*.txt")):
        shutil.copy(f, fw / "LICENSES" / f.name)
    (out / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>amb-iguos</title>'
                                    '<meta http-equiv="refresh" content="0; url=installer/">'
                                    '<a href="installer/">Instalador de amb-iguos</a>\n', encoding="utf-8")
    (out / ".nojekyll").write_text("")
    print(f"site: {out}: installer, muestras, firmware/amb-iguos.fwsc ({len(raw)} B, {product}, {version})")


if __name__ == "__main__":
    if len(sys.argv) != 4 or not re.match(r"^[\w .:+()-]+$", sys.argv[2]):
        sys.exit(__doc__)
    main(*sys.argv[1:4])
