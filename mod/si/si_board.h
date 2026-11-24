// SPDX-License-Identifier: GPL-3.0-only
//
// Run card's transcript reader: transcript lines -> lot-map Board, the JavaScript prototype's lib/board.mjs in C++
// (register.tsx's semantics: begin / finish / background / settle, turn.start, turn.step, turn.complete, tool.call,
// prompt.submit, UserPromptSubmit and SessionStart context, skill.prompt, session.compact). Pure: no files, no
// clock; every time comes from the line's own timestamp.
//
// Privacy: what reaches the board is what lot-map's own classify makes of a tool's input (file name, host,
// command description or first word, skill name, agent description), fixed strings, ids and times. Message text,
// notification summaries and hook output are only tested for shape, never kept.

#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "si_json.h"
#include "si_lotmap.h"
#include "si_model.h"

namespace si {
namespace board {

// register.tsx constants (prototype lib/board.mjs)
constexpr size_t kKeep = 80;
constexpr size_t kCrew = 12;
constexpr Ms kLiveMs = 30 * 60 * 1000;  // a subagent silent this long is forgotten from Board::live (killed with no end)
constexpr size_t kLongLabel = 28;
constexpr Ms kNoteJoinMs = 5000;
// The live replay (map only, never the rows): a line read later than this after its own time would miss its
// pulse, so the map shows it when it arrives, one after another this far apart.
constexpr Ms kLateMs = 600;
constexpr Ms kStaggerMs = 110;
constexpr Ms kReplayMinMs = 150;
constexpr Ms kReplayMaxMs = 800;
constexpr size_t kIgnoredRequests = 256;

// ---- small JS semantics ------------------------------------------------------------------------------

// A transcript value as `===` sees it: two objects are never the same.
struct Scalar {
    enum class Kind : uint8_t { Undefined, Null, Bool, Number, String, Object } kind = Kind::Undefined;
    bool b = false;
    double n = 0;
    std::string s;
    bool Defined() const { return kind != Kind::Undefined; }
    bool Nullish() const { return kind == Kind::Undefined || kind == Kind::Null; }
    bool Is(std::string_view v) const { return kind == Kind::String && s == v; }
};
inline Scalar ScalarOf(json::Val v) {
    Scalar out;
    if (!v.Exists()) return out;
    switch (v.type()) {
        case json::Type::Null: out.kind = Scalar::Kind::Null; break;
        case json::Type::True:
        case json::Type::False:
            out.kind = Scalar::Kind::Bool;
            out.b = v.IsTrue();
            break;
        case json::Type::Number:
            out.kind = Scalar::Kind::Number;
            out.n = v.Num();
            break;
        case json::Type::String:
            out.kind = Scalar::Kind::String;
            out.s = std::string(v.Str());
            break;
        default: out.kind = Scalar::Kind::Object; break;
    }
    return out;
}
inline Scalar StringScalar(std::string_view s) {
    Scalar out;
    out.kind = Scalar::Kind::String;
    out.s = std::string(s);
    return out;
}
inline bool StrictEq(const Scalar& a, const Scalar& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Scalar::Kind::Undefined:
        case Scalar::Kind::Null: return true;
        case Scalar::Kind::Bool: return a.b == b.b;
        case Scalar::Kind::Number: return a.n == b.n;
        case Scalar::Kind::String: return a.s == b.s;
        default: return false;
    }
}
inline bool Truthy(json::Val v) {
    if (!v.Exists()) return false;
    switch (v.type()) {
        case json::Type::Null:
        case json::Type::False: return false;
        case json::Type::True: return true;
        case json::Type::Number: return v.Num() != 0 && !std::isnan(v.Num());
        case json::Type::String: return !v.Str().empty();
        default: return true;
    }
}
inline bool IsStr(json::Val v, std::string_view s) {
    return v.IsString() && v.Str() == s;
}
// Number.prototype.toString, closely enough for a tool name that is a number.
inline std::string JsNumber(double n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
    if (n == std::floor(n) && std::fabs(n) < 1e21) {
        char b[32];
        json::FormatNumbers(b, sizeof b, "%.0f", n);
        return b;
    }
    char b[40];
    for (int p = 1; p <= 17; p++) {
        json::FormatNumbers(b, sizeof b, "%.*g", p, n);
        if (json::ParseNumber(b) == n) break;
    }
    std::string s = b;
    size_t e = s.find('e');
    if (e != std::string::npos) {  // "1e-07" -> "1e-7", "1e+21" stays
        std::string mant = s.substr(0, e), exp = s.substr(e + 1);
        char sign = exp[0];
        exp = exp.substr(1);
        while (exp.size() > 1 && exp[0] == '0') exp.erase(0, 1);
        s = mant + "e" + sign + exp;
    }
    return s;
}
// String(v ?? '')
inline std::wstring JsString(json::Val v) {
    if (!v.Exists() || v.IsNull()) return L"";
    switch (v.type()) {
        case json::Type::String: return lotmap::Widen(v.Str());
        case json::Type::True: return L"true";
        case json::Type::False: return L"false";
        case json::Type::Number: return lotmap::Widen(JsNumber(v.Num()));
        case json::Type::Array: {
            std::wstring out;
            bool first = true;
            for (json::Val c = v.First(); c.Exists(); c = c.Next()) {
                if (!first) out += L',';
                first = false;
                out += JsString(c);
            }
            return out;
        }
        default: return L"[object Object]";
    }
}

// Date.parse for the ISO forms a transcript writes ("2026-11-15T01:00:00.000Z", an offset, or a date alone).
inline std::optional<Ms> ParseTime(std::string_view s) {
    auto digits = [&](size_t at, size_t n, int& out) {
        out = 0;
        if (at + n > s.size()) return false;
        for (size_t i = at; i < at + n; i++) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    auto days = [](int64_t y, int64_t m, int64_t d) {  // H. Hinnant's days_from_civil
        y -= m <= 2;
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        int64_t yoe = y - era * 400;
        int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    };
    int y, mo, d;
    if (!digits(0, 4, y) || s.size() < 10 || s[4] != '-' || !digits(5, 2, mo) || s[7] != '-' || !digits(8, 2, d) ||
        mo < 1 || mo > 12 || d < 1 || d > 31)
        return std::nullopt;
    if (s.size() == 10) return Ms(days(y, mo, d) * 86400000);
    int h = 0, mi = 0, sec = 0, ms = 0;
    if (s[10] != 'T' || !digits(11, 2, h) || s.size() < 16 || s[13] != ':' || !digits(14, 2, mi)) return std::nullopt;
    size_t i = 16;
    if (i < s.size() && s[i] == ':') {
        if (!digits(i + 1, 2, sec)) return std::nullopt;
        i += 3;
        if (i < s.size() && s[i] == '.') {
            size_t a = ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            if (i == a) return std::nullopt;
            int scale = 100;
            for (size_t k = a; k < i && k < a + 3; k++, scale /= 10) ms += (s[k] - '0') * scale;
        }
    }
    if (h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms))) return std::nullopt;
    int64_t local = days(y, mo, d) * 86400000 + int64_t(h) * 3600000 + int64_t(mi) * 60000 + int64_t(sec) * 1000 + ms;
    if (i == s.size()) {
        // No zone: local time, as Date.parse reads a date-time without one.
        SYSTEMTIME lt{}, ut{};
        lt.wYear = WORD(y);
        lt.wMonth = WORD(mo);
        lt.wDay = WORD(d);
        lt.wHour = WORD(h == 24 ? 0 : h);
        lt.wMinute = WORD(mi);
        lt.wSecond = WORD(sec);
        lt.wMilliseconds = WORD(ms);
        if (h == 24 || !TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut)) return local;
        FILETIME ft;
        if (!SystemTimeToFileTime(&ut, &ft)) return local;
        ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
        return Ms(u.QuadPart / 10000) - 11644473600000LL;
    }
    if (s[i] == 'Z' && i + 1 == s.size()) return local;
    if ((s[i] == '+' || s[i] == '-') && i + 6 == s.size() && s[i + 3] == ':') {
        int oh, om;
        if (!digits(i + 1, 2, oh) || !digits(i + 4, 2, om) || oh > 23 || om > 59) return std::nullopt;
        int64_t off = (int64_t(oh) * 60 + om) * 60000;
        return s[i] == '+' ? local - off : local + off;
    }
    return std::nullopt;
}

inline std::string Base36(uint64_t n) {
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string out;
    do {
        out.insert(out.begin(), digits[n % 36]);
        n /= 36;
    } while (n);
    return out;
}

// ---- a session's state ---------------------------------------------------------------------------------

// The context window in tokens for a model (0 = unknown); the model may be unknown too.
using WindowOf = std::function<double(const std::optional<std::string>&)>;

struct Step {
    std::string loop;
    std::optional<std::string> agentId;
    Scalar ref;  // the api-request's id (or the first block's requestRef)
    Scalar requestId;
    std::string entryId;
    bool gotBlocks = false;
    Ms lastTs = 0;
    std::optional<double> usage;  // input + cache read + cache creation, as contextOf adds them
    std::optional<std::string> model;
    std::optional<std::string> stop;  // stop_reason; "" for one that is not a string
    bool completed = false;
};

struct ToolRec {
    std::string entryId;
    std::wstring name;
    Ms startedAt = 0;
    std::optional<std::string> agentId;
};

struct Seen {
    std::optional<Ms> start, end;
};

struct Session {
    std::string id;
    Board board;
    std::optional<int> step;  // board.step: the main loop's step in its turn
    std::set<std::string> changedFiles;  // file_path of the main loop's writing calls in the turn (board.filesChanged)
    std::optional<std::wstring> title;
    std::optional<std::string> cwd;  // UTF-8
    Ms lastActivity = 0;
    std::optional<std::string> lastModel;
    bool turnOpen = false;
    std::unordered_map<std::string, ToolRec> tools;  // tool_use_id -> the entry it opened
    std::map<std::string, std::shared_ptr<Step>> openStep;  // loop ('' main, else agentId) -> step
    std::map<std::string, std::shared_ptr<Step>> lastStep;  // loop -> the step closed last
    std::map<std::string, int> stepIndex;                   // loop -> steps begun in the loop's current turn
    std::deque<std::string> ignoredRequests;
    std::map<std::string, Ms> notedAt;  // hook event -> ts of its last context row
    std::unordered_map<std::string, Seen> seen;  // entry id -> replay times for the map
    Ms lastSlot = 0;
    std::optional<Ms> arrival;  // when the lines being applied were read; none while loading history
    WindowOf windowOf;
    uint64_t seq = 0;
};

inline Session CreateSession(std::string id, WindowOf windowOf = nullptr) {
    Session s;
    s.id = std::move(id);
    s.windowOf = windowOf ? std::move(windowOf) : WindowOf([](const std::optional<std::string>&) { return 0.0; });
    return s;
}

// ---- the handlers' helpers, as in register.tsx ------------------------------------------------------

struct Patch {
    std::optional<std::string> model;
    std::optional<int> step;
    std::optional<Ms> switchedAt;
    std::optional<double> context;
};

struct Start {
    Station station = Station::You;
    std::wstring label;
    From from = From::Core;
    std::optional<Ms> startedAt;
    std::optional<Status> status;
    std::optional<Ms> endedAt;
    std::optional<std::string> agentId;
    std::string tool;  // the tool_use name, for a tool call
    Mark mark = Mark::None;
    bool uncounted = false;
    Patch patch;
};

inline Start MakeStart(Station station, std::wstring label, From from = From::Core,
                       std::optional<Status> status = std::nullopt) {
    Start st;
    st.station = station;
    st.label = std::move(label);
    st.from = from;
    st.status = status;
    return st;
}

inline bool IsFault(Status s) {
    return s == Status::Err || s == Status::Block;
}

inline void ApplyPatch(Session& s, const Patch& p) {
    if (p.model) {
        s.board.model = p.model;
        s.board.modelLabel = lotmap::ShortModel(*p.model);
    }
    if (p.step) {
        s.step = p.step;
        s.board.turnSteps = *p.step;
    }
    if (p.switchedAt) s.board.switchedAt = p.switchedAt;
    if (p.context) s.board.context = p.context;
}

// A question the session waits on (AskUserQuestion, ExitPlanMode): kept while it is open, whatever comes after it (its
// subagents' work can be more than kKeep rows), so the session still reads as asking (StatusOf) and its card keeps
// the question.
inline bool Asks(const Entry& e) {
    return e.status == Status::Run && (e.tool == "AskUserQuestion" || e.tool == "ExitPlanMode");
}

inline void Kept(std::vector<Entry>& entries) {
    if (entries.size() <= kKeep) return;
    size_t from = entries.size() - kKeep;
    std::vector<Entry> out;
    out.reserve(kKeep + 4);
    for (size_t i = 0; i < entries.size(); i++)
        if (i >= from || (entries[i].status == Status::Run && !entries[i].task.empty()) || Asks(entries[i]))
            out.push_back(std::move(entries[i]));
    entries = std::move(out);
}

// An entry ended: a subagent's tool call it was is open no more (Board::live), whether or not the entry is still kept.
inline void Unopen(Board& b, std::string_view id, Ms now) {
    for (auto& [agent, l] : b.live) {
        auto it = std::find(l.calls.begin(), l.calls.end(), id);
        if (it == l.calls.end()) continue;
        l.calls.erase(it);
        l.last = std::max(l.last, now);
        return;
    }
}

inline Entry* FindEntry(Session& s, std::string_view id, bool running) {
    for (auto& e : s.board.entries)
        if (e.id == id && (!running || e.status == Status::Run)) return &e;
    return nullptr;
}

inline void See(Session& s, const Entry& entry, bool start) {
    if (!s.arrival) return;
    std::optional<Ms> at = start ? std::optional<Ms>(entry.startedAt) : entry.endedAt;
    if (!at || *s.arrival - *at <= kLateMs) return;
    Ms slot = std::max(*s.arrival, s.lastSlot + kStaggerMs);
    s.lastSlot = slot;
    // Entries that left the board are dropped here too, not only when a view is made (a session out of view
    // keeps reading live lines).
    if (s.seen.size() > 2 * kKeep + 32) {
        for (auto it = s.seen.begin(); it != s.seen.end();)
            it = it->first != entry.id && FindEntry(s, it->first, false) == nullptr ? s.seen.erase(it) : std::next(it);
    }
    Seen& v = s.seen[entry.id];
    (start ? v.start : v.end) = slot;
}

inline std::string Begin(Session& s, Start start, Ms now) {
    Entry e;
    e.id = "e" + Base36(s.seq++);
    e.station = start.station;
    e.label = lotmap::Clean(start.label);
    e.from = start.from;
    e.tool = std::move(start.tool);
    e.startedAt = start.startedAt.value_or(now);
    e.status = start.status.value_or(Status::Run);
    if (start.status) e.endedAt = start.endedAt.value_or(now);
    if (start.agentId) e.agentId = *start.agentId;
    e.mark = start.mark;
    Board& b = s.board;
    ApplyPatch(s, start.patch);
    b.entries.push_back(e);
    Kept(b.entries);
    if (!start.uncounted) b.counts[Index(e.station)]++;
    if (e.station == Station::Memory && e.from == From::Inbound && !start.uncounted) b.pushed++;
    if (IsFault(e.status) && e.station != Station::You) b.faults[Index(e.station)]++;
    if (start.agentId && std::find(b.agents.begin(), b.agents.end(), *start.agentId) == b.agents.end()) {
        b.agents.push_back(*start.agentId);
        if (b.agents.size() > kCrew) b.agents.erase(b.agents.begin(), b.agents.end() - std::ptrdiff_t(kCrew));
    }
    if (start.agentId) {
        Board::Live& l = b.live[*start.agentId];
        l.last = std::max(l.last, e.endedAt.value_or(e.startedAt));
        if (!e.tool.empty() && e.status == Status::Run) l.calls.push_back(e.id);
        for (auto it = b.live.begin(); it != b.live.end();)
            it = e.startedAt - it->second.last > kLiveMs ? b.live.erase(it) : std::next(it);
    }
    See(s, e, true);
    if (e.endedAt) See(s, e, false);
    return e.id;
}

inline void Close(Session& s, Entry& e, Status status, Ms now) {
    e.status = status;
    e.endedAt = now;
    if (!e.agentId.empty()) Unopen(s.board, e.id, now);
    See(s, e, false);
}

inline void Finish(Session& s, const std::optional<std::string>& id, Status status, Ms now, const Patch& patch = {}) {
    if (!id) return;
    ApplyPatch(s, patch);
    if (Entry* open = FindEntry(s, *id, true)) {
        Close(s, *open, status, now);
        if (IsFault(status)) s.board.faults[Index(open->station)]++;
    } else {
        Unopen(s.board, *id, now);  // a subagent's call whose row left the kept entries
    }
}

inline void Background(Session& s, const std::string& id, const std::string& task) {
    if (Entry* e = FindEntry(s, id, true)) e->task = task;
}

inline void TurnComplete(Session& s, const std::optional<std::string>& agentId, bool aborted, Ms now);

inline void Settle(Session& s, const std::string& task, Status status, Ms now) {
    for (auto& e : s.board.entries) {
        if (e.task == task && e.status == Status::Run) {
            Close(s, e, status, now);
            if (IsFault(status)) s.board.faults[Index(e.station)]++;
            break;
        }
    }
    // A background agent's task id is its agent id: its notice also ends the agent (crew and open rows), one past
    // the crew's 12 too.
    auto& agents = s.board.agents;
    if (std::find(agents.begin(), agents.end(), task) != agents.end() || s.board.live.count(task))
        TurnComplete(s, task, false, now);
}

inline std::optional<double> ContextOf(Session& s, const std::optional<double>& used,
                                       const std::optional<std::string>& model) {
    if (!used) return std::nullopt;
    double window = s.windowOf ? s.windowOf(model) : 0;
    if (!(window > 0) || !(*used > 0)) return std::nullopt;
    return std::min(100.0, (100 * *used) / window);
}

// message.usage's three input counts added as contextOf adds them (a field that is not a number counts 0).
inline double UsageSum(json::Val usage) {
    double used = 0;
    for (const char* k : {"input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"}) {
        json::Val v = usage[k];
        used = used + (v.IsNumber() ? v.Num() : 0);
    }
    return used;
}

inline void NoteContext(Session& s, const std::string& hookEvent, std::wstring_view when, std::wstring_view text,
                        Ms now) {
    if (lotmap::TrimJs(text).empty()) return;
    auto last = s.notedAt.find(hookEvent);
    if (last != s.notedAt.end() && now - last->second < kNoteJoinMs) return;
    s.notedAt[hookEvent] = now;
    std::vector<std::wstring> ids;
    if (lotmap::BaseSetup().hookRecords) ids = lotmap::MemoryIds(text);
    if (ids.empty()) {
        Begin(s, MakeStart(Station::Hooks, lotmap::t::AddedContext(when), From::Core, Status::Ok), now);
        return;
    }
    Begin(s, MakeStart(Station::Hooks, lotmap::t::AddedMemory(when), From::Core, Status::Ok), now);
    std::wstring shown;
    for (size_t i = 0; i < ids.size() && i < 6; i++) shown += (i ? L" #" : L"#") + ids[i];
    if (ids.size() > 6) shown += L" \x2026";
    Begin(s, MakeStart(Station::Memory, lotmap::t::Records(ids.size(), shown), From::Inbound, Status::Ok), now);
}

// ---- lot-map events ----------------------------------------------------------------------------------

inline void TurnStart(Session& s, Ms now) {
    s.stepIndex[""] = 0;
    s.turnOpen = true;
    s.board.faults = {};
    s.board.turnStartedAt = now;
    s.board.turnEndedAt.reset();
    s.board.turnAborted = false;
    s.board.turnSteps = 0;
    s.changedFiles.clear();
    s.board.filesChanged = 0;
    Begin(s, MakeStart(Station::You, lotmap::t::kNewMessage, From::Inbound, Status::Ok), now);
}

inline std::string TurnStep(Session& s, const std::optional<std::string>& agentId, const std::string& model,
                            Ms startedAt) {
    bool isMain = !agentId;
    std::string loop = agentId.value_or("");
    int index = s.stepIndex[loop];
    s.stepIndex[loop] = index + 1;
    if (isMain && s.lastModel != model) {
        std::optional<std::string> before = s.lastModel;
        s.lastModel = model;
        if (before) {
            Start sw = MakeStart(Station::Model, lotmap::ShortModel(*before) + L" \x2192 " + lotmap::ShortModel(model));
            sw.status = Status::Ok;
            sw.mark = Mark::Switch;
            sw.uncounted = true;
            sw.patch.switchedAt = startedAt;
            Begin(s, sw, startedAt);
        }
    }
    Start st = MakeStart(Station::Model, lotmap::ShortModel(model) + L" \x00B7 " + lotmap::t::Step(index + 1) +
                                             (isMain ? L"" : std::wstring(L" \x00B7 ") + lotmap::t::kSubagent));
    st.from = isMain ? From::Core : From::Agents;
    if (isMain) {
        st.patch.model = model;
        st.patch.step = index + 1;
    } else {
        st.agentId = agentId;
    }
    return Begin(s, st, startedAt);
}

inline void TurnComplete(Session& s, const std::optional<std::string>& agentId, bool aborted, Ms now) {
    if (agentId) {
        const std::string& a = *agentId;
        auto& agents = s.board.agents;
        agents.erase(std::remove(agents.begin(), agents.end(), a), agents.end());
        for (auto& x : s.board.entries)
            if (x.status == Status::Run && (x.agentId == a || x.task == a)) Close(s, x, Status::Ok, now);
        // The agent's loop is over: forget its step bookkeeping (a session may run hundreds of workflow agents).
        s.board.live.erase(a);
        s.stepIndex.erase(a);
        s.lastStep.erase(a);
        s.openStep.erase(a);
        return;
    }
    Start done = MakeStart(Station::You, aborted ? lotmap::t::kTurnAborted : lotmap::t::kAnswerReady);
    done.status = aborted ? Status::Err : Status::Ok;
    if (!aborted) done.mark = Mark::Done;
    Begin(s, done, now);
    for (auto& x : s.board.entries)
        if (x.status == Status::Run && x.from == From::Core && x.task.empty()) Close(s, x, Status::Ok, now);
    s.turnOpen = false;
    s.board.turnEndedAt = now;
    s.board.turnAborted = aborted;
    for (auto it = s.tools.begin(); it != s.tools.end();) {
        Entry* e = FindEntry(s, it->second.entryId, true);
        it = e == nullptr ? s.tools.erase(it) : std::next(it);
    }
}

// A call that writes a file counts once per path in the turn, when it is made, whether or not it succeeds.
inline void CountChangedFile(Session& s, const std::wstring& tool, json::Val input) {
    if (tool != L"Edit" && tool != L"Write" && tool != L"MultiEdit" && tool != L"NotebookEdit") return;
    if (!input.IsObjectLike()) return;
    json::Val path = input["file_path"];
    if (!path.IsString()) path = input["notebook_path"];
    if (!path.IsString() || path.Str().empty()) return;
    s.changedFiles.emplace(path.Str());
    s.board.filesChanged = int(s.changedFiles.size());
}

inline void ToolCall(Session& s, json::Val toolUseId, json::Val name, json::Val input,
                     const std::optional<std::string>& agentId, Ms now) {
    std::wstring tool = JsString(name);
    lotmap::Classified seen = lotmap::Classify(tool, input.IsObjectLike() ? input : json::Val());
    Start st = MakeStart(seen.station, seen.label);
    st.tool = lotmap::Narrow(tool);
    if (agentId) {
        st.from = From::Agents;
        st.agentId = agentId;
    } else {
        CountChangedFile(s, tool, input);
    }
    std::string id = Begin(s, st, now);
    if (toolUseId.IsString()) s.tools[std::string(toolUseId.Str())] = ToolRec{id, tool, now, agentId};
}

inline void ToolResult(Session& s, json::Val block, json::Val line, Ms now) {
    json::Val useId = block["tool_use_id"];
    if (!useId.IsString()) return;
    auto found = s.tools.find(std::string(useId.Str()));
    if (found == s.tools.end()) return;
    ToolRec rec = found->second;
    s.tools.erase(found);
    json::Val decision = line["permissionDecision"];
    json::Val denial = line["toolDenialKind"];
    if (IsStr(denial, "interrupted")) {
        Finish(s, rec.entryId, Status::Ok, now);
        return;
    }
    bool rejected = IsStr(decision["decision"], "reject");
    bool hookDeny = rejected && IsStr(decision["source"], "hook");
    if (hookDeny) {
        Start st = MakeStart(Station::Hooks, lotmap::t::Stopped(rec.name));
        st.status = Status::Block;
        st.startedAt = rec.startedAt;
        Begin(s, st, now);
    }
    bool refused = hookDeny || rejected || IsStr(denial, "user-rejected") || IsStr(denial, "permission-rule") ||
                   IsStr(denial, "automode-blocked") || IsStr(denial, "automode-unavailable");
    bool isError = block["is_error"].IsTrue();
    std::optional<std::string> task;
    if (!refused && !isError) task = lotmap::TaskOf(line["toolUseResult"]);
    if (task) {
        Background(s, rec.entryId, *task);
        return;
    }
    Finish(s, rec.entryId, refused ? Status::Block : isError ? Status::Err : Status::Ok, now);
    // How a subagent can end without an end_turn: a workflow agent hands in its StructuredOutput and stops,
    // and a foreground Agent call's result names the agent it ran.
    if (rec.agentId && rec.name == L"StructuredOutput" && !refused && !isError) TurnComplete(s, rec.agentId, false, now);
    json::Val ran = line["toolUseResult"]["agentId"];
    if (!rec.agentId && ran.IsString()) {
        std::string a(ran.Str());
        auto& agents = s.board.agents;
        if (std::find(agents.begin(), agents.end(), a) != agents.end()) TurnComplete(s, a, false, now);
    }
}

inline void SkillPrompt(Session& s, std::wstring_view skill, Ms now) {
    std::wstring label = lotmap::Clean(skill);
    if (label.empty()) return;
    Board& b = s.board;
    for (auto const& x : b.entries)
        if (x.station == Station::Skills && x.status == Status::Run && x.label == label) return;
    Entry e;
    e.id = "e" + Base36(s.seq++);
    e.station = Station::Skills;
    e.label = label;
    e.from = From::Core;
    e.startedAt = now;
    e.endedAt = now;
    e.status = Status::Ok;
    b.entries.push_back(e);
    Kept(b.entries);
    b.counts[Index(Station::Skills)]++;
    See(s, e, true);
    See(s, e, false);
}

inline void Compact(Session& s, Ms startedAt, Ms now, std::optional<double> postTokens) {
    Start st = MakeStart(Station::Model, lotmap::t::kCompacting);
    st.startedAt = startedAt;
    std::string id = Begin(s, st, now);
    Patch p;
    p.context = ContextOf(s, postTokens.value_or(0), s.board.model ? s.board.model : s.lastModel);
    Finish(s, id, Status::Ok, now, p);
}

// ---- model steps from api-request + assistant lines --------------------------------------------------

inline std::shared_ptr<Step> StartStep(Session& s, const std::string& loop, const std::optional<std::string>& agentId,
                                       const std::optional<std::string>& model, Ms startedAt, Scalar ref) {
    std::string entryId = TurnStep(s, agentId, model ? *model : s.lastModel ? *s.lastModel : "model", startedAt);
    auto step = std::make_shared<Step>();
    step->loop = loop;
    step->agentId = agentId;
    step->ref = std::move(ref);
    step->entryId = entryId;
    step->lastTs = startedAt;
    step->model = model;
    s.openStep[loop] = step;
    if (!agentId) s.turnOpen = true;
    return step;
}

inline bool Matches(const Step& step, json::Val line) {
    Scalar ref = ScalarOf(line["requestRef"]);
    if (ref.Defined() && step.ref.Defined()) return StrictEq(ref, step.ref);
    Scalar rid = ScalarOf(line["requestId"]);
    if (rid.Defined() && step.requestId.Defined()) return StrictEq(rid, step.requestId);
    return !step.gotBlocks;
}

inline void CompleteIfEnded(Session& s, const std::shared_ptr<Step>& step) {
    if (step->completed || !step->stop) return;
    const std::string& stop = *step->stop;
    if (stop != "end_turn" && stop != "stop_sequence" && stop != "max_tokens") return;
    step->completed = true;
    TurnComplete(s, step->agentId, false, step->lastTs);
}

// The step's message is all in: the step ends, and with an end_turn so does the loop's turn.
inline void CloseStep(Session& s, const std::string& loop, Ms now) {
    auto it = s.openStep.find(loop);
    if (it == s.openStep.end()) return;
    std::shared_ptr<Step> step = it->second;
    s.openStep.erase(it);
    s.lastStep[loop] = step;
    Ms endedAt = step->gotBlocks ? step->lastTs : now;
    Patch p;
    if (!step->agentId) p.context = ContextOf(s, step->usage, step->model);
    Finish(s, step->entryId, Status::Ok, endedAt, p);
    CompleteIfEnded(s, step);
}

inline void Remember(std::deque<std::string>& set, const std::string& value) {
    if (std::find(set.begin(), set.end(), value) != set.end()) return;
    set.push_back(value);
    if (set.size() > kIgnoredRequests) set.pop_front();
}

inline void OnApiRequest(Session& s, json::Val line, const std::string& loop, const std::optional<std::string>& agentId,
                         Ms now) {
    json::Val qs = line["querySource"];
    if (!agentId && qs.IsString() && qs.Str() != "sdk" && qs.Str().substr(0, 16) != "repl_main_thread") {
        if (line["id"].IsString()) Remember(s.ignoredRequests, std::string(line["id"].Str()));
        return;
    }
    CloseStep(s, loop, now);
    json::Val m = line["params"]["model"];
    std::optional<std::string> model;
    if (m.IsString()) model = std::string(m.Str());
    Scalar ref;
    if (line["id"].IsString()) ref = StringScalar(line["id"].Str());
    StartStep(s, loop, agentId, model, now, ref);
}

inline void OnAssistant(Session& s, json::Val line, const std::string& loop, const std::optional<std::string>& agentId,
                        Ms now) {
    json::Val message = line["message"];
    if (IsStr(message["model"], "<synthetic>")) return;
    Scalar requestRef = ScalarOf(line["requestRef"]);
    if (requestRef.kind == Scalar::Kind::String &&
        std::find(s.ignoredRequests.begin(), s.ignoredRequests.end(), requestRef.s) != s.ignoredRequests.end())
        return;
    std::shared_ptr<Step> step;
    if (auto it = s.openStep.find(loop); it != s.openStep.end()) step = it->second;
    if (step && !Matches(*step, line)) {
        CloseStep(s, loop, now);
        step = nullptr;
    }
    bool reopened = false;
    if (!step) {
        auto it = s.lastStep.find(loop);
        if (it != s.lastStep.end() && it->second->gotBlocks && Matches(*it->second, line)) {
            step = it->second;
            reopened = true;
        }
    }
    json::Val model = message["model"];
    if (!step)
        step = StartStep(s, loop, agentId, model.IsString() ? std::optional<std::string>(std::string(model.Str())) : std::nullopt,
                         now, requestRef);
    step->gotBlocks = true;
    if (step->requestId.Nullish()) step->requestId = ScalarOf(line["requestId"]);
    step->lastTs = std::max(step->lastTs, now);
    json::Val usage = message["usage"];
    if (Truthy(usage)) step->usage = UsageSum(usage);
    if (model.IsString()) step->model = std::string(model.Str());
    json::Val stop = message["stop_reason"];
    if (stop.Exists() && !stop.IsNull()) step->stop = stop.IsString() ? std::string(stop.Str()) : std::string();
    if (!agentId) s.turnOpen = !step->completed;
    json::Val content = message["content"];
    if (content.IsArray()) {
        for (json::Val block = content.First(); block.Exists(); block = block.Next())
            if (IsStr(block["type"], "tool_use")) ToolCall(s, block["id"], block["name"], block["input"], agentId, now);
    }
    if (reopened) {
        // A block of a message read after its step was closed (a read that fell inside the write): the step reaches it.
        for (auto& e : s.board.entries)
            if (e.id == step->entryId && e.endedAt && *e.endedAt < step->lastTs) e.endedAt = step->lastTs;
        CompleteIfEnded(s, step);
    }
}

// ---- user lines --------------------------------------------------------------------------------------

// The text of a user line, only to test what kind of line it is. Never stored.
inline std::wstring TextOf(json::Val content) {
    if (content.IsString()) return lotmap::Widen(content.Str());
    std::wstring out;
    if (!content.IsArray()) return out;
    bool first = true;
    for (json::Val b = content.First(); b.Exists(); b = b.Next()) {
        if (!IsStr(b["type"], "text") || !b["text"].IsString()) continue;
        if (!first) out += L'\n';
        first = false;
        out += lotmap::Widen(b["text"].Str());
    }
    return out;
}

// /<command-name>\s*\/?([^<]{1,120}?)\s*<\/command-name>/.exec(text)?.[1]
// One pass per opening tag instead of the regex's backtracking, which is quadratic on a long run of
// spaces with no closing tag. Neither the name nor the spaces hold a '<', so the closing tag can only
// start at the first '<' after the opening one; the lazy name then ends at the last non-space before it.
inline std::optional<std::wstring> CommandName(std::wstring_view text) {
    static const std::wstring_view open = L"<command-name>", close = L"</command-name>";
    for (size_t at = text.find(open); at != std::wstring_view::npos; at = text.find(open, at + 1)) {
        size_t p = at + open.size();
        size_t e = text.find(L'<', p);
        if (e == std::wstring_view::npos) break;  // no closing tag, and no later opening tag either
        if (text.substr(e, close.size()) != close) continue;
        size_t q0 = p;  // \s* takes every space first
        while (q0 < e && lotmap::IsJsSpace(text[q0])) q0++;
        size_t end = e;  // after the last non-space before the closing tag
        while (end > p && lotmap::IsJsSpace(text[end - 1])) end--;
        // The name starting at q: as short as the trailing \s* allows, at least one character.
        auto name = [&](size_t q) -> std::optional<std::wstring> {
            if (q >= e) return std::nullopt;
            size_t k = end > q ? end - q : 1;
            if (k > 120) return std::nullopt;
            return std::wstring(text.substr(q, k));
        };
        if (q0 < e && text[q0] == L'/') {
            if (auto n = name(q0 + 1)) return n;
        }
        if (auto n = name(q0)) return n;
        // Giving \s* back one space only helps when nothing but spaces stand before the closing tag
        // (the name is then that last space); otherwise every shorter \s* makes the name longer still.
        if (q0 > p) {
            if (auto n = name(q0 - 1)) return n;
        }
    }
    return std::nullopt;
}

inline void OnUser(Session& s, json::Val line, const std::string& loop, const std::optional<std::string>& agentId,
                   Ms now) {
    if (line["isCompactSummary"].IsTrue()) return;
    json::Val content = line["message"]["content"];
    bool anyResult = false;
    if (content.IsArray()) {
        for (json::Val b = content.First(); b.Exists(); b = b.Next())
            if (IsStr(b["type"], "tool_result")) anyResult = true;
    }
    if (anyResult) {
        if (!agentId) s.turnOpen = true;
        for (json::Val b = content.First(); b.Exists(); b = b.Next())
            if (IsStr(b["type"], "tool_result")) ToolResult(s, b, line, now);
        return;
    }
    std::wstring text = TextOf(content);
    std::wstring_view head = lotmap::TrimStartJs(text);
    if (lotmap::StartsWith(head, L"[Request interrupted by user")) {
        CloseStep(s, loop, now);
        TurnComplete(s, agentId, true, now);
        return;
    }
    if (agentId) {
        // A subagent's turn.start does nothing on the map; its steps count from one again.
        if (!line["isMeta"].IsTrue()) s.stepIndex[loop] = 0;
        return;
    }
    Scalar origin = ScalarOf(line["origin"]["kind"]);
    if (origin.Is("task-notification") || lotmap::StartsWith(head, L"<task-notification>")) {
        // A background task reporting back is no message of the person's.
        s.stepIndex[""] = 0;
        s.turnOpen = true;
        auto notice = lotmap::TaskNotice(text);
        if (notice && notice->status) Settle(s, notice->id, *notice->status, now);
        return;
    }
    if (lotmap::StartsWith(head, L"<command-")) {
        if (auto name = CommandName(text)) {
            s.stepIndex[""] = 0;
            SkillPrompt(s, *name, now);
        }
        return;
    }
    if (line["isMeta"].IsTrue() && !origin.Is("peer")) return;
    if (origin.Defined() && !origin.Is("human") && !origin.Is("peer")) return;
    if (head.empty() && !(content.IsArray() && content.Count() > 0)) return;
    TurnStart(s, now);
}

// What one main-loop line, taken on its own, does to the turn by the rules of OnUser and CompleteIfEnded: a prompt
// that begins it, an interrupt or a final answer that ends it, or neither. Keep it in step with those two.
enum class TurnEdge { None, Start, End };
inline TurnEdge TurnEdgeOf(json::Val line) {
    if (!line.IsObjectLike()) return TurnEdge::None;
    json::Val ts = line["timestamp"];
    if (!ts.IsString() || !ParseTime(ts.Str())) return TurnEdge::None;  // ApplyLine skips it
    json::Val type = line["type"];
    if (IsStr(type, "assistant")) {
        json::Val message = line["message"];
        if (IsStr(message["model"], "<synthetic>")) return TurnEdge::None;
        json::Val stop = message["stop_reason"];
        bool ends = IsStr(stop, "end_turn") || IsStr(stop, "stop_sequence") || IsStr(stop, "max_tokens");
        return ends ? TurnEdge::End : TurnEdge::None;
    }
    if (!IsStr(type, "user") || line["isCompactSummary"].IsTrue()) return TurnEdge::None;
    json::Val content = line["message"]["content"];
    if (content.IsArray()) {
        for (json::Val b = content.First(); b.Exists(); b = b.Next())
            if (IsStr(b["type"], "tool_result")) return TurnEdge::None;
    }
    std::wstring text = TextOf(content);
    std::wstring_view head = lotmap::TrimStartJs(text);
    if (lotmap::StartsWith(head, L"[Request interrupted by user")) return TurnEdge::End;
    Scalar origin = ScalarOf(line["origin"]["kind"]);
    if (origin.Is("task-notification") || lotmap::StartsWith(head, L"<task-notification>")) return TurnEdge::None;
    if (lotmap::StartsWith(head, L"<command-")) return TurnEdge::None;
    if (line["isMeta"].IsTrue() && !origin.Is("peer")) return TurnEdge::None;
    if (origin.Defined() && !origin.Is("human") && !origin.Is("peer")) return TurnEdge::None;
    if (head.empty() && !(content.IsArray() && content.Count() > 0)) return TurnEdge::None;
    return TurnEdge::Start;
}

inline std::wstring HookText(json::Val content) {
    if (content.IsString()) return lotmap::Widen(content.Str());
    std::wstring out;
    if (!content.IsArray()) return out;
    bool first = true;
    for (json::Val c = content.First(); c.Exists(); c = c.Next()) {
        if (!c.IsString()) continue;
        if (!first) out += L'\n';
        first = false;
        out += lotmap::Widen(c.Str());
    }
    return out;
}

inline void OnAttachment(Session& s, json::Val line, Ms now) {
    json::Val a = line["attachment"];
    if (!a.IsObjectLike()) return;
    json::Val type = a["type"];
    if (IsStr(type, "queued_command") &&
        (IsStr(a["origin"]["kind"], "task-notification") || IsStr(a["commandMode"], "task-notification"))) {
        if (a["prompt"].IsString()) {
            auto notice = lotmap::TaskNotice(lotmap::Widen(a["prompt"].Str()));
            if (notice && notice->status) Settle(s, notice->id, *notice->status, now);
        }
        return;
    }
    json::Val event = a["hookEvent"];
    if ((IsStr(type, "hook_success") || IsStr(type, "hook_additional_context")) && IsStr(event, "UserPromptSubmit"))
        NoteContext(s, "UserPromptSubmit", lotmap::t::kToMessage, HookText(a["content"]), now);
    else if (IsStr(type, "hook_additional_context") && IsStr(event, "SessionStart"))
        NoteContext(s, "SessionStart", lotmap::t::kAtStart, HookText(a["content"]), now);
}

// ---- entry points ------------------------------------------------------------------------------------

// Applies one parsed transcript line. `fileAgentId` is set for a subagent's file (agent-<id>.jsonl). Lines
// without a timestamp (custom-title and the other meta lines) only update what they name.
inline void ApplyLine(Session& s, json::Val line, const std::optional<std::string>& fileAgentId) {
    if (!line.IsObjectLike()) return;
    std::optional<std::string> agentId;
    if (fileAgentId) {
        json::Val own = line["agentId"];
        // An empty agentId would read as the main loop here (Entry::agentId empty = main): the file's id stands.
        agentId = own.IsString() && !own.Str().empty() ? std::string(own.Str()) : *fileAgentId;
    }
    std::string loop = agentId.value_or("");
    json::Val type = line["type"];
    if (IsStr(type, "custom-title")) {
        json::Val title = line["customTitle"];
        if (!agentId && title.IsString()) {
            std::wstring text = lotmap::Widen(title.Str());
            if (!lotmap::TrimJs(text).empty()) s.title = lotmap::Clean(text, 80);
        }
        return;
    }
    json::Val ts = line["timestamp"];
    if (!ts.IsString()) return;
    std::optional<Ms> parsed = ParseTime(ts.Str());
    if (!parsed) return;
    Ms now = *parsed;
    if (now > s.lastActivity) s.lastActivity = now;
    json::Val cwd = line["cwd"];
    if (!agentId && cwd.IsString() && !cwd.Str().empty()) s.cwd = std::string(cwd.Str());
    if (IsStr(type, "api-request")) return OnApiRequest(s, line, loop, agentId, now);
    if (IsStr(type, "assistant")) return OnAssistant(s, line, loop, agentId, now);
    if (auto it = s.openStep.find(loop); it != s.openStep.end() && it->second->gotBlocks) CloseStep(s, loop, now);
    if (IsStr(type, "user")) return OnUser(s, line, loop, agentId, now);
    if (IsStr(type, "attachment")) return OnAttachment(s, line, now);
    if (IsStr(type, "queue-operation")) {
        json::Val content = line["content"];
        if (IsStr(line["operation"], "enqueue") && content.IsString()) {
            auto notice = lotmap::TaskNotice(lotmap::Widen(content.Str()));
            if (notice && notice->status) Settle(s, notice->id, *notice->status, now);
        }
        return;
    }
    if (IsStr(type, "system")) {
        if (IsStr(line["subtype"], "compact_boundary") && !agentId) {
            json::Val meta = line["compactMetadata"];
            json::Val d = meta["durationMs"];
            double took = d.IsNumber() && d.Num() > 0 ? std::min(d.Num(), 1e12) : 0;
            json::Val post = meta["postTokens"];
            Compact(s, now - Ms(std::llround(took)), now, post.IsNumber() ? std::optional<double>(post.Num()) : std::nullopt);
        }
    }
}

// The lines read in one go from one file are all in: a model message is written whole at once, so a step whose
// blocks were read is over. A step with no blocks yet (the model is still answering) stays open.
inline void EndChunk(Session& s, const std::optional<std::string>& fileAgentId) {
    std::string loop = fileAgentId.value_or("");
    auto it = s.openStep.find(loop);
    if (it != s.openStep.end() && it->second->gotBlocks) CloseStep(s, loop, it->second->lastTs);
}

// Sets the read time for the lines about to be applied: a time (live) or none (history).
inline void SetArrival(Session& s, std::optional<Ms> arrival) {
    s.arrival = arrival;
}

// ---- the view ----------------------------------------------------------------------------------------

// The board the map is drawn from: lines read late are shown when they arrived, so their pulses are seen.
inline Board DisplayBoard(Session& s) {
    if (s.seen.empty()) return s.board;
    for (auto it = s.seen.begin(); it != s.seen.end();)
        it = FindEntry(s, it->first, false) == nullptr ? s.seen.erase(it) : std::next(it);
    Board out = s.board;
    for (auto& e : out.entries) {
        auto v = s.seen.find(e.id);
        if (v == s.seen.end()) continue;
        Ms startedAt = v->second.start.value_or(e.startedAt);
        if (!e.endedAt) {
            e.startedAt = startedAt;
            continue;
        }
        Ms took = std::max<Ms>(0, *e.endedAt - e.startedAt);
        Ms least = startedAt + std::min(kReplayMaxMs, std::max(kReplayMinMs, took));
        Ms ended = std::max({v->second.end.value_or(*e.endedAt), *e.endedAt, least});
        e.startedAt = startedAt;
        e.endedAt = ended;
    }
    return out;
}

inline SessionState StatusOf(const Session& s) {
    auto running = [&](const std::string& id) {
        for (auto const& e : s.board.entries)
            if (e.id == id && e.status == Status::Run) return true;
        return false;
    };
    for (auto const& [k, rec] : s.tools)
        if ((rec.name == L"AskUserQuestion" || rec.name == L"ExitPlanMode") && running(rec.entryId))
            return SessionState::Ask;
    // The main loop resting while subagents, a workflow or a background task still run is work too.
    if (s.turnOpen) return SessionState::Work;
    for (auto const& e : s.board.entries)
        if (e.status == Status::Run) return SessionState::Work;
    return SessionState::Idle;
}

// The feed's rows as register.tsx's Pane draws them, newest first.
inline std::vector<Row> RowsOf(const Board& board) {
    std::unordered_map<std::string, Ms> turnMs;
    std::optional<Ms> askedAt;
    for (auto const& x : board.entries) {
        if (x.station == Station::You && x.from == From::Inbound && x.mark == Mark::None) askedAt = x.startedAt;
        if (x.mark == Mark::Done && askedAt) {
            turnMs[x.id] = x.startedAt - *askedAt;
            askedAt.reset();
        }
    }
    std::vector<Row> rows;
    for (auto const& f : lotmap::FoldRows(board.entries, size_t(kHistoryRows))) {
        const Entry& e = *f.entry;
        Row r;
        r.key = e.id;
        bool isSwitch = e.mark == Mark::Switch;
        r.glyph = isSwitch ? L'\x25C6' : e.status == Status::Run ? L'\x25CF' : e.status == Status::Ok ? L'\x00B7' : L'\x2715';
        r.status = e.status;
        r.station = e.station;
        r.label = e.label;
        auto whole = turnMs.find(e.id);
        if (!e.endedAt) r.time = lotmap::t::kRunning;
        else if (whole != turnMs.end() && whole->second > 0) r.time = lotmap::FormatMs(whole->second);
        else if (f.tookMs > 0) r.time = lotmap::FormatMs(f.tookMs);
        else r.time = L"\x2014";
        r.fold = f.times - 1;
        r.longLabel = e.label.size() > kLongLabel;
        r.mark = e.mark;
        rows.push_back(std::move(r));
    }
    return rows;
}

inline std::wstring LastSegment(std::string_view p) {
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == '/' || p[end - 1] == '\\')) end--;
    size_t start = end;
    while (start > 0 && p[start - 1] != '/' && p[start - 1] != '\\') start--;
    return lotmap::Widen(p.substr(start, end - start));
}

// One SessionView (prototype viewOf without the svg); mapVersion is the caller's.
// Deliberately not the prototype's: while the cwd is not known yet, the project and the title stay empty (the caller
// then shows no title). The prototype falls back to the projects subfolder's name, which
// is the whole cwd with its separators turned into '-' ("C--Users-<name>-..."), and even its last '-' part is only a
// guess at the folder's name that can be the user's own name (a session in the home folder).
inline SessionView ViewOf(Session& s) {
    SessionView v;
    v.id = s.id;
    std::wstring project = s.cwd ? LastSegment(*s.cwd) : std::wstring();
    v.title = s.title ? *s.title : project;
    v.project = project;
    v.model = s.board.model ? lotmap::ShortModel(*s.board.model) : std::wstring();
    v.status = StatusOf(s);
    if (s.board.context) v.context = lotmap::JsRound(*s.board.context * 10) / 10;
    v.lastActivity = s.lastActivity;
    v.agents = int(s.board.agents.size());
    v.map = DisplayBoard(s);
    lotmap::NowParts(s.board, v.nowLeft, v.nowRight);
    v.rows = RowsOf(s.board);
    return v;
}

// ---- equality, for "did anything visible change" -----------------------------------------------------

inline bool SameEntry(const Entry& a, const Entry& b) {
    return a.id == b.id && a.station == b.station && a.label == b.label && a.from == b.from &&
           a.startedAt == b.startedAt && a.endedAt == b.endedAt && a.status == b.status && a.agentId == b.agentId &&
           a.task == b.task && a.mark == b.mark;
}
inline bool SameBoard(const Board& a, const Board& b) {
    if (a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); i++)
        if (!SameEntry(a.entries[i], b.entries[i])) return false;
    return a.counts == b.counts && a.model == b.model && a.modelLabel == b.modelLabel && a.pushed == b.pushed &&
           a.faults == b.faults && a.agents == b.agents && a.context == b.context && a.switchedAt == b.switchedAt;
}
// What the card reads and the map does not draw: kept apart from SameBoard so mapVersion (the board's redraw) is
// not bumped by it.
inline bool SameTurn(const Board& a, const Board& b) {
    return a.turnStartedAt == b.turnStartedAt && a.turnEndedAt == b.turnEndedAt && a.turnAborted == b.turnAborted &&
           a.turnSteps == b.turnSteps && a.filesChanged == b.filesChanged && a.live == b.live;
}
inline bool SameRow(const Row& a, const Row& b) {
    return a.key == b.key && a.glyph == b.glyph && a.status == b.status && a.station == b.station &&
           a.label == b.label && a.time == b.time && a.fold == b.fold && a.longLabel == b.longLabel && a.mark == b.mark;
}
// Everything a view shows: not mapVersion (which follows `map`), and not lastActivity, which only orders the
// sessions (every line with a timestamp moves it). The caller compares the sessions in order, so a new
// order is still a change through `id`.
inline bool SameView(const SessionView& a, const SessionView& b) {
    if (a.rows.size() != b.rows.size()) return false;
    for (size_t i = 0; i < a.rows.size(); i++)
        if (!SameRow(a.rows[i], b.rows[i])) return false;
    return a.id == b.id && a.title == b.title && a.project == b.project && a.model == b.model && a.status == b.status &&
           a.context == b.context && a.agents == b.agents && a.nowLeft == b.nowLeft && a.nowRight == b.nowRight &&
           SameBoard(a.map, b.map) && SameTurn(a.map, b.map);
}

}  // namespace board
}  // namespace si
