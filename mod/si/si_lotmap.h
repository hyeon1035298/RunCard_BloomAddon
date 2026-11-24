// SPDX-License-Identifier: GPL-3.0-only
//
// Run card's transcript reader: the lot-map functions the board builder uses, ported from lot-map v0.7.0 (MIT,
// Copyright (c) the lot-map authors; THIRD_PARTY.md): hooks/classify.ts (classify, clean, shortModel, formatMs,
// memoryIds), hooks/feed.ts (foldRows, taskOf, taskNotice, nowParts), hooks/text.ts (the English strings the board
// uses) and hooks/setup.ts (setupFrom({}) defaults). Labels are UTF-16, as JS strings are, so lengths and cuts
// (clean's 120, a host's 40, the row's 28) count the same units lot-map counts.
//
// Privacy: classify makes a name of a tool's input (a file's last two parts, a host, a command's description or
// first word, a skill name, an agent description); nothing else of the input reaches a label.

#pragma once

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "si_json.h"
#include "si_model.h"

namespace si {
namespace lotmap {

// ---- text helpers -----------------------------------------------------------------------------------

// UTF-8 to UTF-16 as Buffer.toString('utf8') reads it: a malformed sequence becomes U+FFFD.
inline std::wstring Widen(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint8_t b = uint8_t(s[i]);
        if (b < 0x80) {
            out.push_back(wchar_t(b));
            i++;
            continue;
        }
        int need = 0;
        uint8_t lo = 0x80, hi = 0xBF;
        uint32_t code = 0;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
            code = b & 0x1F;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            code = b & 0x0F;
            if (b == 0xE0) lo = 0xA0;
            if (b == 0xED) hi = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            code = b & 0x07;
            if (b == 0xF0) lo = 0x90;
            if (b == 0xF4) hi = 0x8F;
        } else {
            out.push_back(L'\xFFFD');
            i++;
            continue;
        }
        i++;
        bool ok = true;
        for (int k = 0; k < need; k++) {
            if (i >= s.size()) {
                ok = false;
                break;
            }
            uint8_t c = uint8_t(s[i]);
            if (c < lo || c > hi) {
                ok = false;
                break;
            }
            lo = 0x80;
            hi = 0xBF;
            code = (code << 6) | (c & 0x3F);
            i++;
        }
        if (!ok) {
            out.push_back(L'\xFFFD');
            continue;
        }
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(wchar_t(0xD800 + (code >> 10)));
            out.push_back(wchar_t(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(wchar_t(code));
        }
    }
    return out;
}

// UTF-16 to UTF-8; a lone surrogate becomes U+FFFD.
inline std::string Narrow(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = uint16_t(s[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && uint16_t(s[i + 1]) >= 0xDC00 &&
            uint16_t(s[i + 1]) <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint16_t(s[i + 1]) - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// JS `\s` and String.prototype.trim's white space.
inline bool IsJsSpace(wchar_t c) {
    return c == L' ' || (c >= 0x09 && c <= 0x0D) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
inline std::wstring_view TrimStartJs(std::wstring_view s) {
    size_t a = 0;
    while (a < s.size() && IsJsSpace(s[a])) a++;
    return s.substr(a);
}
inline std::wstring_view TrimJs(std::wstring_view s) {
    s = TrimStartJs(s);
    size_t b = s.size();
    while (b > 0 && IsJsSpace(s[b - 1])) b--;
    return s.substr(0, b);
}
inline bool StartsWith(std::wstring_view s, std::wstring_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}
inline bool EndsWith(std::wstring_view s, std::wstring_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
// String.prototype.toLowerCase, by the invariant locale's simple case mapping.
inline std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    if (out.empty()) return out;
    bool ascii = true;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = wchar_t(c + 32);
        else if (c >= 0x80) ascii = false;
    }
    if (ascii) return out;
    std::wstring src(s);
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, src.c_str(), int(src.size()), nullptr, 0, nullptr,
                          nullptr, 0);
    if (n <= 0) return out;
    std::wstring low(size_t(n), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, src.c_str(), int(src.size()), low.data(), n, nullptr,
                  nullptr, 0);
    return low;
}
inline bool IsWordChar(wchar_t c) {  // JS `\w` (ASCII)
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_';
}
inline bool IsDigit(wchar_t c) {
    return c >= L'0' && c <= L'9';
}
inline std::wstring IntText(int64_t n) {
    return std::to_wstring(n);
}
// Math.round: the closest integer, halves up.
inline double JsRound(double x) {
    double r = std::floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}
// A string from a transcript field: its text when it is a string, else empty (lot-map's `str`).
inline std::wstring Str(json::Val v) {
    return v.IsString() ? Widen(v.Str()) : std::wstring();
}

// ---- text.ts: the English strings the board uses -----------------------------------------------------

namespace t {
inline const wchar_t* const kNewMessage = L"new message";
inline const wchar_t* const kAnswerReady = L"answer ready";
inline const wchar_t* const kTurnAborted = L"turn interrupted";
inline const wchar_t* const kSubagent = L"subagent";
inline const wchar_t* const kCompacting = L"compacting context";
inline const wchar_t* const kToMessage = L"to the message";
inline const wchar_t* const kAtStart = L"at session start";
inline const wchar_t* const kRunning = L"running";
inline const wchar_t* const kIdle = L"Nothing is running.";
inline const wchar_t* const kWebSearch = L"web search";
inline const wchar_t* const kArtifact = L"artifact";
inline const wchar_t* const kWorkflow = L"workflow";
inline const wchar_t* const kSkill = L"skill";
inline const wchar_t* const kLoadingTools = L"loading tools";
inline const wchar_t* const kQuestionForYou = L"question for you";
inline const wchar_t* const kFileForYou = L"file for you";
inline std::wstring Step(int n) {
    return L"step " + IntText(n);
}
inline std::wstring AddedContext(std::wstring_view when) {
    return L"added context " + std::wstring(when);
}
inline std::wstring AddedMemory(std::wstring_view when) {
    return L"added memory " + std::wstring(when);
}
inline std::wstring Records(size_t n, std::wstring_view shown) {
    return IntText(int64_t(n)) + L" record" + (n == 1 ? L"" : L"s") + L": " + std::wstring(shown);
}
inline std::wstring Stopped(std::wstring_view tool) {
    return L"stopped " + std::wstring(tool);
}
inline std::wstring AndMore(size_t n) {
    return L" and " + IntText(int64_t(n)) + L" more";
}
}  // namespace t

inline const wchar_t* StationName(Station s) {
    return kStationNames[Index(s)];
}

// ---- setup.ts: the options the board's labels are made with -------------------------------------------
// The board always uses setupFrom({}): no memory servers, commands or paths, no hook records (prototype BASE_SETUP).

struct Setup {
    std::vector<std::wstring> memoryServers;
    std::vector<std::wstring> memoryCommands;
    std::vector<std::wstring> memorySubcommands;
    std::vector<std::wstring> memoryPaths;
    std::vector<std::pair<std::wstring, std::wstring>> memoryLabels;
    bool hookRecords = false;
};
inline const Setup& BaseSetup() {
    static const Setup base;
    return base;
}
inline bool Has(const std::vector<std::wstring>& set, std::wstring_view v) {
    for (auto const& x : set)
        if (x == v) return true;
    return false;
}

// ---- classify.ts ------------------------------------------------------------------------------------

struct Classified {
    Station station = Station::Shell;
    std::wstring label;
};

inline std::wstring Slashes(std::wstring_view path) {
    std::wstring p(path);
    for (wchar_t& c : p)
        if (c == L'\\') c = L'/';
    return p;
}

inline std::vector<std::wstring> SplitNonEmpty(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            if (i > start) out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

inline std::wstring LastTwo(std::wstring_view path) {
    auto parts = SplitNonEmpty(Slashes(path), L'/');
    size_t from = parts.size() > 2 ? parts.size() - 2 : 0;
    std::wstring out;
    for (size_t i = from; i < parts.size(); i++) {
        if (!out.empty() || i > from) out += L'/';
        out += parts[i];
    }
    return out;
}

// \p{L} and \p{N} of HOST, by the system's character types (astral code points: the CJK planes count as letters).
inline bool IsHostLetter(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
    if (cp >= 0x10000) return cp >= 0x20000 && cp <= 0x3FFFF;
    WORD type = 0;
    wchar_t c = wchar_t(cp);
    if (!GetStringTypeW(CT_CTYPE1, &c, 1, &type)) return false;
    return (type & (C1_ALPHA | C1_DIGIT)) != 0;
}

// HOST = /^(?:[\p{L}\p{N}.-]+|\[[0-9a-f:.]+\])(?::\d+)?$/iu
inline bool IsHost(std::wstring_view h) {
    size_t i = 0;
    if (!h.empty() && h[0] == L'[') {
        i = 1;
        size_t start = i;
        while (i < h.size() && (IsDigit(h[i]) || (h[i] >= L'a' && h[i] <= L'f') || (h[i] >= L'A' && h[i] <= L'F') ||
                                h[i] == L':' || h[i] == L'.'))
            i++;
        if (i == start || i >= h.size() || h[i] != L']') return false;
        i++;
    } else {
        size_t start = i;
        while (i < h.size()) {
            uint32_t cp = uint16_t(h[i]);
            size_t width = 1;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < h.size() && uint16_t(h[i + 1]) >= 0xDC00 &&
                uint16_t(h[i + 1]) <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (uint16_t(h[i + 1]) - 0xDC00);
                width = 2;
            }
            if (cp == '.' || cp == '-' || IsHostLetter(cp)) i += width;
            else break;
        }
        if (i == start) return false;
    }
    if (i < h.size() && h[i] == L':') {
        size_t d = i + 1;
        while (d < h.size() && IsDigit(h[d])) d++;
        if (d == i + 1) return false;
        i = d;
    }
    return i == h.size();
}

// Only the host: a login, a password, a path or a query never reach a label.
inline std::wstring HostOf(std::wstring_view url) {
    // /^(?:([a-z][a-z0-9+.-]*):\/\/)?(.*)$/is
    std::wstring scheme;
    std::wstring_view rest = url;
    {
        size_t i = 0;
        auto alpha = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); };
        if (!url.empty() && alpha(url[0])) {
            i = 1;
            while (i < url.size() && (alpha(url[i]) || IsDigit(url[i]) || url[i] == L'+' || url[i] == L'.' ||
                                      url[i] == L'-'))
                i++;
            if (url.substr(i, 3) == L"://") {
                scheme = std::wstring(url.substr(0, i));
                rest = url.substr(i + 3);
            }
        }
    }
    size_t end = rest.find_first_of(L"/?#");
    std::wstring_view first = end == std::wstring_view::npos ? rest : rest.substr(0, end);
    std::wstring_view beyond = end == std::wstring_view::npos ? std::wstring_view() : rest.substr(end);
    size_t at = first.rfind(L'@');
    std::wstring host(at == std::wstring_view::npos ? first : first.substr(at + 1));
    if (!(StartsWith(first, L"[")) && first.find(L'@') == std::wstring_view::npos &&
        first.find(L':') != std::wstring_view::npos && beyond.find(L'@') != std::wstring_view::npos) {
        // `name:word` before an `@` further on: a port if the word is a number, or else a login and a password.
        size_t colon = first.rfind(L':');
        bool port = colon + 1 < first.size();
        for (size_t k = colon + 1; k < first.size(); k++)
            if (!IsDigit(first[k])) port = false;
        if (port) {
            host.clear();
        } else {
            std::wstring_view after = rest.substr(rest.rfind(L'@') + 1);
            size_t stop = after.find_first_of(L"/?#");
            host = std::wstring(stop == std::wstring_view::npos ? after : after.substr(0, stop));
        }
    }
    return IsHost(host) ? host.substr(0, 40) : scheme;
}

// A label as it may be shown: no control characters and no direction marks, one line, at most `max` units.
inline std::wstring Clean(std::wstring_view label, size_t max = 120) {
    auto bad = [](wchar_t c) {
        return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
               (c >= 0x2066 && c <= 0x2069);
    };
    std::wstring a;
    a.reserve(label.size());
    for (size_t i = 0; i < label.size();) {
        if (bad(label[i])) {
            while (i < label.size() && bad(label[i])) i++;
            a += L' ';
        } else {
            a += label[i++];
        }
    }
    std::wstring b;
    b.reserve(a.size());
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] == L' ' && !b.empty() && b.back() == L' ') continue;
        b += a[i];
    }
    std::wstring text(TrimJs(b));
    if (text.size() > max) return text.substr(0, max - 1) + L"\x2026";
    return text;
}

inline bool IsMemoryPath(std::wstring_view path, const Setup& setup) {
    std::wstring p = Slashes(path);
    static const std::wstring kRoot = L"/.claude/projects/";
    for (size_t at = p.find(kRoot); at != std::wstring::npos; at = p.find(kRoot, at + 1)) {
        size_t j = at + kRoot.size();
        size_t k = p.find(L'/', j);
        if (k != std::wstring::npos && k > j && p.compare(k, 8, L"/memory/") == 0) return true;
    }
    for (auto const& part : setup.memoryPaths)
        if (p.find(Slashes(part)) != std::wstring::npos) return true;
    return false;
}

namespace detail {

using Words = std::vector<std::wstring>;

inline bool In(std::initializer_list<const wchar_t*> set, std::wstring_view v) {
    for (auto s : set)
        if (v == s) return true;
    return false;
}

// Commands that work on files and nothing else; commands that say nothing either way; commands that reach the net.
inline bool FileWord(std::wstring_view w) {
    return In({L"cat", L"head", L"tail", L"sed", L"grep", L"rg", L"find", L"ls", L"wc", L"cp", L"mv", L"mkdir", L"touch",
               L"diff", L"cmp", L"sort", L"uniq", L"cut", L"tr", L"awk", L"tee", L"stat", L"file", L"du", L"tree", L"ln",
               L"basename", L"dirname", L"realpath", L"jq", L"rm", L"rmdir"},
              w);
}
inline bool NeutralWord(std::wstring_view w) {
    return In({L"echo", L"printf", L"cd", L"export", L"true", L"set", L"pwd", L"sleep", L"which", L"test", L"["}, w);
}
inline bool NetWord(std::wstring_view w) {
    return In({L"curl", L"wget", L"gh", L"ssh", L"scp", L"sftp", L"ftp", L"rsync", L"nc", L"ncat", L"telnet", L"ping",
               L"dig", L"nslookup"},
              w);
}
inline bool PsFileWord(std::wstring_view w) {
    return In({L"get-content", L"gc", L"type", L"set-content", L"add-content", L"out-file", L"get-childitem", L"gci",
               L"dir", L"get-item", L"gi", L"select-string", L"sls", L"copy-item", L"cpi", L"copy", L"move-item", L"mi",
               L"move", L"rename-item", L"rni", L"ren", L"new-item", L"ni", L"md", L"test-path", L"resolve-path",
               L"get-filehash"},
              w);
}
inline bool PsNeutralWord(std::wstring_view w) {
    return In({L"write-host", L"write-output", L"set-location", L"sl", L"get-location", L"gl", L"start-sleep",
               L"select-object", L"select", L"where-object", L"where", L"?", L"foreach-object", L"foreach", L"%",
               L"sort-object", L"measure-object", L"measure", L"format-table", L"ft", L"format-list", L"fl",
               L"out-string", L"out-null"},
              w);
}
inline bool PsNetWord(std::wstring_view w) {
    return In({L"invoke-webrequest", L"iwr", L"invoke-restmethod", L"irm", L"test-connection", L"test-netconnection",
               L"resolve-dnsname", L"start-bitstransfer"},
              w);
}
// NET_SUBCOMMANDS. Looked up as the table's own names only (lot-map's object lookup also finds `constructor` and
// the other names every JS object has, and then throws; see the report).
inline bool NetSubcommand(std::wstring_view head, std::wstring_view sub, bool& known) {
    struct Row {
        const wchar_t* head;
        std::initializer_list<const wchar_t*> subs;
    };
    static const Row rows[] = {
        {L"git", {L"push", L"pull", L"fetch", L"clone", L"ls-remote"}},
        {L"npm", {L"install", L"i", L"ci", L"add", L"update", L"publish", L"view", L"audit"}},
        {L"pnpm", {L"install", L"i", L"add", L"update", L"publish", L"dlx"}},
        {L"yarn", {L"install", L"add", L"upgrade", L"publish", L"dlx"}},
        {L"bun", {L"install", L"add", L"update", L"publish"}},
        {L"pip", {L"install", L"download"}},
        {L"pip3", {L"install", L"download"}},
        {L"pipx", {L"install", L"upgrade", L"upgrade-all", L"run"}},
        {L"uv", {L"add", L"sync", L"lock", L"publish", L"tool install"}},
        {L"poetry", {L"install", L"add", L"update", L"lock", L"publish"}},
        {L"docker", {L"pull", L"push", L"login"}},
        {L"docker compose", {L"pull", L"push"}},
        {L"brew", {L"install", L"update", L"upgrade", L"fetch"}},
        {L"cargo", {L"install", L"fetch", L"publish", L"update"}},
        {L"go", {L"get", L"install", L"mod download"}},
        {L"apt", {L"install", L"update", L"upgrade", L"full-upgrade", L"dist-upgrade"}},
        {L"apt-get", {L"install", L"update", L"upgrade", L"dist-upgrade"}},
    };
    for (auto const& r : rows) {
        if (head != r.head) continue;
        known = true;
        return In(r.subs, sub);
    }
    known = false;
    return false;
}
inline bool PyModule(std::wstring_view w) {
    return In({L"pip", L"uv", L"pipx", L"poetry"}, w);
}
inline bool Runner(std::wstring_view w) {
    return In({L"python", L"python3", L"bash", L"sh", L"zsh", L"node"}, w);
}
inline bool GitTakes(std::wstring_view w) {
    return In({L"-C", L"-c", L"--git-dir", L"--work-tree", L"--namespace", L"--exec-path", L"--config-env"}, w);
}
inline bool DockerTakes(std::wstring_view w) {
    return In({L"-c", L"--context", L"-H", L"--host", L"--config", L"-l", L"--log-level", L"-f", L"--file", L"-p",
               L"--project-name", L"--profile", L"--env-file", L"--project-directory"},
              w);
}

struct Wrapper {
    const wchar_t* name;
    std::initializer_list<const wchar_t*> takes;
    int skip;
    std::initializer_list<const wchar_t*> asks;
};
inline const Wrapper* WrapperOf(std::wstring_view name) {
    static const Wrapper table[] = {
        {L"sudo", {L"-u", L"-g", L"-p", L"-h", L"-C", L"-D", L"-R", L"-T", L"-U", L"-r", L"-t"}, 0, {}},
        {L"doas", {L"-u", L"-C"}, 0, {}},
        {L"env", {L"-u", L"-C"}, 0, {}},
        {L"time", {}, 0, {}},
        {L"timeout", {L"-s", L"-k"}, 1, {}},
        {L"nohup", {}, 0, {}},
        {L"nice", {L"-n"}, 0, {}},
        {L"ionice", {L"-c", L"-n"}, 0, {}},
        {L"stdbuf", {L"-i", L"-o", L"-e"}, 0, {}},
        {L"caffeinate", {L"-t", L"-w"}, 0, {}},
        {L"command", {}, 0, {L"-v", L"-V"}},
        {L"exec", {L"-a"}, 0, {}},
        {L"xargs", {L"-n", L"-I", L"-P", L"-L", L"-d", L"-E", L"-s", L"-a", L"-J", L"-R"}, 0, {}},
    };
    for (auto const& w : table)
        if (name == w.name) return &w;
    return nullptr;
}

// ASSIGNMENT = /^[A-Za-z_]\w*=/
inline bool IsAssignment(std::wstring_view w) {
    if (w.empty() || !((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z') || w[0] == L'_')) return false;
    size_t i = 1;
    while (i < w.size() && IsWordChar(w[i])) i++;
    return i < w.size() && w[i] == L'=';
}

inline Words Slice(const Words& w, size_t from) {
    return from >= w.size() ? Words() : Words(w.begin() + std::ptrdiff_t(from), w.end());
}

template <class Takes>
Words PastFlags(const Words& words, Takes takes) {
    size_t i = 0;
    while (i < words.size() && StartsWith(words[i], L"-")) i += takes(words[i]) ? 2 : 1;
    return Slice(words, i);
}

// The words of each command in a shell line, leading assignments taken off. Quotes stay.
inline std::vector<Words> Segments(std::wstring_view command, bool deep) {
    std::vector<std::wstring_view> parts;
    size_t start = 0, i = 0;
    while (i < command.size()) {
        wchar_t c = command[i];
        size_t cut = 0;
        if (command.substr(i, 2) == L"&&" || command.substr(i, 2) == L"||") cut = 2;
        else if (c == L';' || c == L'|' || c == L'\n') cut = 1;
        else if (deep && command.substr(i, 2) == L"$(") cut = 2;
        else if (deep && (c == L'`' || c == L'(' || c == L')')) cut = 1;
        if (cut) {
            parts.push_back(command.substr(start, i - start));
            i += cut;
            start = i;
        } else {
            i++;
        }
    }
    parts.push_back(command.substr(start));
    std::vector<Words> out;
    for (auto part : parts) {
        Words words;
        size_t k = 0;
        while (k < part.size()) {
            if (IsJsSpace(part[k])) {
                k++;
                continue;
            }
            size_t from = k;
            while (k < part.size() && !IsJsSpace(part[k])) {
                if (part[k] == L'"' || part[k] == L'\'') {
                    size_t close = part.find(part[k], k + 1);
                    k = close == std::wstring_view::npos ? part.size() : close + 1;
                } else {
                    k++;
                }
            }
            words.emplace_back(part.substr(from, k - from));
        }
        size_t a = 0;
        while (a < words.size() && IsAssignment(words[a])) a++;
        Words rest = Slice(words, a);
        if (!rest.empty()) out.push_back(std::move(rest));
    }
    return out;
}

// A command's own name: no quotes and no folder; a Windows program loses .exe, .cmd or .bat and its capitals.
inline std::wstring Base(std::wstring_view word) {
    std::wstring w;
    for (wchar_t c : word)
        if (c != L'"' && c != L'\'') w += c == L'\\' ? L'/' : c;
    size_t slash = w.rfind(L'/');
    std::wstring name = slash == std::wstring::npos ? w : w.substr(slash + 1);
    if (name.size() >= 4 && name[name.size() - 4] == L'.') {
        std::wstring ext = Lower(std::wstring_view(name).substr(name.size() - 3));
        if (ext == L"exe" || ext == L"cmd" || ext == L"bat") return Lower(std::wstring_view(name).substr(0, name.size() - 4));
    }
    return name;
}

// A script's name without its ending.
inline std::wstring Stem(std::wstring_view name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot + 1 >= name.size()) return std::wstring(name);
    for (size_t i = dot + 1; i < name.size(); i++) {
        wchar_t c = name[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || IsDigit(c))) return std::wstring(name);
    }
    return std::wstring(name.substr(0, dot));
}

inline bool Named(std::wstring_view name, std::wstring_view command) {
    std::wstring s = Stem(name);
    if (s == command) return true;
    return StartsWith(s, command) && s.size() > command.size() &&
           (s[command.size()] == L'-' || s[command.size()] == L'_' || s[command.size()] == L'.');
}

inline std::wstring MemoryLabel(const Words& words, const Setup& setup) {
    std::wstring head = Base(words.empty() ? L"" : words[0]);
    if (!Has(setup.memoryCommands, head)) {
        bool script = Runner(head) && words.size() > 1 && !words[1].empty();
        return Stem(Base(script ? words[1] : (words.empty() ? L"" : words[0])));
    }
    std::wstring sub = words.size() > 1 ? words[1] : L"";
    return Has(setup.memorySubcommands, sub) ? sub : head;
}

// /^[\w.+-]{1,40}$/: what a command's own name looks like.
inline bool LooksLikeCommandName(std::wstring_view s) {
    if (s.empty() || s.size() > 40) return false;
    for (wchar_t c : s)
        if (!(IsWordChar(c) || c == L'.' || c == L'+' || c == L'-')) return false;
    return true;
}

inline bool IsQuote(wchar_t c) {
    return c == L'"' || c == L'\'';
}

// The commands of a shell line, for FallbackLabel. Unlike Segments (lot-map's split, kept for the stations), this one does not cut inside quotes, so a `;` or `|` in a
// quoted value cannot hand a piece of it on as the next command.
inline std::vector<Words> QuotedSegments(std::wstring_view command) {
    std::vector<Words> out;
    Words words;
    std::wstring word;
    bool inWord = false;
    auto endWord = [&] {
        if (inWord) words.push_back(std::move(word));
        word.clear();
        inWord = false;
    };
    auto endSegment = [&] {
        endWord();
        size_t a = 0;
        while (a < words.size() && IsAssignment(words[a])) a++;
        Words rest = Slice(words, a);
        if (!rest.empty()) out.push_back(std::move(rest));
        words.clear();
    };
    for (size_t i = 0; i < command.size();) {
        wchar_t c = command[i];
        if (IsQuote(c)) {
            size_t close = command.find(c, i + 1);
            size_t end = close == std::wstring_view::npos ? command.size() : close + 1;
            word.append(command.substr(i, end - i));
            inWord = true;
            i = end;
        } else if (command.substr(i, 2) == L"&&" || command.substr(i, 2) == L"||") {
            endSegment();
            i += 2;
        } else if (c == L';' || c == L'|' || c == L'\n') {
            endSegment();
            i++;
        } else if (IsJsSpace(c)) {
            endWord();
            i++;
        } else {
            word += c;
            inWord = true;
            i++;
        }
    }
    endSegment();
    return out;
}

// What a shell line without a description is called. A privacy rule of this port that lot-map (classify.ts) and the
// prototype do not have: they show the line's first word, which for `$token = "ghp_..."; ...` is the
// value. Here the label is the first command's own name, and only when it looks like one; otherwise the tool's name.
//   PowerShell: `$x = v`, `$x=v`, `$env:K="v"`, `$x += v` are assignments, and a statement led by a variable is an
//     expression: such a statement names nothing unless its value is itself a command (`$x = Get-Date`,
//     `$r = & "C:\...\git.exe" push`), and the next statement is looked at (`$ErrorActionPreference = 'Stop'; ...`).
//     A quoted word is a string unless `&` or `.` calls it.
//   Bash: the first word (leading NAME=value already taken off) is the command; a quoted one only as a program's path.
inline std::wstring FallbackLabel(std::wstring_view command, bool powershell) {
    std::wstring tool = powershell ? L"PowerShell" : L"Bash";
    std::vector<Words> segments = QuotedSegments(command);
    if (segments.empty()) return L"shell";  // nothing written at all: lot-map's word
    auto isPath = [](std::wstring_view w) { return w.find_first_of(L"/\\") != std::wstring_view::npos; };
    for (auto const& words : segments) {
        std::wstring_view word = words[0];
        size_t next = 1;
        if (powershell && word[0] == L'$') {
            size_t eq = word.find(L'=');
            std::wstring_view value;
            if (eq != std::wstring_view::npos) {
                value = word.substr(eq + 1);  // $x=v, $env:K="v", $x+=v
            } else if (words.size() > 1 && words[1].find(L'=') != std::wstring::npos && words[1].find(L'=') <= 1) {
                value = std::wstring_view(words[1]).substr(words[1].find(L'=') + 1);  // $x = v, $x += v, $x ="v"
                next = 2;
            } else {
                continue;  // led by a variable ($x.Method(), $list | ...): an expression
            }
            if (value.empty()) {
                if (next >= words.size()) continue;
                value = words[next++];
            }
            word = value;
        }
        if (powershell && (word == L"&" || word == L".")) {
            if (next >= words.size()) return tool;
            std::wstring name = Base(words[next]);
            return LooksLikeCommandName(name) ? name : tool;
        }
        if (IsQuote(word[0])) {
            if (powershell) {
                if (words[0][0] == L'$') continue;  // a string put in a variable
                return tool;                        // a string, not a command
            }
            if (!isPath(word)) return tool;  // Bash runs it, but a quoted word without a folder is no program's path
        } else if (powershell && words[0][0] == L'$' &&
                   (IsDigit(word[0]) || word[0] == L'@' || word[0] == L'[' || word[0] == L'(' || word[0] == L'$' ||
                    word[0] == L'{' || word[0] == L'-' || word[0] == L'+')) {
            continue;  // a number, hashtable, array, cast, variable or block put in a variable
        }
        std::wstring name = Base(word);
        return LooksLikeCommandName(name) ? name : tool;
    }
    return tool;
}

}  // namespace detail

// The line of a shell tool. `powershell` adds PowerShell's own command names and reads every name in lower case.
inline Classified ClassifyShell(std::wstring_view command, std::wstring_view description, const Setup& setup,
                                bool powershell = false) {
    using namespace detail;
    auto name = [&](const Words& w, size_t i) {
        std::wstring b = Base(i < w.size() ? std::wstring_view(w[i]) : std::wstring_view());
        return powershell ? Lower(b) : b;
    };
    auto own = [&](const Words& words) {
        if (!powershell) return words;
        size_t i = 0;
        // /^\$[\w:]+$/
        if (!words.empty() && words[0].size() >= 2 && words[0][0] == L'$' && words.size() > 1 && words[1] == L"=") {
            bool ok = true;
            for (size_t k = 1; k < words[0].size(); k++)
                if (!IsWordChar(words[0][k]) && words[0][k] != L':') ok = false;
            if (ok) i = 2;
        }
        if (i < words.size() && (words[i] == L"&" || words[i] == L".")) i++;
        return Slice(words, i);
    };
    auto unwrapped = [&](const Words& words) {
        Words rest = words;
        for (int depth = 0; depth < 6; depth++) {
            const Wrapper* w = WrapperOf(name(rest, 0));
            if (w == nullptr) break;
            if (rest.size() > 1 && In(w->asks, rest[1])) break;
            rest = PastFlags(Slice(rest, 1), [&](std::wstring_view f) { return In(w->takes, f); });
            size_t i = 0;
            while (i < rest.size() && IsAssignment(rest[i])) i++;
            rest = Slice(rest, i + size_t(w->skip));
        }
        return rest;
    };
    auto commands = [&](const Words& words) {
        Words outer = unwrapped(words);
        std::vector<Words> all{outer};
        if (name(outer, 0) == L"find") {
            for (size_t i = 0; i < outer.size(); i++) {
                const std::wstring& word = outer[i];
                if (word != L"-exec" && word != L"-execdir" && word != L"-ok" && word != L"-okdir") continue;
                Words tail = Slice(outer, i + 1);
                size_t end = 0;
                while (end < tail.size() && tail[end] != L";" && tail[end] != L"\\;" && tail[end] != L"\\" &&
                       tail[end] != L"+")
                    end++;
                all.push_back(unwrapped(Words(tail.begin(), tail.begin() + std::ptrdiff_t(end))));
            }
        }
        std::vector<Words> out;
        for (auto& c : all)
            if (!c.empty()) out.push_back(std::move(c));
        return out;
    };
    auto isNet = [&](const Words& words) {
        std::wstring head = name(words, 0);
        Words rest = Slice(words, 1);
        if (NetWord(head) || (powershell && PsNetWord(head))) return true;
        bool py = head == L"py";
        if (StartsWith(head, L"python")) {
            py = true;
            for (size_t k = 6; k < head.size(); k++)
                if (!IsDigit(head[k]) && head[k] != L'.') py = false;
        }
        if (py && !rest.empty() && rest[0] == L"-m" && PyModule(rest.size() > 1 ? rest[1] : L"")) {
            head = rest.size() > 1 ? rest[1] : L"";
            rest = Slice(rest, 2);
        }
        if (head == L"uv" && !rest.empty() && rest[0] == L"pip") {
            head = L"pip";
            rest = Slice(rest, 1);
        }
        if (head == L"git") rest = PastFlags(rest, GitTakes);
        if (head == L"docker") {
            rest = PastFlags(rest, DockerTakes);
            if (!rest.empty() && rest[0] == L"compose") {
                head = L"docker compose";
                rest = PastFlags(Slice(rest, 1), DockerTakes);
            }
        }
        // npx runs what is installed already as readily as it downloads; only its own "yes, install" says which.
        if (head == L"npx") {
            for (auto const& w : rest)
                if (w == L"-y" || w == L"--yes") return true;
            return false;
        }
        std::wstring r0 = rest.size() > 0 ? rest[0] : L"";
        std::wstring r1 = rest.size() > 1 ? rest[1] : L"";
        bool known = false;
        if (NetSubcommand(head, r0, known)) return true;
        return known && NetSubcommand(head, r0 + L" " + r1, known);
    };

    std::vector<Words> lines;
    for (auto& w : Segments(command, false)) {
        Words o = own(w);
        if (!o.empty()) lines.push_back(std::move(o));
    }
    std::wstring trimmed(TrimJs(description));
    // Deliberately not lot-map's `base(lines[0][0])`: see FallbackLabel.
    std::wstring label = !trimmed.empty() ? trimmed : FallbackLabel(command, powershell);
    std::vector<Words> all;
    for (auto const& w : lines)
        for (auto& c : commands(w)) all.push_back(std::move(c));
    for (auto const& words : all) {
        std::wstring head = name(words, 0);
        std::wstring script = Runner(head) && words.size() > 1 && !words[1].empty() ? Base(words[1]) : L"";
        for (auto const& c : setup.memoryCommands)
            if (Named(head, c) || Named(script, c)) return {Station::Memory, MemoryLabel(words, setup)};
    }
    for (auto& w : Segments(command, true))
        for (auto const& c : commands(own(w)))
            if (isNet(c)) return {Station::Web, label};
    auto isNeutral = [&](std::wstring_view head) { return NeutralWord(head) || (powershell && PsNeutralWord(head)); };
    auto isFile = [&](std::wstring_view head) { return FileWord(head) || (powershell && PsFileWord(head)); };
    size_t real = 0;
    bool allFiles = true;
    for (auto const& words : all) {
        std::wstring head = name(words, 0);
        if (isNeutral(head)) continue;
        real++;
        if (!isFile(head)) allFiles = false;
    }
    if (real > 0 && allFiles) return {IsMemoryPath(command, setup) ? Station::Memory : Station::Files, label};
    return {Station::Shell, label};
}

inline std::wstring Underscores(std::wstring_view s) {  // .replace(/_+/g, ' ')
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'_') {
            while (i < s.size() && s[i] == L'_') i++;
            out += L' ';
        } else {
            out += s[i++];
        }
    }
    return out;
}

inline bool IsLineEnd(wchar_t c) {  // what `.` does not match
    return c == L'\n' || c == L'\r' || c == 0x2028 || c == 0x2029;
}

// Which station a tool call belongs to and what to call it: a name, never content. lot-map classify.ts.
inline Classified Classify(std::wstring_view tool, json::Val input, const Setup& setup = BaseSetup()) {
    auto in = [&](const char* key) { return Str(input[key]); };
    auto is = [&](std::initializer_list<const wchar_t*> names) { return detail::In(names, tool); };
    std::wstring name(tool);
    if (is({L"Read", L"Write", L"Edit", L"MultiEdit", L"NotebookEdit"})) {
        std::wstring path = in("file_path");
        if (path.empty()) path = in("notebook_path");
        std::wstring two = LastTwo(path);
        return {IsMemoryPath(path, setup) ? Station::Memory : Station::Files, two.empty() ? name : two};
    }
    if (is({L"Grep", L"Glob"})) return {Station::Files, name};
    if (tool == L"Bash") return ClassifyShell(in("command"), in("description"), setup);
    if (tool == L"PowerShell") return ClassifyShell(in("command"), in("description"), setup, true);
    if (tool == L"WebFetch") return {Station::Web, HostOf(in("url"))};
    if (tool == L"WebSearch") return {Station::Web, t::kWebSearch};
    if (tool == L"Artifact") return {Station::Web, t::kArtifact};
    if (is({L"ArtifactComments", L"ArtifactData", L"SendToProject", L"SendToProjectContext"})) return {Station::Web, name};
    if (tool == L"Agent") {
        std::wstring d = in("description");
        return {Station::Agents, d.empty() ? std::wstring(t::kSubagent) : d};
    }
    if (tool == L"Monitor") {
        std::wstring d = in("description");
        return {Station::Shell, d.empty() ? name : d};
    }
    if (tool == L"Workflow") return {Station::Agents, t::kWorkflow};
    if (is({L"SendMessage", L"TaskStop", L"ListAgents", L"CronCreate", L"CronDelete", L"CronList", L"ScheduleWakeup",
            L"RemoteTrigger"}))
        return {Station::Agents, name};
    if (tool == L"Skill") {
        std::wstring s = in("skill");
        return {Station::Skills, s.empty() ? std::wstring(t::kSkill) : s};
    }
    if (tool == L"ToolSearch") return {Station::Skills, t::kLoadingTools};
    if (is({L"ListSkills", L"SearchSkills", L"SuggestSkills", L"ListPlugins", L"SearchPlugins", L"SuggestPluginInstall"}))
        return {Station::Skills, name};
    if (tool == L"AskUserQuestion") return {Station::You, t::kQuestionForYou};
    if (tool == L"SendUserFile") return {Station::You, t::kFileForYou};
    if (is({L"TaskCreate", L"TaskUpdate", L"TaskList", L"TaskGet", L"TodoWrite", L"EnterPlanMode", L"ExitPlanMode",
            L"PushNotification", L"ReportFindings"}))
        return {Station::You, name};

    // /^mcp__([^_].*?)__(.+)$/
    std::wstring server, action;
    bool mcp = false;
    if (StartsWith(tool, L"mcp__") && tool.size() > 5 && tool[5] != L'_') {
        for (size_t j = 6; j + 2 < tool.size(); j++) {
            if (IsLineEnd(tool[j - 1]) && j > 6) break;  // the server's `.*?` part
            if (tool[j] != L'_' || tool[j + 1] != L'_') continue;
            std::wstring_view rest = tool.substr(j + 2);
            bool clean = true;
            for (wchar_t c : rest)
                if (IsLineEnd(c)) clean = false;
            if (!clean) break;
            server = std::wstring(tool.substr(5, j - 5));
            action = std::wstring(rest);
            mcp = true;
            break;
        }
    }
    if (!mcp) return {Station::Shell, name};
    if (Has(setup.memoryServers, server)) {
        // A number, or a short identifier: a longer string under `id` could be anything.
        std::wstring id;
        json::Val v = input["id"];
        if (v.IsNumber()) {
            double n = v.Num();
            if (n == std::floor(n) && std::fabs(n) < 1e15) id = L" #" + IntText(int64_t(n));
        } else if (v.IsString()) {
            std::wstring s = Widen(v.Str());
            bool ok = !s.empty() && s.size() <= 24;
            for (wchar_t c : s)
                if (!IsWordChar(c) && c != L'-') ok = false;
            if (ok) id = L" #" + s;
        }
        std::wstring label = Underscores(action);
        for (auto const& [k, v2] : setup.memoryLabels)
            if (k == action) label = v2;
        return {Station::Memory, label + id};
    }
    // A cloud session linked to the person's computer reaches the same browser under `remote-devices`.
    bool remote = server == L"remote-devices" && StartsWith(action, L"Claude_Browser__") && action.size() > 16;
    if (server == L"Claude_Browser" || server == L"claude-in-chrome" || remote) {
        std::wstring url = in("url");
        std::wstring act = remote ? action.substr(16) : action;
        bool scheme = false;
        size_t k = 0;
        while (k < url.size() && ((url[k] >= L'a' && url[k] <= L'z') || (url[k] >= L'A' && url[k] <= L'Z'))) k++;
        scheme = k > 0 && k < url.size() && url[k] == L':';
        return {Station::Browser, !url.empty() && scheme ? HostOf(url) : Underscores(act)};
    }
    if (server == L"terminal") return {Station::Shell, Underscores(action)};
    if (server == L"visualize" || StartsWith(server, L"ccd_")) return {Station::You, Underscores(action)};
    if (server == L"plugin-authoring") return {Station::Skills, Underscores(action)};
    return {Station::Web, server + L": " + Underscores(action)};
}

// `claude-opus-5-5` reads as `opus 5.5`. lot-map classify.ts shortModel.
inline std::wstring ShortModel(std::string_view model) {
    // /claude-([a-z]+)-(\d+)(?:-(\d+))?/
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t at = model.find("claude-"); at != std::string_view::npos; at = model.find("claude-", at + 1)) {
        size_t i = at + 7, a = i;
        while (i < model.size() && model[i] >= 'a' && model[i] <= 'z') i++;
        if (i == a || i >= model.size() || model[i] != '-') continue;
        size_t b = ++i;
        while (i < model.size() && digit(model[i])) i++;
        if (i == b) continue;
        std::string out = std::string(model.substr(a, b - 1 - a)) + " " + std::string(model.substr(b, i - b));
        if (i + 1 < model.size() && model[i] == '-' && digit(model[i + 1])) {
            size_t c = ++i;
            while (i < model.size() && digit(model[i])) i++;
            out += "." + std::string(model.substr(c, i - c));
        }
        return Widen(out);
    }
    return Widen(model);
}

// lot-map classify.ts formatMs, English: "12 ms", "2.4 s" (Number.prototype.toFixed(1)), "1 min 5 s".
inline std::wstring FormatMs(int64_t ms) {
    if (ms < 1000) return IntText(ms < 1 ? 1 : ms) + L" ms";
    if (ms < 60000) {
        // toFixed(1) of ms / 1000 exactly as the spec does it: the n closest to x * 10, the larger one on a tie.
        double x = double(ms) / 1000.0;
        int e = 0;
        double m = std::frexp(x, &e);
        uint64_t mant = uint64_t(std::ldexp(m, 53));
        int shift = 53 - e;
        uint64_t y = mant * 10;
        uint64_t n = y >> shift;
        uint64_t rem = y & ((uint64_t(1) << shift) - 1);
        if (rem >= (uint64_t(1) << (shift - 1))) n++;
        return IntText(int64_t(n / 10)) + L"." + IntText(int64_t(n % 10)) + L" s";
    }
    int64_t r = ms % 60000;
    return IntText(ms / 60000) + L" min " + IntText((r + 500) / 1000) + L" s";
}

// The record numbers a memory hook put in front of the model: `[#123`, `[now #123`, `[todo #123`.
inline std::vector<std::wstring> MemoryIds(std::wstring_view text) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != L'[') {
            i++;
            continue;
        }
        size_t j = i + 1;
        size_t k = j;
        while (k < text.size() && text[k] >= L'a' && text[k] <= L'z') k++;
        if (k > j && k < text.size() && text[k] == L' ') j = k + 1;
        if (j < text.size() && text[j] == L'#') {
            size_t d = j + 1;
            while (d < text.size() && IsDigit(text[d])) d++;
            if (d > j + 1) {
                std::wstring id(text.substr(j + 1, d - j - 1));
                if (!Has(out, id)) out.push_back(id);
                i = d;
                continue;
            }
        }
        i++;
    }
    return out;
}

// ---- feed.ts ----------------------------------------------------------------------------------------

// /^[\w-]{1,64}$/
inline bool IsTaskId(std::string_view v) {
    if (v.empty() || v.size() > 64) return false;
    for (char c : v)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

// The id of the background task a tool's answer started, if it started one. lot-map feed.ts taskOf.
inline std::optional<std::string> TaskOf(json::Val r) {
    if (!r.IsObjectLike()) return std::nullopt;
    auto id = [](json::Val v) -> std::optional<std::string> {
        if (v.IsString() && IsTaskId(v.Str())) return std::string(v.Str());
        return std::nullopt;
    };
    json::Val status = r["status"];
    if (status.IsString() && (status.Str() == "async_launched" || status.Str() == "remote_launched")) {
        if (auto a = id(r["agentId"])) return a;
        return id(r["taskId"]);
    }
    if (auto b = id(r["backgroundTaskId"])) return b;
    return r["timeoutMs"].IsNumber() ? id(r["taskId"]) : std::nullopt;
}

struct Notice {
    std::string id;
    std::optional<Status> status;  // none: the task spoke and goes on
};

// A background task's notification, read for the task's id and how it ended. lot-map feed.ts taskNotice.
inline std::optional<Notice> TaskNotice(std::wstring_view text) {
    if (!StartsWith(TrimStartJs(text), L"<task-notification>")) return std::nullopt;
    auto tagged = [&](std::wstring_view open, std::wstring_view close, size_t most) -> std::optional<std::wstring> {
        for (size_t at = text.find(open); at != std::wstring_view::npos; at = text.find(open, at + 1)) {
            size_t i = at + open.size(), a = i;
            while (i < text.size() && i - a < most && (IsWordChar(text[i]) || (most == 64 && text[i] == L'-'))) i++;
            if (i > a && text.substr(i, close.size()) == close) return std::wstring(text.substr(a, i - a));
        }
        return std::nullopt;
    };
    auto id = tagged(L"<task-id>", L"</task-id>", 64);
    if (!id) return std::nullopt;
    Notice n{Narrow(*id), std::nullopt};
    auto word = tagged(L"<status>", L"</status>", size_t(-1));
    if (word) n.status = *word == L"failed" ? Status::Err : Status::Ok;
    return n;
}

// A row of the feed: one entry, or several finished ones in a row at one station, shown as the newest of them.
struct FoldedRow {
    const Entry* entry = nullptr;
    int times = 1;
    int64_t tookMs = 0;
};

// The newest rows first. Finished calls that follow one another at one station fold into a single row.
inline std::vector<FoldedRow> FoldRows(const std::vector<Entry>& entries, size_t max = 16) {
    auto plain = [](const Entry& e) { return e.status == Status::Ok && e.mark == Mark::None; };
    std::vector<FoldedRow> rows;
    for (size_t k = entries.size(); k-- > 0;) {
        const Entry& e = entries[k];
        int64_t took = e.endedAt ? std::max<int64_t>(0, *e.endedAt - e.startedAt) : 0;
        if (!rows.empty()) {
            FoldedRow& last = rows.back();
            if (plain(e) && plain(*last.entry) && last.entry->station == e.station && last.entry->from == e.from) {
                last.times++;
                last.tookMs += took;
                continue;
            }
        }
        if (rows.size() == max) break;
        rows.push_back({&e, 1, took});
    }
    return rows;
}

// What runs now, and how full the context is with the model's name. lot-map feed.ts nowParts.
inline void NowParts(const Board& board, std::wstring& what, std::wstring& meter) {
    const Entry* top = nullptr;
    size_t running = 0;
    for (auto const& e : board.entries) {
        if (e.status != Status::Run) continue;
        running++;
        top = &e;
    }
    what = top == nullptr ? std::wstring(t::kIdle)
                          : std::wstring(StationName(top->station)) + L": " + top->label +
                                (running > 1 ? t::AndMore(running - 1) : L"");
    meter.clear();
    if (board.context) meter = IntText(int64_t(JsRound(*board.context))) + L"%";
    // figures.filter(Boolean): an empty model name drops out with its separator.
    std::wstring model;
    if (board.model && !(top != nullptr && top->station == Station::Model)) model = ShortModel(*board.model);
    if (!model.empty()) meter += (meter.empty() ? L"" : L" \x00B7 ") + model;
}

}  // namespace lotmap
}  // namespace si
