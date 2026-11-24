// SPDX-License-Identifier: GPL-3.0-only
//
// Run card test program: draws cards outside explorer, into a PNG, from invented sessions (no transcript is read).
// It shows each state the card has, dark and light, at twice the size, so the picture can be checked against the
// design it follows; and it checks a few facts about the steps (exit code 1 when one fails).
//
//   card-host.exe <out.png> [--ko|--en] [--accent RRGGBB]

#include "../mod/rc_draw.h"
#include "../mod/rc_link.h"
#include "../mod/rc_place.h"
#include "../mod/si/si_board.h"

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using rc::Ms;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failed++;
}

constexpr Ms kT0 = 1795400000000;  // the invented turn's start

si::Entry E(const char* tool, const wchar_t* label, double start, double end, si::Status status = si::Status::Ok) {
    si::Entry e;
    e.tool = tool;
    e.label = label;
    e.station = si::Station::Files;
    e.startedAt = kT0 + Ms(start * 1000);
    if (end >= 0) e.endedAt = kT0 + Ms(end * 1000);
    e.status = end >= 0 ? status : si::Status::Run;
    return e;
}

si::SessionView Session(bool done) {
    si::SessionView v;
    v.id = "fixture";
    v.title = L"billing";
    v.status = done ? si::SessionState::Idle : si::SessionState::Work;
    v.map.model = "claude-sonnet-5-5";
    v.map.turnStartedAt = kT0;
    auto& es = v.map.entries;
    si::Entry you;  // the turn's own first entry: not a tool, not a step
    you.station = si::Station::You;
    you.startedAt = kT0;
    es.push_back(you);
    const wchar_t* reads[] = {L"billing/invoice.ts", L"billing/plan.ts", L"billing/tax.ts", L"lib/money.ts"};
    for (int i = 0; i < 4; i++) es.push_back(E("Read", reads[i], 0.1 + i * 0.1, 1.9));
    for (int i = 0; i < 4; i++) es.push_back(E("Grep", L"Grep", 2.0 + i * 0.2, 3.4));
    es.push_back(E("Edit", L"billing/invoice.ts", 3.5, 6.0));
    const wchar_t* tests[] = {L"tests/invoice.test.ts", L"tests/plan.test.ts", L"tests/tax.test.ts"};
    for (int i = 0; i < 3; i++) es.push_back(E("Edit", tests[i], 6.1 + i * 0.3, 7.9));
    if (done) {
        es.push_back(E("Bash", L"Run unit tests", 8.0, 11.3));
        es.push_back(E("Bash", L"Push fix/proration-rounding", 11.5, 12.9));
        v.map.turnEndedAt = kT0 + 13200;
        v.map.filesChanged = 4;
    } else {
        es.push_back(E("Bash", L"Run unit tests", 8.0, -1));
    }
    return v;
}

void Fixtures(bool ko) {
    rc::Card work = rc::CardOf(Session(false), ko);
    Check(work.steps.size() == 4, "work: runs of one kind folded into four steps");
    Check(work.phase == rc::Phase::Work, "work: phase");
    Check(rc::Current(work) && rc::Current(work)->icon == rc::Icon::Shell, "work: the running step is the shell");
    Check(work.model == L"Claude Sonnet 5.5", "model name");
    rc::Card done = rc::CardOf(Session(true), ko);
    Check(done.phase == rc::Phase::Done && !rc::Current(done), "done: phase, nothing running");
    Check(rc::ModelName("claude-haiku-4-5-20251001") == L"Claude Haiku 4.5", "model name: date left out");
    Check(rc::ModelName("claude-opus-5-5[1m]") == L"Claude Opus 5.5", "model name: [1m] left out");
    Check(rc::Clock(13200) == L"0:13" && rc::Clock(3723000) == L"1:02:03", "clock");
    Check(rc::Duration(false, 1900, false) == L"1.9s" && rc::Duration(false, 65000, false) == L"1m 5s", "durations");
}

// ---- sub-work: a main session (Opus) with a subagent (Sonnet), a T3 Claude sub-session (Sonnet) and a Codex one ----

si::Entry Sub(si::Entry e, const char* agent) {
    e.agentId = agent;
    e.from = si::From::Agents;
    return e;
}

// A subagent's model step, as the reader writes it ("sonnet 5.5 · step 1 · subagent").
si::Entry ModelStep(const char* agent, const wchar_t* label, double start) {
    si::Entry e;
    e.station = si::Station::Model;
    e.label = label;
    e.agentId = agent;
    e.from = si::From::Agents;
    e.startedAt = kT0 + Ms(start * 1000);
    e.endedAt = e.startedAt + 300;
    e.status = si::Status::Ok;
    return e;
}

si::SessionView MainSession(bool done) {
    si::SessionView v;
    v.id = "main";
    v.title = L"run-card";
    v.status = done ? si::SessionState::Idle : si::SessionState::Work;
    v.map.model = "claude-opus-5-5";
    v.map.turnStartedAt = kT0;
    auto& es = v.map.entries;
    es.push_back(E("Read", L"mod/rc_model.h", 0.1, 0.6));
    es.push_back(E("Read", L"mod/rc_draw.h", 0.2, 0.6));
    es.push_back(E("TodoWrite", L"", 0.8, 0.9));
    es.push_back(E("Agent", L"Review the diff", 1.0, done ? 2.9 : -1));
    es.push_back(ModelStep("a1", L"sonnet 5.5 \x00B7 step 1 \x00B7 subagent", 1.05));
    es.push_back(Sub(E("Read", L"mod/rc_link.h", 1.2, 1.8), "a1"));
    es.push_back(Sub(E("Read", L"tools/card-host.cpp", 1.3, 1.8), "a1"));
    es.push_back(ModelStep("a2", L"opus 5.5 \x00B7 step 1 \x00B7 subagent", 1.5));
    es.push_back(Sub(E("Grep", L"Grep", 1.6, 1.7), "a2"));
    es.push_back(Sub(E("Grep", L"Grep", 6.0, 6.4), "a1"));
    if (done) v.map.turnEndedAt = kT0 + 3000;
    return v;
}

si::SessionView ChildSession() {
    si::SessionView v;
    v.id = "child";
    v.title = L"문서 정리";
    v.status = si::SessionState::Work;
    v.map.model = "claude-sonnet-5-5";
    v.map.turnStartedAt = kT0 + 4500;
    v.map.entries.push_back(E("Bash", L"Build the add-on", 5.0, -1));
    return v;
}

constexpr const char* kKinLine =
    "{\"t\":\"kin\",\"session\":\"main\",\"parent\":null,\"kids\":["
    "{\"thread\":\"t1\",\"session\":\"child\",\"title\":\"\xEB\xAC\xB8\xEC\x84\x9C \xEC\xA0\x95\xEB\xA6\xAC\",\"company\":\"claude\","
    "\"model\":\"claude-sonnet-5-5\",\"state\":\"run\",\"start\":1795400004500,\"end\":null},"
    "{\"thread\":\"t2\",\"session\":\"\",\"title\":\"Lint cleanup\",\"company\":\"codex\",\"model\":\"gpt-6-luna\","
    "\"state\":\"run\",\"start\":1795400004000,\"end\":null}]}";

// The main card with its subs' steps taken in and numbered, as the window makes it (`subs` off: its own steps only),
// from the sessions read here: `main` and `more`.
rc::Card CardWith(const si::SessionView& main, const std::vector<const si::SessionView*>& more,
                  const std::map<std::string, rc::Kin>& kin, bool ko, bool subs = true, rc::SubNumbers* numbers = nullptr) {
    std::map<std::string, const si::SessionView*> views{{main.id, &main}};
    for (const si::SessionView* v : more) views[v->id] = v;
    rc::Card c = rc::CardOf(main, ko);
    rc::TakeIn(c, rc::ChildrenOf(main.id, kin, views), ko);
    rc::WorkOn(c);
    rc::SubNumbers fresh;
    rc::NumberSubs(c, numbers ? *numbers : fresh);
    if (!subs) rc::HideSubs(c);
    return c;
}

rc::Card MainCard(const si::SessionView& main, const si::SessionView& child, const std::map<std::string, rc::Kin>& kin, bool ko,
                  bool subs = true, rc::SubNumbers* numbers = nullptr) {
    return CardWith(main, {&child}, kin, ko, subs, numbers);
}

// A sub-session of the child (a grandchild of the main session): a Claude session read here.
si::SessionView GrandSession() {
    si::SessionView v;
    v.id = "grand";
    v.title = L"문서 정리 2";
    v.status = si::SessionState::Work;
    v.map.model = "claude-sonnet-5-5";
    v.map.turnStartedAt = kT0 + 5200;
    v.map.entries.push_back(E("Edit", L"mod/rc_model.h", 5.5, -1));
    return v;
}

const rc::Step* StepOf(const rc::Card& c, const std::string& sub) {
    for (auto& s : c.steps)
        if (s.sub && s.subOf == sub) return &s;
    return nullptr;
}

// A file of the add-on's folder (the program is build/test/card-host.exe in it), as bytes; empty when not there.
std::string AddonFile(const wchar_t* name) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    for (int up = 0; up < 3; up++) path.resize(path.find_last_of(L"\\/"));
    path += L"\\";
    path += name;
    std::string out;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, got);
        std::fclose(f);
    }
    return out;
}

// card.subtasks's default in the settings Bloom shows (settings.schema.json); nothing when it is not there.
std::optional<bool> SubtasksDefault() {
    std::string text = AddonFile(L"settings.schema.json");
    si::json::Doc doc;
    if (!doc.Parse(text)) return std::nullopt;
    for (si::json::Val g = doc.Root()["groups"].First(); g.Exists(); g = g.Next())
        for (si::json::Val it = g["items"].First(); it.Exists(); it = it.Next())
            if (it["key"].Str() == "card.subtasks" && it["type"].Str() == "bool" && it["default"].IsBool())
                return it["default"].IsTrue();
    return std::nullopt;
}

// The reader's record of every subagent at work (Board::live), as Begin, Finish, TurnComplete and Settle keep it: past
// the 80 kept entries and the 12 the map shows, an open call's end though its row has gone, forgotten when it ends or
// is silent past kLiveMs.
void BoardLiveChecks() {
    namespace sb = si::board;
    sb::Session s;
    auto call = [&](const char* agent, Ms at, bool open) {
        si::board::Start st = sb::MakeStart(si::Station::Files, L"Read", si::From::Agents, open ? std::nullopt : std::optional(si::Status::Ok));
        st.agentId = agent;
        st.tool = "Read";
        st.startedAt = at;
        if (!open) st.endedAt = at;
        return sb::Begin(s, st, at);
    };
    std::string slow = call("slow", kT0, true);
    for (int k = 0; k < 100; k++) call(("w" + std::to_string(k % 14)).c_str(), kT0 + 1000 + k, false);
    bool trimmed = std::none_of(s.board.entries.begin(), s.board.entries.end(), [&](const si::Entry& e) { return e.id == slow; });
    Check(trimmed && s.board.agents.size() == 12 && s.board.live.size() == 15 && s.board.live["slow"].calls.size() == 1,
          "board live: a slow agent's open call kept past the 80 entries, all 15 agents past the 12 shown");
    sb::Finish(s, slow, si::Status::Ok, kT0 + 5000);
    Check(s.board.live["slow"].calls.empty() && s.board.live["slow"].last == kT0 + 5000,
          "board live: its call's end counts though its row has gone");
    sb::TurnComplete(s, std::string("slow"), false, kT0 + 6000);
    sb::Settle(s, "w0", si::Status::Ok, kT0 + 6000);  // w0 is past the 12 the map shows
    Check(!s.board.live.count("slow") && !s.board.live.count("w0") && s.board.live.count("w1"),
          "board live: an agent that ends (its turn, or a background one's notice past the 12) is forgotten");
    call("late", kT0 + 6000 + sb::kLiveMs + 2000, false);
    Check(s.board.live.size() == 1 && s.board.live.count("late"), "board live: agents silent past kLiveMs are forgotten");

    // A question the session waits on stays among the kept entries however much work comes after it.
    sb::Session q;
    si::board::Start ask = sb::MakeStart(si::Station::You, L"Question", si::From::Core);
    ask.tool = "AskUserQuestion";
    ask.startedAt = kT0;
    std::string asked = sb::Begin(q, ask, kT0);
    for (int k = 0; k < 120; k++) {
        si::board::Start st = sb::MakeStart(si::Station::Files, L"Read", si::From::Agents, si::Status::Ok);
        st.agentId = "busy";
        st.tool = "Read";
        sb::Begin(q, st, kT0 + 10 + k);
    }
    bool kept = std::any_of(q.board.entries.begin(), q.board.entries.end(),
                            [&](const si::Entry& e) { return e.id == asked && e.status == si::Status::Run; });
    Check(kept && q.board.entries.size() == 81, "board: an open question is kept past the 80 entries (the card keeps it)");
}

void KinChecks(bool ko) {
    // Bloom's line read.
    auto good = rc::ParseKin(kKinLine);
    Check(good && good->session == "main" && good->parent.empty() && good->kids.size() == 2 &&
              good->kids[0].session == "child" && good->kids[0].title == L"문서 정리" && good->kids[0].company == "claude" &&
              good->kids[0].start == kT0 + 4500 && !good->kids[0].end && good->kids[1].session.empty() &&
              good->kids[1].company == "codex" && good->kids[1].Working(),
          "kin: a good line, its parent and two kids");
    auto bare = rc::ParseKin("{\"t\":\"kin\",\"session\":\"s1\"}");
    auto sparse = rc::ParseKin("{\"t\":\"kin\",\"session\":\"s1\",\"parent\":\"p1\",\"kids\":[{\"state\":\"ask\"},{\"title\":\"x\"},7]}");
    Check(bare && bare->parent.empty() && bare->kids.empty() && sparse && sparse->parent == "p1" && sparse->kids.size() == 1 &&
              sparse->kids[0].company == "other" && sparse->kids[0].state == rc::KidState::Ask && sparse->kids[0].title.empty(),
          "kin: missing fields read as none (a kid with no state left out)");
    auto wrong = rc::ParseKin("{\"t\":\"kin\",\"session\":\"s1\",\"parent\":5,\"kids\":\"no\"}");
    Check(!rc::ParseKin("{\"t\":\"kin\",") && !rc::ParseKin("{\"t\":\"kin\"}") && !rc::ParseKin("{\"t\":\"can\",\"session\":\"s1\"}") &&
              wrong && wrong->parent.empty() && wrong->kids.empty(),
          "kin: a malformed line is none; malformed parts are no parent, no kids");

    // Other companies' model names (for their cards).
    Check(rc::OtherModelName(L"gpt-6-luna") == L"GPT-6 Luna" && rc::OtherModelName(L"gpt-6.1-sol") == L"GPT-6.1 Sol" &&
              rc::OtherModelName(L"gpt-5.4-mini") == L"GPT-5.4 Mini" && rc::OtherModelName(L"gemini-4-neon") == L"Gemini 4 Neon" &&
              rc::OtherModelName(L"gpt-5") == L"GPT-5",
          "other model: GPT and Gemini ids read as names");
    Check(rc::OtherModelName(L"llama-3.3-70b") == L"llama-3.3-70b" && rc::OtherModelName(L"gpt-oss") == L"gpt-oss" &&
              rc::OtherModelName(L"gpt") == L"gpt" && rc::OtherModelName(L"gemini-4-Neon") == L"gemini-4-Neon" &&
              rc::OtherModelName(L"gpt-.5-x") == L"gpt-.5-x" && rc::OtherModelName(L"gpt-6--x") == L"gpt-6--x",
          "other model: unknown ids stay as given");

    // The steps merged in the order they began, the subs' marked.
    std::map<std::string, rc::Kin> kin{{"main", *good}};
    si::SessionView main = MainSession(false), child = ChildSession();
    rc::Card c = MainCard(main, child, kin, ko);
    bool ordered = true;
    for (size_t i = 1; i < c.steps.size(); i++) ordered = ordered && c.steps[i - 1].startedAt <= c.steps[i].startedAt;
    auto find = [&](Ms start) -> const rc::Step* {
        for (auto& s : c.steps)
            if (s.startedAt == kT0 + start) return &s;
        return nullptr;
    };
    const rc::Step *reads = find(100), *agent = find(1000), *a1 = find(1200), *a2 = find(1600), *codex = find(4000),
                   *kid = find(5000), *late = find(6000);
    Check(c.steps.size() == 8 && ordered && reads && !reads->sub && agent && !agent->sub && a1 && a1->sub &&
              a1->subOf == "agent:a1" && a2 && a2->sub && late && late->sub && late->subOf == "agent:a1" && kid && kid->sub &&
              kid->subOf == "session:child" && codex && codex->sub && codex->subOf == "thread:t2" && !codex->endedAt,
          "merge: main, subagent and sub-session steps in the order they began, the subs' marked");
    Check(codex && codex->running == rc::Tr(ko, L"Codex 하위 · Lint cleanup", L"Codex subtask · Lint cleanup"),
          "merge: a Codex sub-session is one running step naming it");
    Check(c.model == L"Claude Opus 5.5" && c.kidsRunning == 2, "merge: the model line stays the main model; two subs at work");
    Check(rc::Under(c) == kid, "merge: the headline names the newest step under way (the sub-session's)");

    // The subs numbered [S1], [S2]... in the order they first appear on the card, each keeping its number.
    {
        auto no = [](const rc::Card& card, Ms start) {
            for (auto& s : card.steps)
                if (s.startedAt == kT0 + start) return s.subNo;
            return -1;
        };
        // At first only the Codex sub-session is listed: the subagents and it.
        rc::Kin codexOnly = *good;
        codexOnly.kids.erase(codexOnly.kids.begin());
        std::map<std::string, rc::Kin> early{{"main", codexOnly}};
        rc::SubNumbers n;
        rc::Card c1 = MainCard(main, child, early, ko, true, &n);
        Check(no(c1, 100) == 0 && no(c1, 1000) == 0 && no(c1, 1200) == 1 && no(c1, 1600) == 2 && no(c1, 4000) == 3 &&
                  no(c1, 6000) == 1,
              "sub tags: S1, S2... in the order the subs first appear; a sub's later step keeps its number");
        // Then the Claude sub-session comes, started before the Codex one: it gets the next number, the others keep theirs.
        si::SessionView earlyKid = ChildSession();
        earlyKid.map.turnStartedAt = kT0 + 1000;
        earlyKid.map.entries[0].startedAt = kT0 + 1100;
        std::map<std::string, rc::Kin> full{{"main", *good}};
        rc::Card c2 = MainCard(main, earlyKid, full, ko, true, &n);
        Check(no(c2, 1100) == 4 && no(c2, 1200) == 1 && no(c2, 1600) == 2 && no(c2, 4000) == 3 && no(c2, 6000) == 1,
              "sub tags: a sub that comes later gets the next number though it started earlier; no number moves");
        // The Codex sub-session done (its line gone): the others keep their numbers.
        rc::Kin noCodex = *good;
        noCodex.kids.pop_back();
        std::map<std::string, rc::Kin> gone{{"main", noCodex}};
        rc::Card c3 = MainCard(main, earlyKid, gone, ko, true, &n);
        Check(no(c3, 1100) == 4 && no(c3, 1600) == 2 && no(c3, 4000) == -1, "sub tags: a sub gone leaves the others' numbers");
        // A fresh count: the full card in first-appearance order; a new turn starts from S1 again.
        rc::SubNumbers m;
        rc::Card c4 = MainCard(main, child, full, ko, true, &m);
        Check(no(c4, 1200) == 1 && no(c4, 1600) == 2 && no(c4, 4000) == 3 && no(c4, 5000) == 4,
              "sub tags: all at once, by their first step's start");
        si::SessionView next = MainSession(false);
        next.map.turnStartedAt = kT0 + 10;
        for (auto& e : next.map.entries) e.startedAt += 10;
        next.map.entries.erase(next.map.entries.begin() + 4, next.map.entries.begin() + 7);  // a1's first steps gone
        rc::Card c5 = MainCard(next, child, full, ko, true, &n);
        Check(no(c5, 1610) == 1 && no(c5, 4000) == 2 && no(c5, 5000) == 3 && no(c5, 6010) == 4,
              "sub tags: a new turn numbers from S1 again");
        bool named = false;
        for (const rc::Card* card : {&c1, &c2, &c4})
            for (auto& s : card->steps)
                for (const wchar_t* w : {L"Sonnet", L"Opus", L"GPT", L"Luna", L"sonnet", L"gpt"})
                    named = named || (s.sub && (s.done.find(w) != std::wstring::npos || s.running.find(w) != std::wstring::npos));
        Check(!named, "sub tags: no sub step carries a model name");
    }

    // card.subtasks off: the main session's steps only, the subs still counted.
    {
        rc::Card off = MainCard(main, child, kin, ko, false);
        bool none = !off.steps.empty() && std::none_of(off.steps.begin(), off.steps.end(), [](const rc::Step& s) { return s.sub; });
        const rc::Step* under = rc::Under(off);
        Check(none && off.steps.size() == 3 && off.kidsRunning == 2 && under && !under->sub && under->startedAt == kT0 + 1000,
              "subs off: no sub step (nor the Codex line), the subs at work counted, the headline names the main step");
        rc::Card offDone = MainCard(MainSession(true), child, kin, ko, false);
        Check(offDone.kidsRunning == 2 && rc::Lingers(offDone, kT0 + 3000 + 600000, 20000) &&
                  std::none_of(offDone.steps.begin(), offDone.steps.end(), [](const rc::Step& s) { return s.sub; }),
              "subs off: a done card still stays while its subs work");
    }
    Check(SubtasksDefault() == std::optional<bool>(false), "subs: card.subtasks is in the settings, off by default");
    {
        std::string wh = AddonFile(L"mod\\run-card.wh.cpp");
        Check(wh.find("  - subtasks: false\n") != std::string::npos && wh.find("bool subtasks = false;") != std::string::npos,
              "subs: the add-on's own default is off too");
    }

    // A sub-session gets no card while its parent is read here.
    std::set<std::string> both{"main", "child"}, alone{"child"};
    std::map<std::string, rc::Kin> own{{"child", rc::Kin{"child", "main", {}}}};
    Check(rc::ParentOf("child", kin, both) == "main" && rc::ParentOf("child", own, both) == "main" &&
              rc::ParentOf("main", kin, both).empty() && rc::ParentOf("child", kin, alone).empty() &&
              rc::CardFor("child", kin, both) == "main" && rc::CardFor("main", kin, both) == "main",
          "kin: a sub-session's work goes on its parent's card (its own when the parent is not read here)");
    // Its own answer names the parent though the parent's list has it no more (done): its steps stay on the card.
    std::map<std::string, rc::Kin> after{{"main", rc::Kin{"main", "", {}}}, {"child", rc::Kin{"child", "main", {}}}};
    rc::Card later = MainCard(main, child, after, ko);
    Check(later.steps.size() == 7 && later.kidsRunning == 0 && StepOf(later, "session:child"),
          "kin: a finished kid's own answer keeps its steps on the card (not at work: no list has it)");

    // The parent's card stays while a kid works, past its turn.
    si::SessionView over = MainSession(true);
    rc::Card ended = MainCard(over, child, kin, ko);
    Ms lateNow = kT0 + 3000 + 600000;
    Check(ended.phase == rc::Phase::Work && rc::Lingers(ended, lateNow, 20000),
          "linger: a turn over while its kids work is not done: the card works on");
    rc::Card quiet = rc::CardOf(over, ko);
    Check(!rc::Lingers(quiet, lateNow, 20000) && rc::Lingers(quiet, kT0 + 3000 + 5000, 20000) &&
              rc::Lingers(quiet, lateNow, 20000, lateNow - 1000),
          "linger: with no kid at work it goes doneSeconds after the turn (or after its kids were last seen)");

    // Its own subagents (a background agent, a workflow's) at work past the turn: the card works on, no Stop; one quiet
    // too long (killed: no end) does not keep it; once they end it says how long it all took. The board's record of
    // live agents (Board::live) is what counts: not the 80 kept entries nor the 12 the map shows.
    {
        si::SessionView v = MainSession(true);
        v.map.live["a1"] = {kT0 + 7000, {"e-open"}};  // in a tool call since 7 s
        Ms at = kT0 + 8000;
        rc::Card a = CardWith(v, {}, {}, ko);
        rc::Card w = rc::CardOf(v, ko, at);
        rc::WorkOn(w);
        w.stoppable = true;
        Check(a.agentsRunning == 1 && w.agentsRunning == 1 && w.phase == rc::Phase::Work && rc::Lingers(w, lateNow, 20000) &&
                  rc::Painter::ButtonsOf(w, nullptr) == 0,
              "subagents: at work past the turn, the card works on with no Stop (the turn is over)");
        rc::Card stale = rc::CardOf(v, ko, kT0 + 7000 + rc::kAgentCallMs);
        rc::WorkOn(stale);
        v.map.live["a1"] = {kT0 + 6400, {}};  // idle, its last line in this turn
        rc::Card idle = rc::CardOf(v, ko, kT0 + 6400 + rc::kAgentQuietMs);
        rc::Card fresh = rc::CardOf(v, ko, kT0 + 6400 + rc::kAgentQuietMs - 1000);
        Check(stale.agentsRunning == 0 && stale.phase == rc::Phase::Done && idle.agentsRunning == 0 && fresh.agentsRunning == 1,
              "subagents: one quiet too long is not at work (an open call 30 min, else 10)");
        // From the last turn: idle, it is not this turn's; in a long call that began before this turn, it is.
        v.map.live["a1"] = {kT0 - 60000, {}};
        rc::Card before = rc::CardOf(v, ko, at);
        v.map.live["a1"] = {kT0 - 60000, {"e-build"}};
        rc::Card building = rc::CardOf(v, ko, at);
        // Past the 12 the map shows, and with none of its rows kept: still counted.
        v.map.live.clear();
        for (int k = 0; k < 20; k++) v.map.live["w" + std::to_string(k)] = {kT0 + 7000, {}};
        rc::Card many = rc::CardOf(v, ko, at);
        Check(before.agentsRunning == 0 && building.agentsRunning == 1 && many.agentsRunning == 20 && v.map.agents.empty(),
              "subagents: a long call from the last turn counts, an idle one does not; twenty at once all count");
        rc::Card over2 = rc::CardOf(MainSession(true), ko);
        rc::WorkOn(over2);
        Check(over2.agentsRunning == 0 && over2.phase == rc::Phase::Done && over2.subsEnd == kT0 + 6400,
              "subagents: all ended, done; the time it took runs to its subs' last step");
    }
    // A subagent's request under the main session's id.
    si::SessionView asking = MainSession(false);
    asking.map.entries.push_back(Sub(E("Bash", L"Run the tests", 6.5, -1), "a1"));
    auto who = rc::SubagentAsking(asking.map, L"Bash", ko);
    asking.map.entries.push_back(E("Bash", L"git status", 6.6, -1));
    Check(who && *who == rc::Tr(ko, L"하위 에이전트 · Review the diff", L"Subagent · Review the diff") &&
              rc::SubagentAsking(asking.map, L"Bash", ko) == std::wstring() && !rc::SubagentAsking(asking.map, L"Edit", ko),
          "subagent ask: told from the board (a subagent's, the main loop's, not there yet)");

    // The line to Bloom: an answer to the sub-session's question goes to that session.
    std::vector<std::vector<int>> picks{{1}};
    Check(rc::SayLine("child", "ok", &picks) == "{\"t\":\"say\",\"session\":\"child\",\"text\":\"ok\",\"picks\":[[1]]}" &&
              rc::SessionLine("kin", "main") == "{\"t\":\"kin\",\"session\":\"main\"}",
          "lines: say with picks to the sub-session, kin");
}

// A sub at work only while Bloom says so, its subs' subs on the card, a sub's number kept across its two names, a
// waiting Codex sub shown with card.subtasks off, the words' target kept.
void LineageChecks(bool ko) {
    auto good = rc::ParseKin(kKinLine);
    si::SessionView main = MainSession(false), over = MainSession(true), child = ChildSession(), grand = GrandSession();
    Ms late = kT0 + 3000 + 600000;

    // A sub-session is at work only while Bloom lists it as running or asking: one killed (its transcript's turn left
    // open) or stopped does not keep the card up.
    {
        std::map<std::string, rc::Kin> gone{{"main", rc::Kin{"main", "", {}}}, {"child", rc::Kin{"child", "main", {}}}};
        rc::Card c = MainCard(over, child, gone, ko);
        const rc::Step* kid = StepOf(c, "session:child");
        Check(c.kidsRunning == 0 && kid && !kid->endedAt && !rc::Lingers(c, late, 20000),
              "kin: a sub no list has is not at work though its turn stays open (killed); the card goes after doneSeconds");
        rc::Kin ended = *good;
        ended.kids[0].state = rc::KidState::Stopped;
        ended.kids[1].state = rc::KidState::Done;
        rc::Card s = MainCard(over, child, {{"main", ended}}, ko);
        Check(s.kidsRunning == 0 && !StepOf(s, "thread:t2") && StepOf(s, "session:child") && !rc::Lingers(s, late, 20000),
              "kin: a sub Bloom says is stopped or done is not at work (a Codex one leaves no line)");
        // Claude Code's own subagents (the same transcript) are not subs counted at work: the turn keeps the card.
        rc::Card a = MainCard(main, child, {}, ko);
        Check(a.kidsRunning == 0 && StepOf(a, "agent:a1") && StepOf(a, "agent:a2") && !StepOf(a, "session:child") &&
                  rc::Lingers(a, late, 20000),
              "kin: subagents' steps as before, never counted as sub-sessions at work");
    }

    // A sub's sub (and its Codex or Gemini sub) goes on the main card with its own number, its question too.
    {
        rc::Kin childKin{"child", "main", {}};
        rc::Kid g;
        g.thread = "t3";
        g.session = "grand";
        g.title = L"문서 정리 2";
        g.company = "claude";
        g.start = kT0 + 5200;
        rc::Kid gem;
        gem.thread = "t4";
        gem.title = L"Gemini side";
        gem.company = "gemini";
        gem.state = rc::KidState::Ask;
        gem.start = kT0 + 5300;
        childKin.kids = {g, gem};
        std::map<std::string, rc::Kin> kin{{"main", *good}, {"child", childKin}};
        std::set<std::string> ids{"main", "child", "grand"};
        rc::Card c = CardWith(main, {&child, &grand}, kin, ko);
        const rc::Step *kid = StepOf(c, "session:child"), *gs = StepOf(c, "session:grand"), *codex = StepOf(c, "thread:t2"),
                       *gemini = StepOf(c, "thread:t4");
        std::set<int> numbers;
        for (auto& s : c.steps)
            if (s.sub) numbers.insert(s.subNo);
        Check(rc::CardFor("grand", kin, ids) == "main" && rc::CardFor("child", kin, ids) == "main" && kid && gs && codex &&
                  gemini && gemini->asks && c.kidsRunning == 4 && gs->subNo > 0 && gs->subNo != kid->subNo &&
                  numbers.size() == 6 && !numbers.count(0),
              "kin: a sub's subs join the main card, each with its own number, all counted at work");
        std::map<std::string, const si::SessionView*> views{{"main", &main}, {"child", &child}, {"grand", &grand}};
        auto kids = rc::ChildrenOf("main", kin, views);
        bool grandAt = std::any_of(kids.begin(), kids.end(), [](const rc::Child& k) { return k.session == "grand" && k.view && k.working; });
        Check(grandAt && rc::ChildrenOf("child", kin, views).empty(),
              "kin: the main card's subs include its subs' subs (their questions too); the child takes in none itself");
        // Its own answer alone naming its parent: still the main card's.
        std::map<std::string, rc::Kin> own{{"main", *good}, {"grand", rc::Kin{"grand", "child", {}}}};
        Check(rc::CardFor("grand", own, ids) == "main" && StepOf(CardWith(main, {&child, &grand}, own, ko), "session:grand"),
              "kin: a grandchild that names its parent itself is on the main card");
        // A line of parents that loops starts nowhere: each keeps its card.
        std::map<std::string, rc::Kin> loop{{"a", rc::Kin{"a", "b", {}}}, {"b", rc::Kin{"b", "a", {}}}};
        std::set<std::string> ab{"a", "b"};
        Check(rc::CardFor("a", loop, ab) == "a" && rc::CardFor("b", loop, ab) == "b", "kin: sessions naming each other keep their own cards");
    }

    // A sub keeps its number when Bloom names its session after its thread.
    {
        rc::Kin unnamed = *good;
        unnamed.kids[0].session.clear();
        rc::SubNumbers n;
        rc::Card c1 = MainCard(main, child, {{"main", unnamed}}, ko, true, &n);
        rc::Card c2 = MainCard(main, child, {{"main", *good}}, ko, true, &n);
        const rc::Step *before = StepOf(c1, "thread:t1"), *after = StepOf(c2, "session:child");
        Check(before && after && before->subNo > 0 && before->subNo == after->subNo && n.last == 4,
              "sub tags: a sub keeps its number when its session is named after its thread; no number is skipped");
    }

    // card.subtasks off: a Codex sub waiting on an answer keeps its one line (nothing else says so).
    {
        rc::Kin asking = *good;
        asking.kids[1].state = rc::KidState::Ask;
        rc::Card c = MainCard(main, child, {{"main", asking}}, ko, false);
        const rc::Step* wait = StepOf(c, "thread:t2");
        int subs = 0;
        for (auto& s : c.steps) subs += s.sub;
        std::wstring says = rc::Tr(ko, L"Codex 하위가 답을 기다리는 중 \x00B7 Lint cleanup", L"Codex subtask waiting for answer \x00B7 Lint cleanup");
        Check(wait && wait->asks && subs == 1 && wait->subNo == 3 && wait->running == says,
              "subs off: a Codex sub waiting on an answer keeps its line, numbered; the other sub steps stay hidden");
    }

    // The words typed go to the session the row answered when they were begun.
    {
        auto same = [](const rc::SayTo& t, const char* to, bool over) { return t.session == to && t.over == over; };
        Check(same(rc::SayTarget("main", "child", true, false, true, "child"), "child", false) &&
                  same(rc::SayTarget("main", "child", true, false, false, ""), "child", true) &&
                  same(rc::SayTarget("main", "child", true, false, true, "grand"), "child", true) &&
                  same(rc::SayTarget("main", "main", false, false, true, "child"), "main", false) &&
                  same(rc::SayTarget("main", "main", true, false, false, ""), "main", true),
              "say: words begun for a question go there or nowhere (over, or another's now); words for the card stay its");
        Check(same(rc::SayTarget("main", "", false, true, true, "child"), "child", false) &&
                  same(rc::SayTarget("main", "", false, true, false, ""), "main", false) &&
                  same(rc::SayTarget("main", "", false, false, true, "grand"), "grand", false) &&
                  rc::RowFor("main", true, "child") == "child" && rc::RowFor("main", false, "child") == "main",
              "say: no words begun (choices only), or no target kept: what the row answers now");
    }
}

struct Shot {
    rc::Card card;
    std::optional<rc::Ask> ask;
    rc::View view;
    Ms now;
    rc::Pointer ptr;
    std::optional<rc::Motion> motion;  // a moment of a change (the window's frames)
};

bool SavePng(IWICImagingFactory* wic, IWICBitmap* bitmap, const wchar_t* path) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bitmap, nullptr)) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    return true;
}

const rc::Motion* MotionOf(const Shot& s) { return s.motion ? &*s.motion : nullptr; }
const rc::Ask* AskOf(const Shot& s) { return s.ask ? &*s.ask : nullptr; }

// Rows of shots, each theme a band (dark over light), at twice the size, into a PNG.
bool Render(ID2D1Factory1* d2d, IWICImagingFactory* wic, rc::Painter& painter, const std::vector<std::vector<Shot>>& rows,
            bool ko, uint32_t accentDark, uint32_t accentLight, const wchar_t* path) {
    constexpr float kScale = 2, kGap = 24, kMargin = 28;
    // Each theme a band: measure the band's height from its cards.
    float bandW = 0, bandH = 0;
    std::vector<float> rowH;
    rc::Palette measure = rc::MakePalette(false, accentDark);
    for (auto& row : rows) {
        float w = kMargin, h = 0;
        for (auto& s : row) {
            rc::Laid L = painter.Run(nullptr, s.card, AskOf(s), s.view, measure, s.now, s.ptr, ko, false, MotionOf(s));
            w += L.w + kGap;
            h = std::max(h, L.h);
        }
        bandW = std::max(bandW, w - kGap + kMargin);
        rowH.push_back(h);
        bandH += h + kGap;
    }
    bandH += 2 * kMargin - kGap;
    UINT W = UINT(bandW * kScale), H = UINT(2 * bandH * kScale);

    ComPtr<IWICBitmap> bitmap;
    wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    ComPtr<ID2D1RenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    if (!bitmap || FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), rp, &rt))) return false;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt.As(&dc))) return false;
    dc->BeginDraw();
    for (int theme = 0; theme < 2; theme++) {
        bool light = theme == 1;
        rc::Palette p = rc::MakePalette(light, light ? accentLight : accentDark);
        float top = theme * bandH;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(kScale, kScale));
        ComPtr<ID2D1SolidColorBrush> ground;
        dc->CreateSolidColorBrush(si::draw_impl::Color(light ? 0xFFE6E6E9 : 0xFF141416), &ground);
        dc->FillRectangle({0, top, bandW, top + bandH}, ground.Get());
        float y = top + kMargin;
        for (size_t r = 0; r < rows.size(); r++) {
            float x = kMargin;
            for (auto& s : rows[r]) {
                dc->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * D2D1::Matrix3x2F::Scale(kScale, kScale));
                rc::Laid L = painter.Run(dc.Get(), s.card, AskOf(s), s.view, p, s.now, s.ptr, ko, true, MotionOf(s));
                x += L.w + kGap;
            }
            y += rowH[r] + kGap;
        }
    }
    HRESULT hr = dc->EndDraw();
    if (FAILED(hr)) {
        std::fprintf(stderr, "draw failed: 0x%08lx\n", hr);
        return false;
    }
    if (!SavePng(wic, bitmap.Get(), path)) return false;
    std::printf("wrote %ls (%ux%u)\n", path, W, H);
    return true;
}

// One card drawn alone at twice the size: its pixels, and its layout from the drawing pass.
struct Picture {
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    rc::Laid laid;
};

Picture Draw1(ID2D1Factory1* d2d, IWICImagingFactory* wic, rc::Painter& painter, const Shot& s, const rc::Palette& p, bool ko) {
    Picture out;
    rc::Laid L = painter.Run(nullptr, s.card, AskOf(s), s.view, p, s.now, s.ptr, ko, false, MotionOf(s));
    out.w = UINT(std::ceil(L.w * 2));
    out.h = UINT(std::ceil(L.h * 2));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1DeviceContext> dc;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    if (FAILED(wic->CreateBitmap(out.w, out.h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), rp, &rt)) || FAILED(rt.As(&dc)))
        return out;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(2, 2));
    out.laid = painter.Run(dc.Get(), s.card, AskOf(s), s.view, p, s.now, s.ptr, ko, false, MotionOf(s));
    if (FAILED(dc->EndDraw())) return out;
    out.px.resize(size_t(out.w) * out.h * 4);
    bitmap->CopyPixels(nullptr, out.w * 4, UINT(out.px.size()), out.px.data());
    return out;
}

// Whether two pictures are the same in the pixel rows from `top` to `bottom` (DIPs; all of them by default).
bool SamePixels(const Picture& a, const Picture& b, float top = 0, float bottom = 1e9f) {
    if (a.w != b.w || a.h != b.h || a.px.empty() || b.px.empty()) return false;
    size_t row = size_t(a.w) * 4;
    size_t from = size_t(std::clamp(top * 2, 0.0f, float(a.h))), to = size_t(std::clamp(bottom * 2, 0.0f, float(a.h)));
    for (size_t y = from; y < to; y++)
        if (!std::equal(a.px.begin() + y * row, a.px.begin() + (y + 1) * row, b.px.begin() + y * row)) return false;
    return true;
}

// Whether two pictures differ by no more than `by` in any channel of any pixel: the same picture, up to an edge's
// shift by a small fraction of a pixel (a fade, or a part drawn elsewhere, differs by far more).
bool NearPixels(const Picture& a, const Picture& b, int by) {
    if (a.w != b.w || a.h != b.h || a.px.empty() || b.px.empty()) return false;
    for (size_t i = 0; i < a.px.size(); i++)
        if (std::abs(int(a.px[i]) - int(b.px[i])) > by) return false;
    return true;
}

bool SameHits(const rc::Laid& a, const rc::Laid& b) {
    auto close = [](float x, float y) { return std::fabs(x - y) < 0.01f; };
    auto same = [&](const D2D1_RECT_F& x, const D2D1_RECT_F& y) {
        return close(x.left, y.left) && close(x.top, y.top) && close(x.right, y.right) && close(x.bottom, y.bottom);
    };
    if (a.hits.size() != b.hits.size() || !same(a.field, b.field)) return false;
    for (size_t i = 0; i < a.hits.size(); i++)
        if (a.hits[i].act != b.hits[i].act || !same(a.hits[i].r, b.hits[i].r)) return false;
    return true;
}

}  // namespace

// Where the cards go clear of what lies along the monitor's edges (rc::Clear): an island at the top edge over the
// top-right column moves the stack below it, and only a stack that would touch it.
void PlaceChecks() {
    const rc::Box mon{0, -40, 1000, 840}, area{0, 0, 1000, 800};  // the work area starts 40 below the monitor's top
    const std::vector<rc::Size> two{{300, 100}, {300, 60}};
    const float margin = 12, gap = 8;
    auto lay = [&](rc::Anchor a) { return [&, a](rc::Box in) { return rc::Stack(two, a, in, margin, gap); }; };
    const rc::Box island{600, -40, 380, 90};  // along the top edge, reaching 50 into the work area
    rc::Box room = rc::Clear(lay(rc::Anchor::TopRight), area, mon, margin, gap, {island});
    auto boxes = rc::Stack(two, rc::Anchor::TopRight, room, margin, gap);
    Check(boxes[0].y == island.Bottom() + gap, "place: the stack starts a gap below an island over its column");
    Check(boxes[1].y == boxes[0].Bottom() + gap, "place: the next card below it as before");
    room = rc::Clear(lay(rc::Anchor::TopLeft), area, mon, margin, gap, {island});
    Check(room.y == area.y && room.h == area.h, "place: a stack on the other side stays where it was");
    room = rc::Clear(lay(rc::Anchor::BottomRight), area, mon, margin, gap, {island});
    Check(room.y == area.y, "place: a stack from the bottom that does not reach it stays");
    const rc::Box floating{600, 50, 300, 60};  // over the stack but off every edge: not along one
    Check(rc::EdgeOf(floating, mon) == rc::Edge::None, "place: a shape off the edges is not along one");
    room = rc::Clear(lay(rc::Anchor::TopRight), area, mon, margin, gap, {floating});
    Check(room.y == area.y, "place: nothing moves for a shape off the edges");
    const rc::Box bar{0, 770, 1000, 70};  // a taskbar slid out over the bottom of the work area
    room = rc::Clear(lay(rc::Anchor::BottomRight), area, mon, margin, gap, {bar});
    boxes = rc::Stack(two, rc::Anchor::BottomRight, room, margin, gap);
    Check(boxes[0].Bottom() == bar.y - gap, "place: a stack from the bottom ends a gap above a taskbar slid out");
    const rc::Box side{960, 100, 40, 300};  // an island along the right edge
    room = rc::Clear(lay(rc::Anchor::TopRight), area, mon, margin, gap, {side});
    boxes = rc::Stack(two, rc::Anchor::TopRight, room, margin, gap);
    Check(boxes[0].Right() == side.x - gap, "place: a stack moves left of an island along the right edge");
}

// The cards in a row (card.arrange = row): from the place's corner inwards, tops or bottoms aligned, from the middle
// out at the centre places; an island over a top-centre row moves the row below it.
void RowChecks() {
    const rc::Box mon{0, -40, 1000, 840}, area{0, 0, 1000, 800};
    const std::vector<rc::Size> three{{300, 100}, {200, 60}, {250, 80}};
    const float margin = 12, gap = 8;
    auto b = rc::Row(three, rc::Anchor::TopRight, area, margin, gap);
    Check(b[0].Right() == area.Right() - margin && b[1].Right() == b[0].x - gap && b[2].Right() == b[1].x - gap,
          "row: at the top right the first card at the corner, the next ones to its left");
    Check(b[0].y == margin && b[1].y == margin && b[2].y == margin, "row: a top place's cards align their tops");
    b = rc::Row(three, rc::Anchor::BottomLeft, area, margin, gap);
    Check(b[0].x == margin && b[1].x == b[0].Right() + gap && b[2].x == b[1].Right() + gap,
          "row: at the bottom left the next ones to the first card's right");
    Check(b[0].Bottom() == area.Bottom() - margin && b[1].Bottom() == b[0].Bottom() && b[2].Bottom() == b[0].Bottom(),
          "row: a bottom place's cards align their bottoms");
    const std::vector<rc::Size> four{{300, 100}, {200, 60}, {250, 80}, {150, 40}};
    b = rc::Row(four, rc::Anchor::TopCenter, area, margin, gap);
    Check(b[2].Right() + gap == b[0].x && b[0].Right() + gap == b[1].x && b[1].Right() + gap == b[3].x,
          "row: at the centre the first in the middle, then right, left, right");
    Check(b[2].x - area.x == area.Right() - b[3].Right(), "row: the centre's row as a whole centred");
    auto stacked = rc::Stack(three, rc::Anchor::MiddleLeft, area, margin, gap);
    b = rc::Row(three, rc::Anchor::MiddleLeft, area, margin, gap);
    Check(b[1].x == stacked[1].x && b[1].y == stacked[1].y && !rc::Across(rc::Anchor::MiddleRight, true) &&
              rc::Across(rc::Anchor::BottomCenter, true) && !rc::Across(rc::Anchor::TopLeft, false),
          "row: the middle places stack");
    const rc::Box island{350, -40, 300, 90};  // along the top edge in the middle, reaching 50 into the work area
    auto lay = [&](rc::Box in) { return rc::Row(three, rc::Anchor::TopCenter, in, margin, gap); };
    rc::Box room = rc::Clear(lay, area, mon, margin, gap, {island});
    b = rc::Row(three, rc::Anchor::TopCenter, room, margin, gap);
    Check(b[0].y == island.Bottom() + gap && b[1].y == b[0].y && b[2].y == b[0].y,
          "row: a top-centre row reaching an island goes a gap below it");
    Check(rc::RoomAcross(area, margin) == area.w - 2 * margin, "row: the room across is the width less the margins");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "card-host.exe <out.png> [--ko|--en] [--accent RRGGBB]\n");
        return 2;
    }
    bool ko = true;
    uint32_t accentDark = 0xFFE4E4E4, accentLight = 0xFF2E2E2E;  // Bloom's gray, the accent this PC uses
    for (int i = 2; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--en") ko = false;
        if (a == L"--ko") ko = true;
        if (a == L"--accent" && i + 1 < argc) accentDark = accentLight = 0xFF000000u | wcstoul(argv[++i], nullptr, 16);
    }
    PlaceChecks();
    RowChecks();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Fixtures(ko);
    KinChecks(ko);
    BoardLiveChecks();
    LineageChecks(ko);

    ComPtr<ID2D1Factory1> d2d;
    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                      reinterpret_cast<void**>(d2d.GetAddressOf()));
    ComPtr<IDWriteFactory> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!d2d || !dwrite || !wic) return 3;

    rc::Painter painter(d2d.Get(), dwrite.Get());
    rc::Card work = rc::CardOf(Session(false), ko);
    rc::Card done = rc::CardOf(Session(true), ko);
    rc::Card stopped = done;
    stopped.phase = rc::Phase::Stopped;
    rc::Ask approve;
    approve.id = "a1";
    approve.tool = L"Bash";
    // One long line, as a chained command comes: shown wrapped, whole.
    approve.body = L"command: git fetch origin && git rebase origin/main && npm test -- --runInBand --coverage=false && "
                   L"git push --force-with-lease origin fix/proration-rounding\ndescription: Rebase on main, test, push the fix";
    rc::Ask question;
    question.id = "q1";
    question.tool = L"AskUserQuestion";
    rc::Question q;
    q.header = ko ? L"반올림" : L"Rounding";
    q.text = ko ? L"일할 계산 금액은 어떻게 반올림할까요?" : L"How should prorated amounts round?";
    q.choices = {{ko ? L"센트 단위 반올림 (추천)" : L"Round to the cent (Recommended)",
                  ko ? L"지금 청구서와 같습니다" : L"Matches today's invoices"},
                 {ko ? L"내림" : L"Round down",
                  ko ? L"고객에게 조금 덜 청구합니다. 회계 쪽에서 월말에 차액을 한 번에 맞춰야 합니다"
                     : L"Bills customers slightly less; accounting squares the difference at the end of the month"},
                 {ko ? L"은행가 반올림" : L"Banker's rounding", L""}};
    question.questions = {q};
    question.picks = {{0}};

    // What the card shows whole, and what goes back to Bloom's card.
    Check(painter.Fits(approve, ko), "fits: a long one-line command, wrapped");
    rc::Ask huge = approve;
    huge.body.clear();
    for (int i = 0; i < 40; i++) huge.body += L"echo line " + std::to_wstring(i) + L" && ";
    Check(!painter.Fits(huge, ko), "fits: a command past the card's lines goes to Bloom");
    Check(painter.Fits(question, ko), "fits: the question");
    rc::Ask wordy = question;
    for (int i = 0; i < 6; i++) wordy.questions[0].text += L" " + wordy.questions[0].text;
    Check(!painter.Fits(wordy, ko), "fits: a question past three lines goes to Bloom");

    // Pills of short and long words: as wide as a card (pill.width = same), else each as wide as its words.
    {
        auto pillW = [&](const rc::Card& c) {
            return painter.Run(nullptr, c, nullptr, rc::View::Pill, rc::MakePalette(false, accentDark), kT0 + 10500, {}, ko).w;
        };
        Check(pillW(stopped) == rc::kCardW && pillW(work) == rc::kCardW, "pill: the same length, a card's width");
        painter.SamePill(rc::kRowPillW);
        Check(pillW(stopped) == rc::kRowPillW && pillW(work) == rc::kRowPillW, "pill: in a row the same, shorter length");
        painter.SamePill(0);
        Check(pillW(stopped) != pillW(work), "pill: fit to the words, short and long differ");
        painter.SamePill(rc::kCardW);
    }

    Ms now = kT0 + 10500;
    // Moments of the changes: the step before the shell, still running, is where a new step, the
    // request and the end begin from.
    rc::Card before = work;
    before.steps.pop_back();
    before.steps.back().endedAt.reset();
    auto moment = [&](auto set) {
        rc::Motion m;
        set(m);
        return std::optional<rc::Motion>(m);
    };
    rc::Card t3 = work;
    t3.canSay = true;
    rc::Card both = t3;
    both.stoppable = true;
    rc::Card typing = t3;
    typing.composing = typing.typed = true;
    typing.draft = ko ? L"테스트는 건너뛰고 바로 푸시해 줘" : L"Skip the tests and push it";
    rc::Card asked = t3;
    asked.phase = rc::Phase::Ask;
    asked.asking = true;
    asked.composing = true;
    rc::Ask t3q = question;  // a T3 question: its choices, answered in the row
    t3q.inRow = true;
    t3q.picks = {{}};
    rc::Ask t3picked = t3q;
    t3picked.picks = {{2}};
    {
        rc::Laid L = painter.Run(nullptr, asked, &t3q, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        bool pick = false, field = false, say = false, send = false;
        for (auto& h : L.hits) pick |= h.act == rc::Act::Pick, field |= h.act == rc::Act::Field, say |= h.act == rc::Act::Say, send |= h.act == rc::Act::Send;
        Check(pick && field && !say && !send, "say: a T3 question shows its choices over the row, nothing to send yet");
        rc::Laid P = painter.Run(nullptr, asked, &t3picked, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        say = false;
        for (auto& h : P.hits) say |= h.act == rc::Act::Say;
        Check(say, "say: a choice picked lights the send button");
    }
    // Several questions, one at a time: the second of four shown, the first answered.
    rc::Ask pages = t3q;
    {
        rc::Question a = q, b = q, c = q, d = q;
        a.header = ko ? L"통화" : L"Currency";
        a.text = ko ? L"어느 통화부터 적용할까요?" : L"Which currency goes first?";
        a.choices = {{ko ? L"원화" : L"KRW", L""}, {ko ? L"달러" : L"USD", L""}};
        b.header = ko ? L"적용 시점" : L"When";
        b.text = ko ? L"새 계산 방식은 언제부터 쓸까요?" : L"When should the new way of rounding start?";
        b.choices = {{ko ? L"다음 달부터 (추천)" : L"From next month (Recommended)",
                      ko ? L"이번 달 청구서는 그대로 두고 다음 달 청구서부터 새 방식으로 계산합니다."
                         : L"This month's invoices stay as they are; next month's use the new way."},
                     {ko ? L"바로" : L"Right away",
                      ko ? L"지금 열려 있는 청구서도 다시 계산합니다." : L"Open invoices are worked out again too."},
                     {ko ? L"아직 미정" : L"Not decided yet", L""}};
        c.header = ko ? L"알림" : L"Notice";
        c.multi = true;
        d.header = ko ? L"검토" : L"Review";
        pages.questions = {a, b, c, d};
        pages.picks = {{0}, {}, {}, {}};
        pages.page = 1;
    }
    rc::Ask pagesFirst = pages, pagesLast = pages, relayed = pages;
    relayed.inRow = false;  // Bloom's request: its 보내기 under the question
    relayed.id = "q4";
    pagesFirst.page = 0;
    pagesLast.page = 3;
    pagesLast.picks = {{0}, {0}, {1}, {2}};
    {
        rc::Palette p = rc::MakePalette(false, accentDark);
        auto laid = [&](const rc::Ask& a) { return painter.Run(nullptr, asked, &a, rc::View::Card, p, now, {}, ko); };
        auto has = [](const rc::Laid& L, rc::Act act, int q) {
            for (auto& h : L.hits)
                if (h.act == act && h.q == q) return true;
            return false;
        };
        rc::Laid L = laid(pages), F = laid(pagesFirst), E = laid(pagesLast);
        int picks = 0, other = 0;
        for (auto& h : L.hits)
            if (h.act == rc::Act::Pick) (h.q == 1 ? picks : other)++;
        Check(picks == 3 && other == 0, "pages: only the question shown has choices to pick");
        Check(has(L, rc::Act::Page, 0) && has(L, rc::Act::Page, 2), "pages: ‹ and › turn to the question before and after");
        Check(!has(F, rc::Act::Page, -1) && has(F, rc::Act::Page, 1) && has(E, rc::Act::Page, 2) && !has(E, rc::Act::Page, 4),
              "pages: no ‹ on the first, no › on the last");
        bool say = false;
        for (auto& h : E.hits) say |= h.act == rc::Act::Say;
        Check(say && !has(L, rc::Act::Say, -1), "pages: send lights once every question has a pick, from any page");
        rc::Ask gaps = pages;
        gaps.picks = {{0}, {}, {0}, {}};
        Check(pages.NextOpen(0) == 1 && gaps.NextOpen(1) == 3 && gaps.NextOpen(3) == 1 && pagesLast.NextOpen(3) == -1,
              "pages: a pick turns to the next question not answered, after it, else before it");
        float tallest = 0;
        for (int i = 0; i < 4; i++) {
            rc::Ask at = pages;
            at.page = i;
            tallest = std::max(tallest, laid(at).h);
        }
        Check(painter.AskH(asked, pages, p, now, ko) == tallest && tallest > F.h,
              "pages: the room a request needs is its tallest page");
        rc::Ask single = t3q;
        rc::Laid S = laid(single);
        Check(!has(S, rc::Act::Page, 1) && !has(S, rc::Act::Page, -1), "pages: one question has no pager");
    }
    // Contrast: every word of the card at 4.5:1 or more on what it is drawn on (the body over a black or a white desk,
    // a hovered or picked choice, the row to type in, an accent button), dark and light, Bloom's rose and gray.
    {
        auto over = [](uint32_t top, uint32_t under) {
            double a = (top >> 24) / 255.0;
            uint32_t out = 0xFF000000;
            for (int sh = 0; sh <= 16; sh += 8) {
                double v = ((top >> sh) & 255) * a + ((under >> sh) & 255) * (1 - a);
                out |= uint32_t(std::lround(v)) << sh;
            }
            return out;
        };
        double worst = 99;
        std::string where;
        for (bool light : {false, true})
            for (uint32_t accent : light ? std::vector<uint32_t>{0xFFE04E78, 0xFF2E2E2E} : std::vector<uint32_t>{0xFFFF7A9E, 0xFFE4E4E4}) {
                rc::Palette p = rc::MakePalette(light, accent);
                for (uint32_t desk : {0xFF000000u, 0xFFFFFFFFu}) {
                    uint32_t body = over(p.surface, desk);
                    struct Pair {
                        const char* what;
                        uint32_t ink, ground;
                    } pairs[] = {{"faint on the body", p.faint, body},
                                 {"dim on the body", p.dim, body},
                                 {"faint on a hovered choice", p.faint, over(p.hover, body)},
                                 {"dim on a picked choice", p.dim, over(p.accentSoft, body)},
                                 {"faint in the row to type in", p.faint, over(p.btn, body)},
                                 {"accent ink on the accent", p.accentInk, p.accent}};
                    for (auto& pr : pairs) {
                        double r = rc::Contrast(pr.ink, pr.ground);
                        if (r < worst) worst = r, where = std::string(light ? "light " : "dark ") + pr.what;
                    }
                }
            }
        Check(worst >= 4.5, ("contrast: every word at 4.5:1 or more (lowest " + std::to_string(worst) + ", " + where + ")").c_str());
    }
    rc::Card sent = t3;
    sent.note = ko ? L"현재 턴에 전달했습니다" : L"Sent to the current turn";
    rc::Card failed = done;
    failed.canSay = true;
    failed.note = ko ? L"T3가 꺼져 있어 보내지 못했습니다" : L"Could not send because T3 is not running";
    failed.noteBad = true;
    {
        rc::Laid L = painter.Run(nullptr, typing, nullptr, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        bool field = false, say = false, stop = false, open = false;
        for (auto& h : L.hits) {
            field |= h.act == rc::Act::Field;
            say |= h.act == rc::Act::Say;
            stop |= h.act == rc::Act::Stop;
            open |= h.act == rc::Act::Open;
        }
        Check(field && say && !open && L.field.right - L.field.left > 200, "say: the row typed in is one box with its send button");
        rc::Laid B = painter.Run(nullptr, both, nullptr, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        bool reply = false;
        for (auto& h : B.hits) reply |= h.act == rc::Act::Reply, stop |= h.act == rc::Act::Stop, open |= h.act == rc::Act::Open;
        Check(reply && stop && open, "say: Stop, 보내기 and Open side by side");
        Check(B.h == L.h, "say: typing keeps the card's height");
    }
    // The light over a working headline: on a pill folded back from a card, waiting while a card opens.
    {
        rc::Motion folded;
        folded.shine = true;
        folded.open = 0;
        rc::Motion opening = folded;
        opening.open = 0.5f;
        rc::Palette p = rc::MakePalette(false, accentDark);
        Picture pill = Draw1(d2d.Get(), wic.Get(), painter, {work, std::nullopt, rc::View::Pill, now, {}, folded}, p, ko);
        Picture card = Draw1(d2d.Get(), wic.Get(), painter, {work, std::nullopt, rc::View::Card, now, {}, opening}, p, ko);
        Check(pill.laid.shine && !card.laid.shine, "shine: a pill folded back from a card has the light, a card opening waits");
    }
    // Sub-work on the main card: its steps taken in; a sub-session's question with the line saying whose; the turn over
    // while its subs work; a subagent's request.
    auto good = rc::ParseKin(kKinLine);
    std::map<std::string, rc::Kin> kin{{"main", *good}};
    si::SessionView mainView = MainSession(false), childView = ChildSession(), overView = MainSession(true);
    rc::Card family = MainCard(mainView, childView, kin, ko);
    family.canSay = true;
    rc::Card familyDone = MainCard(overView, childView, kin, ko);
    rc::Card familyOff = MainCard(mainView, childView, kin, ko, false), familyDoneOff = MainCard(overView, childView, kin, ko, false);
    familyOff.canSay = true;
    rc::Kin codexAsks = *good;  // the Codex sub waits on an answer
    codexAsks.kids[1].state = rc::KidState::Ask;
    rc::Card familyAskOff = MainCard(mainView, childView, {{"main", codexAsks}}, ko, false);
    familyAskOff.canSay = true;
    rc::Card subAsked = family;
    subAsked.composing = true;
    subAsked.subAsk = rc::Tr(ko, L"하위 · 문서 정리", L"subtask · 문서 정리");
    rc::Ask subQ = t3q;
    subQ.from = "child";
    subQ.sub = subAsked.subAsk;
    subQ.sourced = true;
    rc::Ask subApprove = approve;
    subApprove.sub = rc::Tr(ko, L"하위 에이전트 · Review the diff", L"Subagent · Review the diff");
    subApprove.body = L"command: npm test -- --runInBand\ndescription: Run the tests";
    {
        Check(familyDone.phase == rc::Phase::Work &&
                  painter.TitleOf(familyDone, nullptr, ko) == rc::Tr(ko, L"하위 2개 진행 중", L"2 subtasks running"),
              "linger: a turn over with kids at work says they work (no 완료 while they run)");
        rc::Laid L = painter.Run(nullptr, subAsked, &subQ, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        bool pick = false, field = false;
        for (auto& h : L.hits) pick |= h.act == rc::Act::Pick, field |= h.act == rc::Act::Field;
        rc::Laid plain = painter.Run(nullptr, asked, &t3q, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        Check(pick && field && painter.TitleOf(subAsked, &subQ, ko) == rc::Tr(ko, L"하위의 질문", L"Question from subtask") &&
                  L.h == plain.h + 22 && rc::Painter::Dotted(subAsked, nullptr),
              "sub question: on the main card with its choices, the line saying whose, the row to answer it");
        // card.subtasks off: the same headline while subs work past the turn, a sub's question on the card as on.
        rc::Card subAskedOff = familyOff;
        subAskedOff.composing = true;
        subAskedOff.subAsk = subAsked.subAsk;
        rc::Laid off = painter.Run(nullptr, subAskedOff, &subQ, rc::View::Card, rc::MakePalette(false, accentDark), now, {}, ko);
        bool offPick = false, offField = false;
        for (auto& h : off.hits) offPick |= h.act == rc::Act::Pick, offField |= h.act == rc::Act::Field;
        Check(painter.TitleOf(familyDoneOff, nullptr, ko) == painter.TitleOf(familyDone, nullptr, ko) &&
                  painter.TitleOf(subAskedOff, &subQ, ko) == rc::Tr(ko, L"하위의 질문", L"Question from subtask") && offPick &&
                  offField && off.h == L.h,
              "subs off: the turn over says its subs still work; a sub's question on the main card as with them on");
        std::wstring working = painter.TitleOf(familyOff, nullptr, ko);
        Check(working.find(L"Codex") == std::wstring::npos && working.find(L"Build") == std::wstring::npos &&
                  working.find(L"Grep") == std::wstring::npos && working != painter.TitleOf(family, nullptr, ko),
              "subs off: the headline names no hidden sub step");
    }
    std::vector<std::vector<Shot>> rows = {
        {{work, std::nullopt, rc::View::Pill, now, {}}, {done, std::nullopt, rc::View::Pill, kT0 + 20000, {}}},
        {{work, std::nullopt, rc::View::Card, now, {}},
         {work, approve, rc::View::Card, now, {rc::Act::Allow}},
         {work, question, rc::View::Card, now, {}},
         {done, std::nullopt, rc::View::Card, kT0 + 20000, {rc::Act::Open}},
         {stopped, std::nullopt, rc::View::Card, kT0 + 20000, {}}},
        {{work, std::nullopt, rc::View::Card, now, {}, moment([&](rc::Motion& m) { m.open = rc::Bezier(rc::kEase, 0.12f); })},
         {work, std::nullopt, rc::View::Card, now, {}, moment([&](rc::Motion& m) {
              m.title = m.rows = 0.4f;
              m.titleWas = m.rowsWas = &before;
          })},
         {work, approve, rc::View::Card, now, {}, moment([&](rc::Motion& m) {
              m.title = m.buttons = 0.45f;
              m.dot = 0.3f;
              m.titleWas = m.buttonsWas = &work;
          })},
         {done, std::nullopt, rc::View::Card, kT0 + 20000, {}, moment([&](rc::Motion& m) {
              m.check = 0.35f;
              m.title = m.rows = m.buttons = 0.5f;
              m.titleWas = m.rowsWas = m.buttonsWas = &work;
          })},
         {work, std::nullopt, rc::View::Pill, now, {}, moment([&](rc::Motion& m) { m.shine = true; })}},
        // 보내기 (a T3 session): the button, the row typed in (words kept, a question's empty box), what came of it.
        {{t3, std::nullopt, rc::View::Card, now, {rc::Act::Reply}},
         {both, std::nullopt, rc::View::Card, now, {}},
         {typing, std::nullopt, rc::View::Card, now, {rc::Act::Say}},
         {asked, std::nullopt, rc::View::Card, now, {}},
         {asked, t3q, rc::View::Card, now, {}},
         {asked, t3picked, rc::View::Card, now, {rc::Act::Say}},
         {sent, std::nullopt, rc::View::Card, now, {}},
         {failed, std::nullopt, rc::View::Card, kT0 + 20000, {}}},
        // Several questions one at a time: the second of four (› pointed at), the first, the last with all picked
        // (send lit), and Bloom's own (보내기 under it).
        {{asked, pages, rc::View::Card, now, {rc::Act::Page, 2}},
         {asked, pagesFirst, rc::View::Card, now, {}},
         {asked, pagesLast, rc::View::Card, now, {}},
         {work, relayed, rc::View::Card, now, {rc::Act::Pick, 1, 0}}},
        // Sub-work: the merged steps, a sub-session's question, the turn over while subs work, a subagent's request.
        {{family, std::nullopt, rc::View::Card, now, {}},
         {subAsked, subQ, rc::View::Card, now, {}},
         {familyDone, std::nullopt, rc::View::Card, now, {}},
         {family, subApprove, rc::View::Card, now, {}},
         {familyDone, std::nullopt, rc::View::Pill, now, {}}},
        // ... with card.subtasks off: the main session's steps only; the turn over while subs work.
        {{familyOff, std::nullopt, rc::View::Card, now, {}},
         {familyDoneOff, std::nullopt, rc::View::Card, now, {}},
         {familyAskOff, std::nullopt, rc::View::Card, now, {}}},
    };
    // 보내기 opening its row to type in (rc::Motion::say), and the row folding back unsent: Stop, 보내기, Open on a
    // working T3 card; the box with the edit box over it (focused), as the window has it once it arrives.
    rc::Card sayRow = both, sayBox = both;
    sayBox.composing = sayBox.focused = true;
    auto opening = [&](float k) {
        return moment([&](rc::Motion& m) {
            m.say = k;
            m.sayWas = &sayRow;
        });
    };
    auto folding = [&](float k) {
        return moment([&](rc::Motion& m) {
            m.say = k;
            m.sayBack = true;
            m.sayWas = &sayBox;
        });
    };
    std::vector<std::vector<Shot>> say(2);
    for (float k : {0.0f, 0.12f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        say[0].push_back({sayBox, std::nullopt, rc::View::Card, now, {}, opening(k)});
        say[1].push_back({sayRow, std::nullopt, rc::View::Card, now, {}, folding(k)});
    }
    {
        rc::Palette dark = rc::MakePalette(false, accentDark), light = rc::MakePalette(true, accentLight);
        auto laid = [&](const rc::Card& c, const std::optional<rc::Motion>& m) {
            return painter.Run(nullptr, c, nullptr, rc::View::Card, dark, now, {}, ko, false, m ? &*m : nullptr);
        };
        float restH = laid(sayRow, std::nullopt).h;
        bool steady = laid(sayBox, std::nullopt).h == restH;
        for (int i = 0; i <= 20; i++) {
            float k = i / 20.0f;
            steady = steady && laid(sayBox, opening(k)).h == restH && laid(sayRow, folding(k)).h == restH;
        }
        Check(steady, "send morph: every moment, opening and folding, keeps the card's height");
        bool ends = true, nears = true, still = true, moves = true;
        for (const rc::Palette* p : {&dark, &light})
            for (rc::Pointer ptr : {rc::Pointer{}, rc::Pointer{rc::Act::Reply}}) {
                Picture row = Draw1(d2d.Get(), wic.Get(), painter, {sayRow, std::nullopt, rc::View::Card, now, ptr}, *p, ko);
                Picture box = Draw1(d2d.Get(), wic.Get(), painter, {sayBox, std::nullopt, rc::View::Card, now, ptr}, *p, ko);
                Picture open0 = Draw1(d2d.Get(), wic.Get(), painter, {sayBox, std::nullopt, rc::View::Card, now, ptr, opening(0)}, *p, ko);
                Picture fold0 = Draw1(d2d.Get(), wic.Get(), painter, {sayRow, std::nullopt, rc::View::Card, now, ptr, folding(0)}, *p, ko);
                ends = ends && SamePixels(open0, row) && SameHits(open0.laid, row.laid) && SamePixels(fold0, box) &&
                       SameHits(fold0.laid, box.laid) && open0.laid.h == row.laid.h && fold0.laid.h == box.laid.h;
                // The row's place: 보내기's.
                float top = 0, bottom = 0;
                for (auto& h : row.laid.hits)
                    if (h.act == rc::Act::Reply) top = h.r.top, bottom = h.r.bottom;
                for (float k : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
                    Picture o = Draw1(d2d.Get(), wic.Get(), painter, {sayBox, std::nullopt, rc::View::Card, now, ptr, opening(k)}, *p, ko);
                    Picture f = Draw1(d2d.Get(), wic.Get(), painter, {sayRow, std::nullopt, rc::View::Card, now, ptr, folding(k)}, *p, ko);
                    // Clicks: the end each moment is nearer (the box past the shape's halfway).
                    float go = rc::Bezier(rc::kSayEase, k);
                    nears = nears && SameHits(o.laid, go >= 0.5f ? box.laid : row.laid) && SameHits(f.laid, 1 - go >= 0.5f ? box.laid : row.laid);
                    // Nothing but the row moves (its blur may reach a few DIPs past it), and it does move.
                    still = still && o.h == row.h && f.h == row.h && SamePixels(o, row, 0, top - 8) && SamePixels(o, row, bottom + 8) &&
                            SamePixels(f, row, 0, top - 8) && SamePixels(f, row, bottom + 8);
                    moves = moves && !SamePixels(o, row) && !SamePixels(o, box) && !SamePixels(f, row) && !SamePixels(f, box);
                }
            }
        Check(ends, "send morph: opening at 0 is the buttons' row, folding at 0 the row to type in (pixels, clicks, height)");
        Check(nears, "send morph: in between it answers clicks as the end it is nearer, opening and folding");
        Check(still && moves, "send morph: in between only the row changes, and it changes");
        // Turned back halfway (Changes, rc::SayTurn): the new way starts with the shape where it is and the fades where
        // they are, so the picture does not jump, either way and turned twice; by the end the fades are the way's own.
        auto turned = [&](bool back, float k, float turn, rc::SayFade was) {
            return moment([&](rc::Motion& m) {
                m.say = k;
                m.sayBack = back;
                m.sayWas = back ? &sayBox : &sayRow;
                m.sayTurn = turn;
                m.sayTurnWas = was;
            });
        };
        bool turns = true, settles = true;
        for (const rc::Palette* p : {&dark, &light})
            for (float k : {0.1f, 0.2f, 0.35f, 0.5f, 0.7f}) {
                rc::SayTurned toFold = rc::SayTurn(false, k, -1, {}), toOpen = rc::SayTurn(true, k, -1, {});
                rc::SayTurned again = rc::SayTurn(true, std::min(1.0f, toFold.k + 0.1f), toFold.k, toFold.was);
                auto draw = [&](const rc::Card& c, const std::optional<rc::Motion>& m) {
                    return Draw1(d2d.Get(), wic.Get(), painter, {c, std::nullopt, rc::View::Card, now, {}, m}, *p, ko);
                };
                turns = turns && NearPixels(draw(sayBox, opening(k)), draw(sayRow, turned(true, toFold.k, toFold.k, toFold.was)), 16) &&
                        NearPixels(draw(sayRow, folding(k)), draw(sayBox, turned(false, toOpen.k, toOpen.k, toOpen.was)), 16) &&
                        NearPixels(draw(sayRow, turned(true, std::min(1.0f, toFold.k + 0.1f), toFold.k, toFold.was)),
                                   draw(sayBox, turned(false, again.k, again.k, again.was)), 16);
                for (bool back : {false, true}) {
                    rc::SayTurned t = back ? toOpen : toFold;
                    rc::SayFade end = rc::SayFades(!back, 1, t.k, t.was), own = rc::SayFades(!back, 1);
                    settles = settles && end.others == own.others && end.word == own.word;
                }
            }
        Check(turns, "send morph: turned back halfway (twice too), the new way goes on from the picture as it was");
        Check(settles, "send morph: a way turned back ends as its own (the fades settled)");
        // The shape: the button's at 0, growing both ways, the box's at the end; folding the same backwards.
        auto spans = [&](const std::optional<rc::Motion>& m, const rc::Card& c) {
            rc::Laid L = laid(c, m);
            for (auto& h : L.hits)
                if (h.act == rc::Act::Field || h.act == rc::Act::Reply) return h.r;
            return D2D1_RECT_F{};
        };
        D2D1_RECT_F btn = spans(std::nullopt, sayRow), fld = spans(std::nullopt, sayBox);
        Check(fld.left < btn.left && fld.right > btn.right && spans(opening(0.9f), sayBox).left == fld.left &&
                  spans(folding(0.9f), sayRow).left == btn.left,
              "send morph: the box spans the row on both sides of 보내기; the clicks follow the shape's way");
    }
    rows.insert(rows.end(), say.begin(), say.end());
    if (!Render(d2d.Get(), wic.Get(), painter, rows, ko, accentDark, accentLight, argv[1])) return 4;
    // The 보내기 strips alone, for a closer look.
    std::wstring strip = argv[1];
    if (strip.size() > 4 && strip.ends_with(L".png")) strip.resize(strip.size() - 4);
    strip += L"-send.png";
    if (!Render(d2d.Get(), wic.Get(), painter, say, ko, accentDark, accentLight, strip.c_str())) return 5;
    return g_failed ? 1 : 0;
}
