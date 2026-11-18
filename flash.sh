#!/bin/sh
# Install build/mini-grain.fwsc on an FM-1 over USB (tools/fm1_install.py; see BUILDING.md).
cd "$(dirname "$0")"
python3 tools/fm1_install.py "${1:-build/mini-grain.fwsc}"
