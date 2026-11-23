// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: what one card says, made from the session the transcript reader hands over (si::SessionView, from
// Session island's reader) and, while the session waits on Bloom, the request Bloom passed on (Ask).
//
// The card covers the current turn only: the steps since the person's last message. A step is one tool call, or a
// run of calls of the same kind one after another ("Read 12 files"); its label says what was done in a few words.
// Labels carry only what the reader already keeps: a file's last two path parts, a host, a command's or an agent's
// own short description, a skill's name. Nothing here touches a window, a file or the clock: `now` is passed in.

#pragma once

#include "si/si_json.h"
#include "si/si_lotmap.h"
#include "si/si_model.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rc {

using si::Ms;

// ---- what the card shows -------------------------------------------------------------------------------------

enum class Icon : uint8_t { Read, Search, Edit, Shell, Web, Agent, Skill, Question, Plan, Mcp, Todo, Other, Think, Wait };

// Where the turn is. Ask and Approve both wait for the person: Ask is read from the transcript (a question or a plan
// with no answer yet), Approve is Bloom's request (a permission, or a question to answer on the card).
enum class Phase : uint8_t { Work, Ask, Approve, Done, Stopped };

struct Step {
    Icon icon = Icon::Other;
    std::wstring done;     // the step finished: "Read 12 files"
    std::wstring running;  // the step under way: "Reading 12 files"
    Ms startedAt = 0;
    std::optional<Ms> endedAt;  // absent while it runs
    bool failed = false;        // an error, or refused
    bool sub = false;           // a sub's step: a subagent's, or a T3 sub-session's (drawn with its tag)
    std::string subOf;          // ... which sub: "agent:<its id>", "session:<its id>" or "thread:<its T3 thread>"
    std::string subAlso;        // ... the same sub's other name: its T3 thread beside its session (one number for both)
    int subNo = 0;              // ... its number on the card for the turn ("[S1]"; NumberSubs); 0: not numbered
    bool asks = false;          // ... a T3 sub's one step saying it waits on an answer (kept with card.subtasks off)
};

// One question of AskUserQuestion, as Bloom passes it on.
struct Choice {
    std::wstring label, description;
};
struct Question {
    std::wstring header, text;
    bool multi = false;
    std::vector<Choice> choices;
};

// A request from Bloom waiting on this session's card.
struct Ask {
    std::string id;
    bool inRow = false;  // a T3 session's question (Bloom's `question`): answered through the row to type in, not Bloom's request
    std::wstring tool;  // "Bash", "Edit", "AskUserQuestion"
    std::wstring body;  // every field of the call as "name: value", one per line (empty for questions)
    std::vector<Question> questions;
    std::vector<std::vector<int>> picks;  // the choices picked so far, per question
    int page = 0;                         // the question shown: several are shown one at a time
    // A sub's request on the card of the session it works for: the line saying whose it is ("하위 · 문서 정리"), and the
    // sub-session it came from (a T3 sub's: its answers go there; empty for a subagent's, which asks under the card's).
    std::wstring sub;
    std::string from;
    bool sourced = false;  // whose it is is settled (a subagent's request is told from the board once it shows there)
    bool Answered() const {
        if (questions.empty()) return false;
        for (size_t i = 0; i < questions.size(); i++)
            if (i >= picks.size() || picks[i].empty()) return false;
        return true;
    }
    // The question to go to once question `from` is answered: the next one not answered after it, else before it;
    // -1 when every one is.
    int NextOpen(int from) const {
        int n = int(questions.size());
        for (int k = 1; k < n; k++) {
            int i = (from + k) % n;
            if (i >= int(picks.size()) || picks[i].empty()) return i;
        }
        return -1;
    }
};

struct Card {
    std::string id;       // the session's id (its transcript's name)
    std::wstring title;   // the session's title, else its folder
    std::wstring model;   // "Claude Sonnet 5.5"
    Phase phase = Phase::Work;
    Ms turnStart = 0;
    std::optional<Ms> turnEnd;
    std::vector<Step> steps;  // the turn's steps, oldest first
    int filesChanged = 0;
    bool asking = false;      // the transcript shows a question or a plan waiting (Phase::Ask while it lasts)
    bool askingPlan = false;  // ... and it is a plan
    bool stoppable = false;   // a session in a terminal that Bloom can stop (Esc into its console): the Stop button
    bool stopping = false;    // Stop was just pressed: the button waits, and is not pressed twice
    // 보내기 (a T3 session's card): words for the session, typed in the buttons' row.
    bool canSay = false;      // Bloom can send this session words: the 보내기 button
    bool composing = false;   // the row is the box to type in (Stop and Open hidden)
    bool focused = false;     // the window's edit box is over it, taking the keys
    bool typed = false;       // there are words in it (its send button lit)
    std::wstring draft;       // the words in it while the edit box is elsewhere (drawn)
    std::wstring note;        // what came of a send, in the row for a while ("보냈어요")
    bool noteBad = false;     // ... and it did not go
    // Its subs (T3 sub-sessions, and its own subagents; their steps are among `steps`, marked).
    int kidsRunning = 0;      // sub-sessions still at work: the card stays while there are any
    int agentsRunning = 0;    // its own subagents still at work (background agents, workflow agents: past the turn too)
    Ms subsEnd = 0;           // when the last of its subs' steps shown ended (WorkOn; its subs may be hidden after)
    std::wstring subAsk;      // the row answers a sub's question: whose ("하위 · 문서 정리"); empty: none
};

// ---- words ------------------------------------------------------------------------------------------------------

// The card's text in Korean or English (the add-ons go by Windows' display language).
inline std::wstring Tr(bool ko, std::wstring_view k, std::wstring_view e) {
    return std::wstring(ko ? k : e);
}

inline std::wstring Num(long long n) {
    return std::to_wstring(n);
}

// "Claude Sonnet 5.5" from "claude-sonnet-5-5" (a date suffix and "[1m]" left out); other ids as they are.
inline std::wstring ModelName(std::string_view id) {
    std::string s(id);
    if (size_t b = s.find('['); b != std::string::npos) s.resize(b);
    std::vector<std::string> parts;
    for (size_t at = 0; at <= s.size();) {
        size_t dash = s.find('-', at);
        if (dash == std::string::npos) dash = s.size();
        parts.push_back(s.substr(at, dash - at));
        at = dash + 1;
    }
    auto digits = [](const std::string& p) {
        return !p.empty() && std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (parts.size() < 3 || parts[0] != "claude") return std::wstring(s.begin(), s.end());
    std::string out = "Claude";
    std::string version;
    for (size_t i = 1; i < parts.size(); i++) {
        const std::string& p = parts[i];
        if (digits(p)) {
            if (p.size() >= 6) break;  // a date
            version += (version.empty() ? "" : ".") + p;
        } else if (!p.empty()) {
            out += " " + std::string(1, char(toupper(p[0]))) + p.substr(1);
        }
    }
    if (!version.empty()) out += " " + version;
    return std::wstring(out.begin(), out.end());
}

// A step's length: "0.4s", "1.9s", "12s", "1m 5s" / "0.4초", "1분 5초". A running step counts whole seconds.
inline std::wstring Duration(bool ko, Ms ms, bool running) {
    if (ms < 0) ms = 0;
    if (ms < 10000 && !running) {
        long long tenths = (ms + 50) / 100;
        std::wstring v = Num(tenths / 10) + L"." + Num(tenths % 10);
        return v + (ko ? L"초" : L"s");
    }
    long long s = ms / 1000;
    if (s < 60) return Num(s) + (ko ? L"초" : L"s");
    long long m = s / 60;
    s %= 60;
    if (m < 60) return ko ? Num(m) + L"분 " + Num(s) + L"초" : Num(m) + L"m " + Num(s) + L"s";
    return ko ? Num(m / 60) + L"시간 " + Num(m % 60) + L"분" : Num(m / 60) + L"h " + Num(m % 60) + L"m";
}

// The turn's clock in the header: "0:13", "12:04", "1:02:09".
inline std::wstring Clock(Ms ms) {
    long long s = std::max<Ms>(ms, 0) / 1000;
    auto two = [](long long v) { return (v < 10 ? L"0" : L"") + Num(v); };
    if (s < 3600) return Num(s / 60) + L":" + two(s % 60);
    return Num(s / 3600) + L":" + two(s / 60 % 60) + L":" + two(s % 60);
}

// ---- steps from the reader's entries ----------------------------------------------------------------------------

// A tool's kind: calls of one kind in a row fold into one step.
inline Icon KindOf(std::string_view tool) {
    if (tool == "Read" || tool == "NotebookRead") return Icon::Read;
    if (tool == "Grep" || tool == "Glob" || tool == "LS" || tool == "ToolSearch") return Icon::Search;
    if (tool == "Edit" || tool == "MultiEdit" || tool == "Write" || tool == "NotebookEdit") return Icon::Edit;
    if (tool == "Bash" || tool == "PowerShell" || tool == "BashOutput" || tool == "KillShell" || tool == "Monitor")
        return Icon::Shell;
    if (tool == "WebFetch" || tool == "WebSearch") return Icon::Web;
    if (tool == "Agent" || tool == "Task" || tool == "Workflow" || tool == "SendMessage") return Icon::Agent;
    if (tool == "Skill") return Icon::Skill;
    if (tool == "AskUserQuestion") return Icon::Question;
    if (tool == "ExitPlanMode" || tool == "EnterPlanMode") return Icon::Plan;
    if (tool == "TodoWrite" || tool.starts_with("Task")) return Icon::Todo;
    if (tool.starts_with("mcp__")) return Icon::Mcp;
    return Icon::Other;
}

namespace detail {

// The calls of one step.
struct Run {
    Icon icon = Icon::Other;
    std::string tool;               // the first call's tool
    std::vector<std::wstring> labels;  // each call's label, as the reader made it
    Ms startedAt = 0;
    std::optional<Ms> endedAt;
    bool running = false, failed = false;
    int calls = 0;
    size_t order = 0;  // its first call's place on the board
};

inline int Distinct(const std::vector<std::wstring>& labels) {
    std::vector<std::wstring> seen;
    for (auto& l : labels)
        if (!l.empty() && std::find(seen.begin(), seen.end(), l) == seen.end()) seen.push_back(l);
    return int(seen.size());
}

// A "server: tool" name from mcp__server__tool.
inline std::wstring McpName(std::string_view tool) {
    std::string t(tool.substr(5));
    size_t cut = t.find("__");
    std::string out = cut == std::string::npos ? t : t.substr(0, cut) + ": " + t.substr(cut + 2);
    return std::wstring(out.begin(), out.end());
}

inline Step Words(const Run& r, bool ko) {
    Step s;
    s.icon = r.icon;
    s.startedAt = r.startedAt;
    s.endedAt = r.endedAt;
    s.failed = r.failed;
    const std::wstring one = r.labels.empty() ? L"" : r.labels.back();
    int n = Distinct(r.labels);
    std::wstring count = Num(r.calls);
    switch (r.icon) {
        case Icon::Read:
            if (n > 1) {
                s.done = Tr(ko, L"파일 " + Num(n) + L"개 읽음", L"Read " + Num(n) + L" files");
                s.running = Tr(ko, L"파일 " + Num(n) + L"개 읽는 중", L"Reading " + Num(n) + L" files");
            } else {
                s.done = Tr(ko, one + L" 읽음", L"Read " + one);
                s.running = Tr(ko, one + L" 읽는 중", L"Reading " + one);
            }
            break;
        case Icon::Search:
            s.done = r.calls > 1 ? Tr(ko, L"검색 " + count + L"번", L"Searched " + count + L" times")
                                 : Tr(ko, L"검색", L"Searched");
            s.running = Tr(ko, L"검색하는 중", L"Searching");
            break;
        case Icon::Edit:
            if (n > 1) {
                s.done = Tr(ko, L"파일 " + Num(n) + L"개 편집함", L"Edited " + Num(n) + L" files");
                s.running = Tr(ko, L"파일 " + Num(n) + L"개 편집하는 중", L"Editing " + Num(n) + L" files");
            } else {
                s.done = Tr(ko, one + L" 편집함", L"Edited " + one);
                s.running = Tr(ko, one + L" 편집하는 중", L"Editing " + one);
            }
            break;
        case Icon::Shell:
            if (r.calls > 1) {
                s.done = Tr(ko, L"명령 " + count + L"개 실행함", L"Ran " + count + L" commands");
                s.running = Tr(ko, L"명령 실행 중 · ", L"Running · ") + one;
            } else {
                s.done = one.empty() ? Tr(ko, L"명령 실행함", L"Ran a command") : one;
                s.running = Tr(ko, L"명령 실행 중", L"Running") + (one.empty() ? L"" : L" · " + one);
            }
            break;
        case Icon::Web:
            if (r.tool == "WebSearch") {
                s.done = r.calls > 1 ? Tr(ko, L"웹 검색 " + count + L"번", L"Searched the web " + count + L" times")
                                     : Tr(ko, L"웹 검색", L"Searched the web");
                s.running = Tr(ko, L"웹 검색하는 중", L"Searching the web");
            } else if (n > 1) {
                s.done = Tr(ko, L"웹 페이지 " + Num(n) + L"개 읽음", L"Read " + Num(n) + L" pages");
                s.running = Tr(ko, L"웹 페이지 읽는 중", L"Reading pages");
            } else {
                s.done = Tr(ko, one + L" 읽음", L"Read " + one);
                s.running = Tr(ko, one + L" 읽는 중", L"Reading " + one);
            }
            break;
        case Icon::Agent:
            if (r.calls > 1) {
                s.done = Tr(ko, L"에이전트 " + count + L"개", L"Ran " + count + L" agents");
                s.running = Tr(ko, L"에이전트 " + count + L"개 일하는 중", L"Running " + count + L" agents");
            } else {
                s.done = Tr(ko, L"에이전트 · ", L"Agent · ") + one;
                s.running = Tr(ko, L"에이전트 일하는 중 · ", L"Agent working · ") + one;
            }
            break;
        case Icon::Skill:
            s.done = Tr(ko, L"스킬 · ", L"Skill · ") + one;
            s.running = Tr(ko, L"스킬 읽는 중 · ", L"Loading skill · ") + one;
            break;
        case Icon::Question:
            s.done = Tr(ko, L"질문에 답함", L"Answered a question");
            s.running = Tr(ko, L"답을 기다리는 중", L"Waiting for your answer");
            break;
        case Icon::Plan:
            s.done = r.tool == "EnterPlanMode" ? Tr(ko, L"계획 세우기 시작", L"Started planning")
                                               : Tr(ko, L"계획 승인함", L"Plan approved");
            s.running = r.tool == "EnterPlanMode" ? Tr(ko, L"계획 세우는 중", L"Planning")
                                                  : Tr(ko, L"계획 승인을 기다리는 중", L"Waiting for plan approval");
            break;
        case Icon::Todo:
            s.done = Tr(ko, L"할 일 정리", L"Updated the to-do list");
            s.running = Tr(ko, L"할 일 정리하는 중", L"Updating the to-do list");
            break;
        case Icon::Mcp: {
            std::wstring name = McpName(r.tool);
            s.done = r.calls > 1 ? name + L" ×" + count : name;
            s.running = name + Tr(ko, L" 쓰는 중", L" running");
            break;
        }
        default: {
            std::wstring name = one.empty() ? std::wstring(r.tool.begin(), r.tool.end()) : one;
            s.done = r.calls > 1 ? name + L" ×" + count : name;
            s.running = name;
            break;
        }
    }
    return s;
}

}  // namespace detail

// Another company's model id as its name reads, like ModelName's: "GPT-6 Luna" from "gpt-6-luna", "GPT-5.4 Mini",
// "Gemini 4 Neon"; an id of another family, or not of the form family-version[-words], as it is.
inline std::wstring OtherModelName(const std::wstring& id) {
    std::vector<std::wstring> parts;
    for (size_t at = 0; at <= id.size();) {
        size_t dash = id.find(L'-', at);
        if (dash == std::wstring::npos) dash = id.size();
        parts.push_back(id.substr(at, dash - at));
        at = dash + 1;
    }
    auto version = [](const std::wstring& p) {
        return !p.empty() && p.front() != L'.' && p.back() != L'.' &&
               std::all_of(p.begin(), p.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'.'; });
    };
    auto word = [](const std::wstring& p) {
        return !p.empty() && std::all_of(p.begin(), p.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); });
    };
    if (parts.size() < 2 || !version(parts[1])) return id;
    std::wstring out = parts[0] == L"gpt" ? L"GPT-" + parts[1] : parts[0] == L"gemini" ? L"Gemini " + parts[1] : L"";
    if (out.empty()) return id;
    for (size_t i = 2; i < parts.size(); i++) {
        if (!word(parts[i])) return id;
        std::wstring w = parts[i];
        if (w[0] >= L'a' && w[0] <= L'z') w[0] = wchar_t(w[0] - L'a' + L'A');
        out += L" " + w;
    }
    return out;
}

// The turn's steps: the tool calls since the turn began, runs of one kind folded, in the order they began. The main
// loop's, and with `subagents` its subagents' too (Agent, workflow agents: each loop folded on its own, its steps
// marked as that subagent's).
inline std::vector<Step> StepsOf(const si::Board& b, bool ko, bool subagents = true) {
    std::vector<Step> out;
    if (!b.turnStartedAt) return out;
    std::vector<std::pair<size_t, Step>> made;
    std::map<std::string, detail::Run> open;  // each loop's run under way ('' the main loop's, else the agent's id)
    auto flush = [&](const std::string& loop) {
        auto it = open.find(loop);
        if (it == open.end()) return;
        Step s = detail::Words(it->second, ko);
        if (!loop.empty()) {
            s.sub = true;
            s.subOf = "agent:" + loop;
        }
        made.emplace_back(it->second.order, std::move(s));
        open.erase(it);
    };
    for (size_t i = 0; i < b.entries.size(); i++) {
        const si::Entry& e = b.entries[i];
        if (e.tool.empty() || (!subagents && !e.agentId.empty()) || e.startedAt < *b.turnStartedAt) continue;
        Icon icon = KindOf(e.tool);
        bool running = e.status == si::Status::Run && !e.endedAt;
        bool failed = e.status == si::Status::Err || e.status == si::Status::Block;
        auto it = open.find(e.agentId);
        detail::Run* run = it != open.end() ? &it->second : nullptr;
        // Agents and questions stand alone (each its own step); a failure ends a run so it shows.
        bool joins = run && run->icon == icon && icon != Icon::Agent && icon != Icon::Question && icon != Icon::Plan &&
                     !run->failed && (icon != Icon::Mcp && icon != Icon::Other ? true : run->tool == e.tool);
        if (!joins) {
            flush(e.agentId);
            run = &open[e.agentId];
            run->icon = icon;
            run->tool = e.tool;
            run->startedAt = e.startedAt;
            run->order = i;
        }
        run->labels.push_back(e.label);
        run->calls++;
        run->running = run->running || running;
        run->failed = run->failed || failed;
        if (running) {
            run->endedAt.reset();
        } else if (e.endedAt && !run->running) {
            run->endedAt = std::max(run->endedAt.value_or(0), *e.endedAt);
        }
    }
    while (!open.empty()) flush(open.begin()->first);
    std::stable_sort(made.begin(), made.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [order, s] : made) out.push_back(std::move(s));
    return out;
}

// The session's own subagents at work (Agent calls, workflow agents, background ones going on past the turn), from the
// board's record of every live one (Board::live: not the 80 kept entries, nor the 12 the map shows): one in a tool call
// that wrote within kAgentCallMs (a long build), else one that wrote in this turn within kAgentQuietMs (one killed
// leaves no end). `now` 0: no quiet counted.
constexpr Ms kAgentQuietMs = 10 * 60 * 1000, kAgentCallMs = 30 * 60 * 1000;
inline int AgentsAtWork(const si::Board& b, Ms now) {
    if (!b.turnStartedAt) return 0;
    int n = 0;
    for (const auto& [id, l] : b.live) {
        bool calling = !l.calls.empty();
        if (!calling && l.last < *b.turnStartedAt) continue;
        if (now && now - l.last >= (calling ? kAgentCallMs : kAgentQuietMs)) continue;
        n++;
    }
    return n;
}

inline Card CardOf(const si::SessionView& v, bool ko, Ms now = 0) {
    Card c;
    c.id = v.id;
    c.title = v.title.empty() ? v.project : v.title;
    // The main loop's model: a subagent's steps never change it (the board takes its model from the main loop only).
    c.model = v.map.model ? ModelName(*v.map.model) : std::wstring();
    c.turnStart = v.map.turnStartedAt.value_or(0);
    c.turnEnd = v.map.turnEndedAt;
    c.filesChanged = v.map.filesChanged;
    c.steps = StepsOf(v.map, ko);
    c.agentsRunning = AgentsAtWork(v.map, now);
    c.asking = v.status == si::SessionState::Ask;
    if (c.asking)
        for (auto it = c.steps.rbegin(); it != c.steps.rend(); ++it)
            if (!it->endedAt && !it->sub && it->icon == Icon::Plan) c.askingPlan = true;
    c.phase = c.turnEnd ? (v.map.turnAborted ? Phase::Stopped : Phase::Done) : c.asking ? Phase::Ask : Phase::Work;
    return c;
}

// The last step, while it runs.
inline const Step* Current(const Card& c) {
    return !c.steps.empty() && !c.steps.back().endedAt ? &c.steps.back() : nullptr;
}

// The step the headline names: the newest one still running (a sub's work runs beside the main loop's Agent step).
inline const Step* Under(const Card& c) {
    for (auto it = c.steps.rbegin(); it != c.steps.rend(); ++it)
        if (!it->endedAt) return &*it;
    return nullptr;
}

// ---- sub-sessions: Bloom's `kin` ----------------------------------------------------------------------------------
// A T3 session can start sub-sessions (T3 threads of their own: Claude, Codex, Gemini...). Bloom, which reads T3 Code's
// state, names a session's parent and its subs at work when asked; the card of the session that started them shows
// their work among its steps, and a Claude sub-session (whose transcript is read here like any) gets no card of its
// own. Its subs' subs (and theirs) are taken in by the same card, each with its own number.

enum class KidState : uint8_t { Run, Ask, Done, Failed, Stopped };

// One sub-session, as Bloom's `kin` names it.
struct Kid {
    std::string thread, session;  // its T3 thread, and its Claude Code session (empty when it has none: Codex...)
    std::wstring title;           // the thread's title
    std::string company = "other";  // claude, codex, gemini, other
    std::string model;            // the model id T3 runs it on
    KidState state = KidState::Run;
    Ms start = 0;
    std::optional<Ms> end;
    bool Working() const { return state == KidState::Run || state == KidState::Ask; }
};

struct Kin {
    std::string session;
    std::string parent;      // the session that started it, when Bloom knows one; empty: none
    std::vector<Kid> kids;   // its sub-sessions at work
};

constexpr size_t kMaxKids = 24;  // Bloom sends no more; past that the rest are left out

// Bloom's answer {"t":"kin","session","parent","kids":[...]}. Nothing when the line is not one, or names no session; a
// missing or malformed part reads as none (no parent, no kids), and a kid without a state Bloom gives is left out.
inline std::optional<Kin> ParseKin(std::string_view line) {
    si::json::Doc doc;
    if (!doc.Parse(line)) return std::nullopt;
    si::json::Val o = doc.Root();
    if (!o.IsObject() || o["t"].Str() != "kin" || !o["session"].IsString() || o["session"].Str().empty())
        return std::nullopt;
    auto str = [](si::json::Val v) { return v.IsString() ? std::string(v.Str()) : std::string(); };
    Kin k;
    k.session = str(o["session"]);
    k.parent = str(o["parent"]);
    if (k.parent == k.session) k.parent.clear();
    si::json::Val kids = o["kids"];
    if (!kids.IsArray()) return k;
    for (si::json::Val v = kids.First(); v.Exists() && k.kids.size() < kMaxKids; v = v.Next()) {
        if (!v.IsObject()) continue;
        std::string state = str(v["state"]);
        Kid kid;
        if (state == "run") kid.state = KidState::Run;
        else if (state == "ask") kid.state = KidState::Ask;
        else if (state == "done") kid.state = KidState::Done;
        else if (state == "failed") kid.state = KidState::Failed;
        else if (state == "stopped") kid.state = KidState::Stopped;
        else continue;
        kid.thread = str(v["thread"]);
        kid.session = str(v["session"]);
        if (kid.session == k.session) continue;
        kid.title = si::lotmap::Widen(str(v["title"]));
        std::string company = str(v["company"]);
        if (company == "claude" || company == "codex" || company == "gemini") kid.company = company;
        kid.model = str(v["model"]);
        if (v["start"].IsNumber()) kid.start = Ms(v["start"].Num());
        if (v["end"].IsNumber()) kid.end = Ms(v["end"].Num());
        k.kids.push_back(std::move(kid));
    }
    return k;
}

// The session whose card a session's work goes on, of those read here (`ids`): its parent, as Bloom named it (its own
// answer, else a parent that lists it among its kids); empty when it has none here.
inline std::string ParentOf(const std::string& id, const std::map<std::string, Kin>& kin, const std::set<std::string>& ids) {
    if (auto k = kin.find(id); k != kin.end() && !k->second.parent.empty())
        return ids.count(k->second.parent) && k->second.parent != id ? k->second.parent : std::string();
    for (auto& [parent, k] : kin) {
        if (parent == id || !ids.count(parent)) continue;
        for (const Kid& kid : k.kids)
            if (kid.session == id) return parent;
    }
    return {};
}

// The card a session's work, requests and questions go on: the one of the session its line of parents starts from
// (its parent's, or its parent's parent's...), else its own. A line that loops or runs past 8 parents starts nowhere:
// every session on it keeps its own card (none is left without one).
inline std::string CardFor(const std::string& id, const std::map<std::string, Kin>& kin, const std::set<std::string>& ids) {
    std::string at = id;
    for (int hop = 0; hop < 8; hop++) {
        std::string up = ParentOf(at, kin, ids);
        if (up.empty()) return at;
        if (up == id) break;
        at = up;
    }
    return ParentOf(at, kin, ids).empty() ? at : id;
}

// A sub-session a card takes in: what Bloom says of it, and its transcript when it is a Claude session read here.
struct Child {
    std::string thread, session;
    std::wstring title;
    std::string company = "claude", model;
    bool working = false, asking = false;
    Ms start = 0;
    const si::SessionView* view = nullptr;
};

// The sub-sessions of `main` (every session whose work goes on its card, CardFor: its subs, their subs...): the kids
// Bloom lists for it and for each of those subs, and the sessions read here whose work goes on its card though no list
// has them any more (they are done: their steps stay on its card for the turn). A sub is at work only while Bloom lists
// it as running or asking: a transcript left with its turn open (its Claude Code killed, or stopped from T3) does not
// keep the card up.
inline std::vector<Child> ChildrenOf(const std::string& main, const std::map<std::string, Kin>& kin,
                                     const std::map<std::string, const si::SessionView*>& views) {
    std::vector<Child> out;
    std::set<std::string> ids;
    for (auto& [id, v] : views) ids.insert(id);
    if (CardFor(main, kin, ids) != main) return out;  // its work, and its subs', is on another card
    auto ours = [&](const std::string& id) { return id != main && CardFor(id, kin, ids) == main; };
    auto had = [&](const std::string& session, const std::string& thread) {
        return std::any_of(out.begin(), out.end(), [&](const Child& c) {
            return (!session.empty() && c.session == session) || (!thread.empty() && c.thread == thread);
        });
    };
    auto listed = [&](const Kin& k) {
        for (const Kid& kid : k.kids) {
            auto v = kid.session.empty() ? views.end() : views.find(kid.session);
            // One read here whose work goes on another card is that card's.
            if (kid.session == main || (v != views.end() && !ours(kid.session)) || had(kid.session, kid.thread)) continue;
            Child ch;
            ch.thread = kid.thread;
            ch.session = kid.session;
            ch.title = kid.title;
            ch.company = kid.company;
            ch.model = kid.model;
            ch.working = kid.Working();
            ch.asking = kid.state == KidState::Ask;
            ch.start = kid.start;
            if (v != views.end()) ch.view = v->second;
            out.push_back(std::move(ch));
        }
    };
    // The card's own kids first, then its subs'.
    if (auto k = kin.find(main); k != kin.end()) listed(k->second);
    for (auto& [id, k] : kin)
        if (ours(id)) listed(k);
    for (auto& [id, v] : views) {
        if (!ours(id) || had(id, {})) continue;
        Child ch;
        ch.session = id;
        ch.title = v->title.empty() ? v->project : v->title;
        if (v->map.model) ch.model = *v->map.model;
        ch.asking = v->status == si::SessionState::Ask;
        ch.start = v->map.turnStartedAt.value_or(0);
        ch.view = v;
        out.push_back(std::move(ch));
    }
    return out;
}

// "Codex 하위 · 문서 정리": a sub-session named by its company and title; `waiting`: "Codex 하위가 답을 기다리는 중 ·
// 문서 정리" (the waiting before the title, which a long one would cut off).
inline std::wstring SubName(const Child& ch, bool ko, bool waiting = false) {
    std::wstring who = ch.company == "claude" ? L"Claude" : ch.company == "codex" ? L"Codex" : ch.company == "gemini" ? L"Gemini"
                                                                                  : Tr(ko, L"다른 AI", L"Other AI");
    std::wstring sub = waiting ? Tr(ko, L" 하위가 답을 기다리는 중", L" subtask waiting for an answer") : Tr(ko, L" 하위", L" subtask");
    return who + sub + (ch.title.empty() ? L"" : L" \x00B7 " + ch.title);
}

// The card's subs taken in: each Claude sub-session's steps since the card's turn began (from its own transcript),
// marked as that sub's; a sub-session at work with no transcript here (Codex, Gemini) or no steps yet one running step
// saying who it is. Every step in the order it began; the subs at work counted.
inline void TakeIn(Card& c, const std::vector<Child>& kids, bool ko) {
    std::vector<Step> subs;
    for (const Child& ch : kids) {
        bool working = ch.working;
        if (working) c.kidsRunning++;
        // Its name for its number: its session, else its T3 thread (Bloom may name the session only later; NumberSubs
        // keeps the number either name got).
        std::string of = !ch.session.empty() ? "session:" + ch.session
                         : !ch.thread.empty() ? "thread:" + ch.thread
                                              : "title:" + si::lotmap::Narrow(ch.title);
        std::string also = !ch.session.empty() && !ch.thread.empty() ? "thread:" + ch.thread : std::string();
        size_t had = subs.size();
        if (ch.view)
            for (Step& s : StepsOf(ch.view->map, ko, false)) {
                if (s.startedAt < c.turnStart) continue;
                s.sub = true;
                s.subOf = of;
                s.subAlso = also;
                subs.push_back(std::move(s));
            }
        if (subs.size() == had && working) {
            Step s;
            s.icon = Icon::Agent;
            s.sub = true;
            s.subOf = of;
            s.subAlso = also;
            s.asks = ch.asking;
            s.done = SubName(ch, ko);
            s.running = SubName(ch, ko, ch.asking);
            s.startedAt = std::max(ch.start, c.turnStart);
            subs.push_back(std::move(s));
        }
    }
    std::stable_sort(subs.begin(), subs.end(), [](const Step& a, const Step& b) { return a.startedAt < b.startedAt; });
    std::vector<Step> all;
    all.reserve(c.steps.size() + subs.size());
    std::merge(c.steps.begin(), c.steps.end(), subs.begin(), subs.end(), std::back_inserter(all),
               [](const Step& a, const Step& b) { return a.startedAt < b.startedAt; });
    c.steps = std::move(all);
}

// The numbers of a card's subs for one turn: each sub keeps the one it got while the turn lasts (the window keeps one
// of these per card; a new turn starts from 1 again).
struct SubNumbers {
    Ms turn = -1;                   // the turn they are for (the card's turnStart)
    std::map<std::string, int> of;  // by Step::subOf (and Step::subAlso, the same sub's other name)
    int last = 0;                   // the last number given
};

// Numbers the card's subs "[S1]", "[S2]"... in the order they first appear on it (their first step's start; at one
// start, in the steps' order), each sub keeping its number for the turn though others come and go, and though Bloom
// names its session only after its thread (the number either name got).
inline void NumberSubs(Card& c, SubNumbers& n) {
    if (n.turn != c.turnStart) n = {c.turnStart, {}, 0};
    auto known = [&](const Step& s) {
        for (const std::string* name : {&s.subOf, &s.subAlso})
            if (auto it = n.of.find(*name); !name->empty() && it != n.of.end()) return it->second;
        return 0;
    };
    struct Fresh {
        Ms at;
        std::string of, also;
    };
    std::vector<Fresh> fresh;
    for (const Step& s : c.steps) {
        if (!s.sub || known(s)) continue;
        auto it = std::find_if(fresh.begin(), fresh.end(), [&](const Fresh& f) { return f.of == s.subOf; });
        if (it == fresh.end()) fresh.push_back({s.startedAt, s.subOf, s.subAlso});
        else it->at = std::min(it->at, s.startedAt);
    }
    std::stable_sort(fresh.begin(), fresh.end(), [](const Fresh& a, const Fresh& b) { return a.at < b.at; });
    for (const Fresh& f : fresh) {
        n.of[f.of] = ++n.last;
        if (!f.also.empty()) n.of[f.also] = n.last;
    }
    for (Step& s : c.steps)
        if (s.sub) {
            s.subNo = known(s);
            n.of[s.subOf] = s.subNo;  // both names hold it from now on
            if (!s.subAlso.empty()) n.of[s.subAlso] = s.subNo;
        }
}

// The card with its main session's steps only (card.subtasks off): its subs' steps left out, all else kept (the subs
// at work counted, their questions and requests on the card, and the one step saying a sub with no transcript here,
// Codex or Gemini, waits on an answer: nothing else on the card says so).
inline void HideSubs(Card& c) {
    std::erase_if(c.steps, [](const Step& s) { return s.sub && !s.asks; });
}

// Where the words typed in a card's row go (SendSay), and whether they go at all.
struct SayTo {
    std::string session;  // the session they go to
    bool over = false;    // the question they were typed for is over: nothing is sent (as Bloom's "no-question")
};

// Who the row answers now: the session whose question it was opened for, else the card's own.
inline std::string RowFor(const std::string& card, bool askOpened, const std::string& askFor) {
    return askOpened && !askFor.empty() ? askFor : card;
}

// The words' target: the one the row answered when they were begun (`draftFor`, and `draftAsk`: it was open for a
// question then; empty: no words kept), else the one it answers now. Words begun for a question that is over, or
// that the row answers no more (another sub's question took it), are not sent to whoever asks now; words begun for
// the card's own session go there, a question opening meanwhile or not.
inline SayTo SayTarget(const std::string& card, const std::string& draftFor, bool draftAsk, bool blank, bool askOpened,
                       const std::string& askFor) {
    std::string now = RowFor(card, askOpened, askFor);
    if (blank || draftFor.empty()) return {now, false};
    return {draftFor, draftAsk && !(askOpened && now == draftFor)};
}

// Its subs at work: sub-sessions and its own subagents.
inline int SubsAtWork(const Card& c) {
    return c.kidsRunning + c.agentsRunning;
}

// A turn over while its subs still work is not done: the card works on, saying so, until the last of them ends (no
// 완료 while subs run). A stopped turn stays stopped. Also notes when its subs' shown steps ended
// (before HideSubs takes them off), for the time the card says it took. After TakeIn (the sub-sessions counted).
inline void WorkOn(Card& c) {
    for (const Step& s : c.steps)
        if (s.sub && s.endedAt) c.subsEnd = std::max(c.subsEnd, *s.endedAt);
    if (c.phase == Phase::Done && SubsAtWork(c) > 0) c.phase = Phase::Work;
}

// Whether a card is still shown at `now`: its turn runs, a sub still works, or it ended (the turn, its subs' last
// steps, and `lastKid`: when a sub was last seen at work) less than `doneMs` ago.
inline bool Lingers(const Card& c, Ms now, double doneMs, Ms lastKid = 0) {
    if (!c.turnEnd || SubsAtWork(c) > 0) return true;
    Ms end = std::max(*c.turnEnd, lastKid);
    for (const Step& s : c.steps)
        if (s.sub && s.endedAt) end = std::max(end, *s.endedAt);
    return double(now) < double(end) + doneMs;
}

// Whose a request Bloom passed on under the card's own session is: a subagent's when the tool asking runs only in
// subagents' loops. The line saying so ("하위 에이전트 · <its Agent call's words>" when one Agent call runs), empty
// for the main loop's; nothing while no call of that tool runs on the board yet (it is read a moment after).
inline std::optional<std::wstring> SubagentAsking(const si::Board& b, const std::wstring& tool, bool ko) {
    std::string t = si::lotmap::Narrow(tool);
    bool sub = false;
    const si::Entry* agent = nullptr;
    int agents = 0;
    for (const si::Entry& e : b.entries) {
        bool running = e.status == si::Status::Run && !e.endedAt;
        if (!running || e.tool.empty()) continue;
        if (e.tool == t) {
            if (e.agentId.empty()) return L"";
            sub = true;
        }
        if (e.agentId.empty() && KindOf(e.tool) == Icon::Agent) {
            agent = &e;
            agents++;
        }
    }
    if (!sub) return std::nullopt;
    std::wstring who = Tr(ko, L"하위 에이전트", L"Subagent");
    return agents == 1 && !agent->label.empty() ? who + L" \x00B7 " + agent->label : who;
}

}  // namespace rc
