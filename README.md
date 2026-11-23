# Run Card (Bloom add-on)

While Claude Code sessions work, a small pill shows for each one on a side of the screen with what
it is doing and how long it has taken. Point at a pill and it opens where it is into a card: the
model, the step it is on, a bar of the steps, the last five steps and how long each took. The
design follows the run-status card shared by @eliakuratli; colours and type are
[Bloom](https://github.com/hyeon1035298/Bloom)'s.

- Permission requests and Claude's questions open the card by themselves: allow, deny or pick an
  answer right there. "In terminal" has Bloom bring the session's window forward. This goes
  through the approval hook Bloom already uses, so nothing is installed into Claude Code.
- A finished turn stays for a while as "Done in 13s · 6 files changed", then goes; right-click
  closes it at once.
- Pick a monitor (the main one or a Windows display number) and one of eight places (the four
  corners, top and bottom centre, left and right middle). Several sessions stack from that place
  in the order they came, and the others move smoothly as one opens or folds; the middle places
  spread evenly up and down.
- While the add-on is connected, Bloom leaves out its own Claude Code notices (working, done,
  waiting for approval). Codex and usage notices stay. Turn the add-on off and Bloom's notices
  come back.
- It reads the transcripts Claude Code writes (`%USERPROFILE%\.claude\projects`, other config
  folders and Bloom's account folders too) and uses no tokens. It shows tool names, the last part
  of a file path, a web address's host and the short descriptions the model writes; no message
  text. Nothing is sent anywhere (it talks to Bloom only, on this PC).
- With a full-screen app in front it hides, and requests that come meanwhile go to Bloom's card.

## Settings (in Bloom)

| | |
|---|---|
| Monitor | main, display 1–4 |
| Place | eight places (top right) |
| Margin | 0–200 (16) |
| Open | when pointed at, always |
| Keep a finished card | 0–600 s (20) |
| Answer approvals and questions on the card | on |
| Theme | Windows taskbar theme, dark, light |

Answering on the card needs a Bloom with the run-card link (the version after 1.4.3). With an
older Bloom the cards still show and approvals come as Bloom's cards.

## Build

`bash build.sh` with PillTaskbar_BloomAddon's add-on tools and SessionIsland_BloomAddon's
transcript reader next to this folder (or `PILL_ROOT`, `SI_ROOT`). It writes `build/addon`:
`RunCardHost.exe`, `RunCardRuntime.dll`, `RunCard.dll`, `addon.json`, `settings.schema.json` and
the license notices.

Tests: `bash tools/build-card-host.sh` (the cards drawn to PNG, model checks) and
`bash tools/build-run-host.sh`, a program that runs the runtime and the mod outside explorer with
a stand-in Bloom that checks answers to approvals and questions.

## License

GPL-3.0-only. Sources and credits are in `THIRD_PARTY.md`.
