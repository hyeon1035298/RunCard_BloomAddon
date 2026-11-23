#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the test program tools/run-host.cpp into build/test (needs build.sh's gen/ first).
# PILL_ROOT as in build.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${PILL_ROOT:-}" ]]; then
    for d in "$here/../PillTaskbar_BloomAddon" "$here/../PillTaskbar"; do
        if [[ -f "$d/tools/build-addon.sh" ]]; then PILL_ROOT="$d"; break; fi
    done
fi
cxx="${CXX:-/c/Program Files/Windhawk/Compiler/bin/clang++.exe}"
out="$here/build/test"
mkdir -p "$out"
"$cxx" -target x86_64-w64-mingw32 -std=c++23 -O2 -Wall -Wextra -municode -static \
    -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008 \
    -I"$PILL_ROOT/common" -I"$here/build/addon/gen" \
    "$here/tools/run-host.cpp" -o "$out/run-host.exe" \
    -lwindowscodecs -lole32 -lgdi32 -luser32
echo "built $out/run-host.exe"
