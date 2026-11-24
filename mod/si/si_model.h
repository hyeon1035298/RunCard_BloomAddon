// SPDX-License-Identifier: GPL-3.0-only
//
// Run card's transcript reader: what the reader hands the cards.
// The shapes follow lot-map v0.7.0 (MIT, Copyright (c) the lot-map authors; THIRD_PARTY.md):
// types/index.d.ts (Entry, Board), hooks/setup.ts (STATIONS order), hooks/text.ts (English station
// names), and the JavaScript prototype's CONTRACT.md (Session, Row).
//
//   si_json.h, si_lotmap.h, si_board.h, si_store.h  the reader: transcripts -> Snapshot (a thread of its own)
//   si_draw.h                                       drawing helpers the cards use with Direct2D/DirectWrite
// Nothing here touches a file, a clock or a window.

#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace si {

// Milliseconds since 1970 (UTC). Every time on a board is one of these, from a transcript line's own timestamp.
using Ms = int64_t;

// ---- stations -------------------------------------------------------------------------------------
// lot-map's STATIONS order (setup.ts): the order round the ring, clockwise from the top.

enum class Station : uint8_t { You, Model, Memory, Files, Shell, Web, Browser, Agents, Skills, Hooks };
constexpr int kStations = 10;

// lot-map's English names (text.ts, `stations`). The map and the rows use these.
inline constexpr const wchar_t* kStationNames[kStations] = {L"You",   L"Model",   L"Memory", L"Files",  L"Shell",
                                                            L"Network", L"Browser", L"Agents", L"Skills", L"Hooks"};

constexpr int Index(Station s) {
    return int(s);
}

// ---- the board: lot-map's types/index.d.ts --------------------------------------------------------

enum class Status : uint8_t { Run, Ok, Err, Block };
// Where the pulse starts: the core, the agents station, or the station itself (a signal coming in).
enum class From : uint8_t { Core, Agents, Inbound };
// A moment the map marks: the model changed, or the answer is ready.
enum class Mark : uint8_t { None, Switch, Done };

struct Entry {
    std::string id;  // board-local ("e1a"); the same entry keeps it as the board changes
    Station station = Station::You;
    std::wstring label;  // lot-map clean(): one line, at most 120 characters
    From from = From::Core;
    Ms startedAt = 0;
    std::optional<Ms> endedAt;  // absent while it runs
    Status status = Status::Run;
    std::string agentId;  // the subagent whose loop made it; empty for the main loop
    std::string task;     // the background task it started; empty for none
    std::string tool;     // the tool_use name ("Read", "Bash", "mcp__x__y"); empty for what is not a tool call
    Mark mark = Mark::None;
};

struct Board {
    std::vector<Entry> entries;  // oldest first, as lot-map keeps them
    std::array<int, kStations> counts{};
    std::optional<std::string> model;  // the model id as the transcript has it ("claude-opus-5-5")
    std::wstring modelLabel;           // lot-map shortModel(model) ("opus 5.5"); empty while `model` is unset
    int pushed = 0;                    // of the memory count, how many a hook brought unasked
    std::array<int, kStations> faults{};  // errors and refusals per station since the person's last message
    std::vector<std::string> agents;      // subagents still at work, at most 12
    // Every subagent still at work (`agents` has the newest 12 for the map): when it last wrote, and the entries of
    // its tool calls still open. Kept outside `entries`, which keep the newest 80 (a busy session's other agents can
    // push a slow one's rows out). Run Card goes by it; not drawn.
    struct Live {
        Ms last = 0;
        std::vector<std::string> calls;
        bool operator==(const Live&) const = default;
    };
    std::map<std::string, Live> live;
    std::optional<double> context;        // how full the context window is, 0 to 100
    std::optional<Ms> switchedAt;         // when the model last changed
    // The current turn, as fields so the 80-entry trim never loses them. They are not drawn: the map and the rows
    // ignore them, and board::SameView (not SameBoard) compares them, so a change only makes a new snapshot.
    std::optional<Ms> turnStartedAt;  // the human's or a peer's prompt that began it
    std::optional<Ms> turnEndedAt;    // when it completed (answered or interrupted); absent while it runs
    bool turnAborted = false;         // it ended on an interrupt
    int turnSteps = 0;                // model steps of the main loop so far
    int filesChanged = 0;             // distinct files the main loop's Edit/Write/MultiEdit/NotebookEdit calls named
};

// ---- a session as the reader hands it over: CONTRACT.md Session and Row -----------------------------

enum class SessionState : uint8_t { Idle, Work, Ask };

// One row under the map: lot-map foldRows(board.entries) in its order (newest first).
struct Row {
    std::string key;  // the entry id the row shows
    wchar_t glyph = L'\x00B7';  // L'\x25C6' (model switch) / L'\x25CF' run / L'\x00B7' ok / L'\x2715' err and block
    Status status = Status::Ok;
    Station station = Station::You;  // its name is kStationNames[]
    std::wstring label;              // lot-map clean(), at most 120 characters
    std::wstring time;  // "running", lot-map formatMs ("2.4 s", "1 min 5 s", "12 ms"), or L"\x2014" for a moment
    int fold = 0;       // rows folded into this one (shown as "  +N" after the label)
    bool longLabel = false;  // more than 28 characters (the Pane's expand control)
    Mark mark = Mark::None;
};

struct SessionView {
    std::string id;         // the transcript's file name (a uuid)
    std::wstring title;     // custom title, else the project. Shown only; never logged or written to a file
    std::wstring project;   // the cwd's last part
    std::wstring model;     // shortModel of the main loop's model; empty before the first step
    SessionState status = SessionState::Idle;
    std::optional<double> context;  // 0..100, one decimal
    Ms lastActivity = 0;
    int agents = 0;  // subagents at work
    // The board the map is drawn from: lot-map's board with lines that were read late moved to when they
    // were read (prototype displayBoard), so their pulses are seen.
    Board map;
    // Changes whenever `map` does; the same value means the same board.
    uint64_t mapVersion = 0;
    std::wstring nowLeft;   // lot-map nowParts().what: the line of now ("Shell: npm test", "Nothing is running.")
    std::wstring nowRight;  // nowParts().meter ("42% · opus 5.5"); the desktop Pane does not show it
    std::vector<Row> rows;  // newest first, at most kHistoryRows
};

constexpr int kHistoryRows = 19;  // prototype lib/board.mjs HISTORY_ROWS
constexpr int kMaxSessions = 6;   // prototype lib/store.mjs MAX_SESSIONS

struct Snapshot {
    Ms now = 0;                         // when the reader made it
    std::vector<SessionView> sessions;  // active only, most recent activity first, at most kMaxSessions
    // The reader's last word: it hit something it could not get past and has stopped (sessions empty).
    bool stopped = false;
};

}  // namespace si
