#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the card test program (tools/card-host.cpp) into build/test and runs it:
#   bash tools/build-card-host.sh [--en] [--accent RRGGBB]
# The picture goes to build/test/cards.png (cards-en.png with --en).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
bash "$here/tools/stage-si.sh"
cxx="${CXX:-/c/Program Files/Windhawk/Compiler/bin/clang++.exe}"
out="$here/build/test"
mkdir -p "$out"
"$cxx" -target x86_64-w64-mingw32 -std=c++23 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers \
    -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -municode -static \
    "$here/tools/card-host.cpp" -o "$out/card-host.exe" -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid
name="cards.png"
for a in "$@"; do [[ "$a" == "--en" ]] && name="cards-en.png"; done
"$out/card-host.exe" "$(cygpath -w "$out/$name")" "$@"
