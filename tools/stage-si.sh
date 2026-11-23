#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Puts Session island's transcript reader headers into mod/si/, where the mod and the test program include them
# from. They are Session island's, kept in its repository: mod/si/ is a build copy (git-ignored), made fresh each
# build so it never drifts. SI_ROOT is a SessionIsland_BloomAddon checkout; without it a sibling folder named
# SessionIsland_BloomAddon or SessionIsland is used.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${SI_ROOT:-}" ]]; then
    for d in "$here/../SessionIsland_BloomAddon" "$here/../SessionIsland"; do
        if [[ -f "$d/mod/si_store.h" ]]; then SI_ROOT="$d"; break; fi
    done
fi
if [[ -z "${SI_ROOT:-}" || ! -f "$SI_ROOT/mod/si_store.h" ]]; then
    echo "SessionIsland_BloomAddon not found: set SI_ROOT to a checkout of it" >&2
    exit 1
fi
mkdir -p "$here/mod/si"
for h in si_model.h si_json.h si_lotmap.h si_board.h si_store.h si_draw.h; do
    cp "$SI_ROOT/mod/$h" "$here/mod/si/$h"
done
echo "reader: $(cd "$SI_ROOT" && pwd)"
