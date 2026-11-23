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

#include "si/si_model.h"

#include <algorithm>
#include <cstdint>
#include <optional>
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
    std::wstring tool;  // "Bash", "Edit", "AskUserQuestion"
    std::wstring body;  // every field of the call as "name: value", one per line (empty for questions)
    std::vector<Question> questions;
    std::vector<std::vector<int>> picks;  // the choices picked so far, per question
    bool Answered() const {
        if (questions.empty()) return false;
        for (size_t i = 0; i < questions.size(); i++)
            if (i >= picks.size() || picks[i].empty()) return false;
        return true;
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

// The turn's steps: the main loop's tool calls since the turn began, runs of one kind folded.
inline std::vector<Step> StepsOf(const si::Board& b, bool ko) {
    std::vector<Step> out;
    if (!b.turnStartedAt) return out;
    std::optional<detail::Run> run;
    auto flush = [&] {
        if (run) out.push_back(detail::Words(*run, ko));
        run.reset();
    };
    for (const si::Entry& e : b.entries) {
        if (e.tool.empty() || !e.agentId.empty() || e.startedAt < *b.turnStartedAt) continue;
        Icon icon = KindOf(e.tool);
        bool running = e.status == si::Status::Run && !e.endedAt;
        bool failed = e.status == si::Status::Err || e.status == si::Status::Block;
        // Agents and questions stand alone (each its own step); a failure ends a run so it shows.
        bool joins = run && run->icon == icon && icon != Icon::Agent && icon != Icon::Question && icon != Icon::Plan &&
                     !run->failed && (icon != Icon::Mcp && icon != Icon::Other ? true : run->tool == e.tool);
        if (!joins) {
            flush();
            run = detail::Run{};
            run->icon = icon;
            run->tool = e.tool;
            run->startedAt = e.startedAt;
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
    flush();
    return out;
}

inline Card CardOf(const si::SessionView& v, bool ko) {
    Card c;
    c.id = v.id;
    c.title = v.title.empty() ? v.project : v.title;
    c.model = v.map.model ? ModelName(*v.map.model) : std::wstring();
    c.turnStart = v.map.turnStartedAt.value_or(0);
    c.turnEnd = v.map.turnEndedAt;
    c.filesChanged = v.map.filesChanged;
    c.steps = StepsOf(v.map, ko);
    c.asking = v.status == si::SessionState::Ask;
    if (c.asking)
        for (auto it = c.steps.rbegin(); it != c.steps.rend(); ++it)
            if (!it->endedAt && it->icon == Icon::Plan) c.askingPlan = true;
    c.phase = c.turnEnd ? (v.map.turnAborted ? Phase::Stopped : Phase::Done) : c.asking ? Phase::Ask : Phase::Work;
    return c;
}

// The step under way, if any.
inline const Step* Current(const Card& c) {
    return !c.steps.empty() && !c.steps.back().endedAt ? &c.steps.back() : nullptr;
}

}  // namespace rc
