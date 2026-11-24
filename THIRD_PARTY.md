# Task Cards add-on: sources and licenses

The add-on as a whole (this repository and its release zip) is distributed under the
GNU General Public License, version 3 only (`LICENSE`). It combines the parts below.

## The mod (GPL-3.0-only)

`mod/run-card.wh.cpp` and its headers `mod/rc_*.h` (built into `RunCard.dll`) and the test
program `tools/card-host.cpp` were written for this add-on. The card's layout follows a
run-status card design shared by @eliakuratli (a picture of a UI, no code).

## Claude's mark

The mark in each card's disc (`kClaudeMark` in `mod/rc_draw.h`) is Claude's logo, a trademark of
Anthropic PBC, shown only to say the cards are about Claude Code's sessions. Its path data is the
`claude.svg` of Simple Icons (https://simpleicons.org, data under CC0-1.0); the trademark stays its
owner's and is not under this add-on's license.

## The transcript reader (GPL-3.0-only, with MIT parts)

Following Claude Code's transcripts is the reader in `mod/si/`: `si_model.h`, `si_json.h`,
`si_lotmap.h`, `si_board.h` and `si_store.h`, with drawing helpers in `si_draw.h`. Parts of the
reader are lot-map v0.7.0 (https://github.com/alexeyw/lot-map, commit
09f6b93cd57e0ceb78cbcab9715a30b80c36388b) moved to C++, chiefly how a tool call is sorted and
labelled (`hooks/classify.ts`); they are marked in those headers. lot-map's license:

```
MIT License

Copyright (c) 2026 Aleksei Volodin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```


## The add-on runtime and host (GPL-3.0-only)

`RunCardRuntime.dll`, `RunCardHost.exe` and the add-on entry code built into
`RunCard.dll` come from PillTaskbar (https://github.com/hyeon1035298/PillTaskbar_BloomAddon),
GPL-3.0-only, built with this add-on's names from `addon.conf`. A release zip's `SOURCE.txt`
names the commit of this repository and of PillTaskbar_BloomAddon it was built from.

The runtime implements the part of the Windhawk mod API that the mod uses. It was written for
PillTaskbar and follows the behavior of Windhawk (https://github.com/ramensoftware/windhawk,
GPL-3.0).

The runtime and host contain or link these libraries. Their license texts are in
`THIRD_PARTY_NOTICES.txt` next to the binaries.

| Library | License | Used for |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (with HDE64) | BSD-2-Clause | function hooks (runtime) |
| [Zydis](https://github.com/zyantific/zydis) / Zycore 4.1.0 amalgamation | MIT | `Wh_Disasm` (runtime) and the symbol porter's instruction decoder (host) |
| mingw-w64 runtime | see `COPYING.MinGW-w64-runtime.txt` | C runtime startup code |
| libc++, libunwind, compiler-rt (LLVM 20) | Apache-2.0 WITH LLVM-exception | the statically linked C++ runtime |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | MIT | headers the mod includes |

## Microsoft symbols

The mod hooks nothing in the taskbar and names no symbols, so the add-on host skips symbol
preparation: it downloads no symbol file and connects to no symbol server. Nothing from
Microsoft is included in this repository or in the release zip.

## Fonts and icons

The add-on asks Windows for Segoe UI and Malgun Gothic, Cascadia Mono (Consolas without it),
and for the glyphs of Segoe Fluent Icons (Segoe MDL2 Assets on older systems). No font is
included. The mark on a card is a plain eight-ray spark drawn in code, not a company's logo.
