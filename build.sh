#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the Run card add-on into build/addon with PillTaskbar's add-on tools and this folder's addon.conf, then
# adds settings.schema.json to the bundle. Targets as in PillTaskbar's tools/build-addon.sh (default all).
# PILL_ROOT is a PillTaskbar_BloomAddon checkout and SI_ROOT a SessionIsland_BloomAddon checkout (the transcript
# reader); without them sibling folders named PillTaskbar(_BloomAddon) and SessionIsland(_BloomAddon) are used.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
if [[ -z "${PILL_ROOT:-}" ]]; then
    for d in "$here/../PillTaskbar_BloomAddon" "$here/../PillTaskbar"; do
        if [[ -f "$d/tools/build-addon.sh" ]]; then PILL_ROOT="$d"; break; fi
    done
fi
if [[ -z "${PILL_ROOT:-}" || ! -f "$PILL_ROOT/tools/build-addon.sh" ]]; then
    echo "PillTaskbar_BloomAddon not found: set PILL_ROOT to a checkout of it" >&2
    exit 1
fi
PILL_ROOT="$(cd "$PILL_ROOT" && pwd)"
if [[ -z "${SI_ROOT:-}" ]]; then
    for d in "$here/../SessionIsland_BloomAddon" "$here/../SessionIsland"; do
        if [[ -f "$d/mod/si_store.h" ]]; then SI_ROOT="$d"; break; fi
    done
fi
export SI_ROOT
bash "$here/tools/stage-si.sh"
out="${OUT_DIR:-$here/build/addon}"
echo "tools: $PILL_ROOT"
ADDON_CONF="$here/addon.conf" OUT_DIR="$out" bash "$PILL_ROOT/tools/build-addon.sh" "$@"
if [[ -f "$out/addon.json" ]]; then
    # Session island's bundle finisher: the schema into addon.json, the same way for both add-ons.
    python -I "$SI_ROOT/tools/finish-bundle.py" "$out" "$here/settings.schema.json"
fi
