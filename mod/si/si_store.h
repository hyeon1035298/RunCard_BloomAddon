// SPDX-License-Identifier: GPL-3.0-only
//
// Run card's transcript reader: the sessions being watched, the JavaScript prototype's lib/tail.mjs, store.mjs and
// window.mjs in C++, and the Reader: one worker thread that turns Claude Code transcripts into Snapshots.
//
//   %USERPROFILE%\.claude\projects\<project>\<session>.jsonl                     a session's main transcript
//   ...\<project>\<session>\subagents\**\agent-<id>.jsonl                         its subagents (workflows too)
//   ...\.claude\settings.json                                                     the context window setting
//
// Reads only; never writes anywhere. Every file is opened for one read and closed again, with every share mode, so
// Claude Code can append, rename or delete it meanwhile. The thread sleeps while the projects folder is quiet:
// ReadDirectoryChangesW wakes it, and besides that only a slow rescan and the moment a session's activity window
// runs out do.

#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "si_board.h"
#include "si_json.h"
#include "si_lotmap.h"
#include "si_model.h"

namespace si {
namespace store {

constexpr uint64_t kFirstReadMax = 2 * 1024 * 1024;  // tail.mjs FIRST_READ_MAX: a main transcript's first read
// A subagent file's first read, and all first reads of one session's Load together (the most recent files first).
// The prototype reads 2 MB of every subagent written within the window; the same 2 MB here keeps the station counts
// and step numbers the prototype shows, and kLoadBudget bounds a session with many active subagents.
constexpr uint64_t kSubFirstReadMax = 2 * 1024 * 1024;
constexpr uint64_t kLoadBudget = 8 * 1024 * 1024;
// How far back from a main transcript's first read the prompt of a turn still running is looked for.
constexpr uint64_t kTurnScanMax = 64 * 1024 * 1024;
constexpr size_t kChunk = 1024 * 1024;               // one ReadFile at most this much
constexpr size_t kMaxLine = 8 * 1024 * 1024;         // a longer line is skipped (the prototype keeps any)
constexpr int kSubDepth = 4;                         // store.mjs SUB_DEPTH
constexpr Ms kQuietSubPollMs = 30000;                // store.mjs QUIET_SUB_POLL_MS
constexpr uint64_t kSettingsMax = 1024 * 1024;

inline Ms FileTimeMs(const FILETIME& t) {
    ULARGE_INTEGER u{{t.dwLowDateTime, t.dwHighDateTime}};
    return Ms(u.QuadPart / 10000) - 11644473600000LL;
}
inline Ms WallMs() {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    return FileTimeMs(t);
}

struct FileStat {
    uint64_t size = 0;
    Ms mtime = 0;
};
// A file's size and last write, from the file itself (a directory listing can lag behind a file being written).
inline bool StatFile(const std::wstring& path, FileStat& out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    out.size = uint64_t(d.nFileSizeHigh) << 32 | d.nFileSizeLow;
    out.mtime = FileTimeMs(d.ftLastWriteTime);
    return true;
}
inline bool IsDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Counters for tests and the smoke run.
struct Counters {
    std::atomic<uint64_t> filesOpened{0};
    std::atomic<uint64_t> bytesRead{0};
    std::atomic<uint64_t> linesParsed{0};
    std::atomic<uint64_t> filesStatted{0};  // a file's size looked up (Tail)
    std::atomic<uint64_t> dirsWalked{0};    // a session's subagents folder listed
    std::atomic<uint64_t> extraParses{0};   // lines a Load parsed for their time too (json::SkimTop could not say)
};

// ---- tail.mjs: a JSONL file that only grows -------------------------------------------------------------

// A remembered byte offset, and the bytes after the last newline kept until the line is complete.
class Tail {
public:
    enum class Got { Nothing, Lines, Truncated, Gone };

    Tail() = default;
    explicit Tail(std::wstring p) : path(std::move(p)) {}

    std::wstring path;
    uint64_t offset = 0;
    uint64_t size = 0;
    Ms mtime = 0;
    const std::atomic<bool>* cancel = nullptr;
    Counters* counters = nullptr;

    // First read: at most `max` bytes from the end; a cut first line is dropped. False when it cannot be read.
    template <class F>
    bool Open(F&& line, uint64_t max = kFirstReadMax) {
        FileStat st;
        if (!Stat(st)) return false;
        uint64_t from = st.size > max ? st.size - max : 0;
        rest_.clear();
        skipping_ = from > 0;
        uint64_t got = 0;
        if (!ReadRange(from, st.size, line, got)) return false;
        offset = from + got;
        size = st.size;
        mtime = st.mtime;
        Release();
        return true;
    }

    // New complete lines since the last read go to `line`; Truncated when the file got shorter.
    template <class F>
    Got Read(F&& line) {
        FileStat st;
        if (!Stat(st)) return Got::Gone;
        mtime = st.mtime;
        if (st.size < offset) return Got::Truncated;
        if (st.size == offset) return Got::Nothing;
        uint64_t got = 0;
        lines_ = 0;
        // A file that cannot be opened just now (locked, or going away) is tried again on the next change.
        if (!ReadRange(offset, st.size, line, got)) return Got::Nothing;
        offset += got;
        size = st.size;
        Release();
        return lines_ > 0 ? Got::Lines : Got::Nothing;
    }

    // Open, with the whole first read in `buf` (one buffer, at most `max` bytes): the lines go to `line` as views into
    // it, so a caller may keep their places instead of copies while `buf` lives.
    template <class F>
    bool OpenInto(std::string& buf, F&& line, uint64_t max) {
        buf.clear();
        FileStat st;
        if (!Stat(st)) return false;
        uint64_t from = st.size > max ? st.size - max : 0;
        rest_.clear();
        skipping_ = from > 0;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        if (counters) counters->filesOpened++;
        buf.resize(size_t(st.size - from));
        size_t got = 0;
        while (got < buf.size() && !Cancelled()) {
            DWORD want = DWORD(std::min<size_t>(kChunk, buf.size() - got));
            OVERLAPPED where{};
            where.Offset = DWORD(from + got);
            where.OffsetHigh = DWORD((from + got) >> 32);
            DWORD n = 0;
            if (!ReadFile(file, buf.data() + got, want, &n, &where) || n == 0) break;
            if (counters) counters->bytesRead += n;
            got += n;
        }
        CloseHandle(file);
        buf.resize(got);
        Feed(buf.data(), buf.size(), line);  // one Feed: no line is put together in rest_, so every view is into buf
        offset = from + got;
        size = st.size;
        mtime = st.mtime;
        Release();
        return true;
    }

private:
    std::string rest_;
    bool skipping_ = false;  // dropping bytes up to the next newline (a cut first line, or one too long)
    size_t lines_ = 0;

    bool Cancelled() const { return cancel != nullptr && cancel->load(std::memory_order_relaxed); }

    bool Stat(FileStat& st) const {
        if (counters) counters->filesStatted++;
        return StatFile(path, st);
    }

    void Release() {
        if (rest_.empty() && rest_.capacity() > 64 * 1024) std::string().swap(rest_);
    }

    template <class F>
    void Emit(std::string_view text, F& line) {
        std::string_view t = json::TrimLine(text);
        if (t.empty()) return;
        lines_++;
        line(t);
    }

    template <class F>
    void Feed(const char* p, size_t n, F& line) {
        size_t i = 0;
        while (i < n && !Cancelled()) {
            const char* nl = static_cast<const char*>(std::memchr(p + i, '\n', n - i));
            if (skipping_) {
                if (nl == nullptr) return;
                i = size_t(nl - p) + 1;
                skipping_ = false;
                continue;
            }
            if (nl == nullptr) {
                if (rest_.size() + (n - i) > kMaxLine) {
                    std::string().swap(rest_);
                    skipping_ = true;
                } else {
                    rest_.append(p + i, n - i);
                }
                return;
            }
            size_t len = size_t(nl - (p + i));
            if (!rest_.empty()) {
                if (rest_.size() + len <= kMaxLine) {
                    rest_.append(p + i, len);
                    Emit(rest_, line);
                }
                rest_.clear();
            } else if (len <= kMaxLine) {
                Emit(std::string_view(p + i, len), line);
            }
            i += len + 1;
        }
    }

    template <class F>
    bool ReadRange(uint64_t from, uint64_t to, F& line, uint64_t& got) {
        got = 0;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        if (counters) counters->filesOpened++;
        std::string buf;
        buf.resize(size_t(std::min<uint64_t>(kChunk, to > from ? to - from : 0)));
        uint64_t at = from;
        while (at < to && !Cancelled()) {
            DWORD want = DWORD(std::min<uint64_t>(buf.size(), to - at));
            OVERLAPPED where{};
            where.Offset = DWORD(at);
            where.OffsetHigh = DWORD(at >> 32);
            DWORD n = 0;
            if (!ReadFile(file, buf.data(), want, &n, &where) || n == 0) break;
            if (counters) counters->bytesRead += n;
            Feed(buf.data(), n, line);
            at += n;
            got += n;
        }
        CloseHandle(file);
        return true;
    }
};

// The prompt that began the turn still running at byte `end` of a transcript, looked for backward from there: the
// line (whole) of the last prompt, `tail` being the rest of the line that `end` cuts (the part at and after it). None
// when a turn ended after that prompt, when no prompt is found within `budget` bytes, or when the file cannot be read.
// Only lines that may move the turn are parsed: user lines without a tool result, and final answers.
inline std::optional<std::string> TurnPromptBefore(const std::wstring& path, uint64_t end, std::string_view tail,
                                                   uint64_t budget, Counters* counters,
                                                   const std::atomic<bool>* cancel) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    if (counters) counters->filesOpened++;
    json::Doc doc;
    auto has = [](std::string_view l, std::string_view what) { return l.find(what) != std::string_view::npos; };
    auto edge = [&](std::string_view l) {
        l = json::TrimLine(l);
        if (has(l, "\"type\":\"user\"")) {
            if (has(l, "\"type\":\"tool_result\"")) return board::TurnEdge::None;
        } else if (!has(l, "\"type\":\"assistant\"") ||
                   !(has(l, "\"stop_reason\":\"end_turn\"") || has(l, "\"stop_reason\":\"stop_sequence\"") ||
                     has(l, "\"stop_reason\":\"max_tokens\""))) {
            return board::TurnEdge::None;
        }
        if (!doc.Parse(l)) return board::TurnEdge::None;
        if (counters) counters->linesParsed++;
        return board::TurnEdgeOf(doc.Root());
    };
    std::optional<std::string> found;
    std::string carry(tail);  // the end part of a line whose start is not read yet
    bool dropping = carry.size() > kMaxLine;  // inside a line too long to keep (the Tail skips it too)
    if (dropping) carry.clear();
    std::string data;
    uint64_t at = end, scanned = 0;
    bool done = false;
    while (!done && at > 0 && scanned < budget && !(cancel && cancel->load(std::memory_order_relaxed))) {
        size_t n = size_t(std::min<uint64_t>(kChunk, at));
        data.resize(n + carry.size());
        OVERLAPPED where{};
        where.Offset = DWORD(at - n);
        where.OffsetHigh = DWORD((at - n) >> 32);
        DWORD got = 0;
        if (!ReadFile(file, data.data(), DWORD(n), &got, &where) || got != n) break;
        if (counters) counters->bytesRead += got;
        at -= n;
        scanned += n;
        size_t stop = n;  // lines are looked at in data[0, stop), from its end
        if (dropping) {
            size_t nl = std::string_view(data.data(), n).rfind('\n');
            if (nl == std::string_view::npos) continue;
            dropping = false;
            stop = nl;
        } else {
            std::memcpy(data.data() + n, carry.data(), carry.size());
            stop = data.size();
        }
        carry.clear();
        std::string_view view(data.data(), stop);
        while (true) {
            size_t nl = view.rfind('\n');
            if (nl == std::string_view::npos) {
                if (at == 0) {
                    board::TurnEdge e = edge(view);
                    if (e == board::TurnEdge::Start) found = std::string(json::TrimLine(view));
                } else if (view.size() > kMaxLine) {
                    dropping = true;
                } else {
                    carry.assign(view);
                }
                break;
            }
            board::TurnEdge e = edge(view.substr(nl + 1));
            if (e != board::TurnEdge::None) {
                if (e == board::TurnEdge::Start) found = std::string(json::TrimLine(view.substr(nl + 1)));
                done = true;
                break;
            }
            view = view.substr(0, nl);
        }
    }
    CloseHandle(file);
    return found;
}

// ---- window.mjs: the context window a model runs with -----------------------------------------------------

// PillTaskbar's rule (ClaudeWindow): the model's own window (1M for the current models, 200k for older ones,
// 0 for a model not Claude's), capped by settings.json `modelSettings.<model>.autoCompactWindow`, else
// `autoCompactWindow`.
inline double ModelWindow(const std::optional<std::string>& model) {
    if (!model || model->rfind("claude-", 0) != 0) return 0;
    for (const char* p : {"claude-fable-5", "claude-mythos-5", "claude-opus-5", "claude-sonnet-5", "claude-haiku-5",
                          "claude-opus-4-6", "claude-opus-4-7", "claude-opus-4-8", "claude-sonnet-4-6"})
        if (model->rfind(p, 0) == 0) return 1000000;
    return 200000;
}

// settings.json read again at most every 30 s (window.mjs makeWindowOf). Read only; never written.
class ContextWindows {
public:
    explicit ContextWindows(std::wstring settingsPath) : path_(std::move(settingsPath)) {}

    double operator()(const std::optional<std::string>& model) {
        Load();
        double own = ModelWindow(model);
        std::string key = model ? AsciiLower(*model) : std::string();
        double set = 0;
        for (auto const& [k, v] : byModel_)
            if (k == key) set = v;
        if (set == 0) set = all_;
        return set != 0 && own != 0 ? std::min(set, own) : set != 0 ? set : own;
    }

private:
    std::wstring path_;
    uint64_t readAt_ = 0;
    bool read_ = false;
    double all_ = 0;
    std::vector<std::pair<std::string, double>> byModel_;  // modelSettings, keys lower-cased as PillTaskbar compares

    static std::string AsciiLower(std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = char(c + 32);
        return s;
    }
    static double Num(json::Val v) {
        return v.IsNumber() && std::isfinite(v.Num()) && v.Num() > 0 ? v.Num() : 0;
    }
    void Load() {
        uint64_t now = GetTickCount64();
        if (read_ && now - readAt_ < 30000) return;
        read_ = true;
        readAt_ = now;
        all_ = 0;
        byModel_.clear();
        HANDLE f = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        std::string text;
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && uint64_t(size.QuadPart) <= kSettingsMax) {
            text.resize(size_t(size.QuadPart));
            DWORD n = 0;
            if (!ReadFile(f, text.data(), DWORD(text.size()), &n, nullptr)) n = 0;
            text.resize(n);
        }
        CloseHandle(f);
        json::Doc doc;
        if (!doc.Parse(json::TrimLine(text))) return;
        json::Val root = doc.Root();
        all_ = Num(root["autoCompactWindow"]);
        json::Val ms = root["modelSettings"];
        if (!ms.IsObject()) return;
        for (json::Val c = ms.First(); c.Exists(); c = c.Next())
            byModel_.emplace_back(AsciiLower(std::string(c.Key())), Num(c["autoCompactWindow"]));
    }
};

// ---- store.mjs ----------------------------------------------------------------------------------------

// /^agent-([\w-]{1,64})\.jsonl$/
inline std::optional<std::string> SubAgentId(std::wstring_view name) {
    if (!lotmap::StartsWith(name, L"agent-") || !lotmap::EndsWith(name, L".jsonl")) return std::nullopt;
    std::wstring_view id = name.substr(6, name.size() - 6 - 6);
    if (id.empty() || id.size() > 64) return std::nullopt;
    std::string out;
    for (wchar_t c : id) {
        if (!(lotmap::IsWordChar(c) || c == L'-')) return std::nullopt;
        out += char(c);
    }
    return out;
}

struct SubFile {
    std::wstring path;
    std::string agentId;
    Ms listed = 0;  // last write as the folder listing has it
};

struct Sub {
    std::wstring path;
    Tail tail;
    std::string agentId;
    Ms polledAt = 0;
};

struct Sess {
    board::Session state;
    std::wstring root;  // the projects folder it was found in
    std::wstring proj;
    Tail main;
    std::vector<std::unique_ptr<Sub>> subs;  // in the order they were found (Map insertion order)
    std::unordered_map<std::wstring, Sub*> subByPath;  // PathKey(path) -> its Sub
};

// A path as a key: the folder watch and the folder listing may not spell a name's case alike.
inline std::wstring PathKey(std::wstring p) {
    if (!p.empty()) CharLowerBuffW(p.data(), DWORD(p.size()));
    return p;
}

class Store {
public:
    Store(std::wstring root, Ms activeMs, board::WindowOf windowOf, const std::atomic<bool>* cancel = nullptr,
          Counters* counters = nullptr)
        : watchMs_(activeMs), windowOf_(std::move(windowOf)), cancel_(cancel), counters_(counters) {
        AddRoot(std::move(root));
        if (roots_.empty()) roots_.emplace_back();  // an empty root stays the one root, as before
    }

    // The projects folders read: the first is the one it was made with; AddRoot adds more (a folder already there,
    // spelled any way, is not added twice). Each has its own folder watch in the Reader, root index = watch index.
    const std::wstring& root(size_t i = 0) const { return roots_[i]; }
    size_t rootCount() const { return roots_.size(); }
    bool AddRoot(std::wstring root) {
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (root.empty() && !roots_.empty()) return false;
        std::wstring key = PathKey(root);
        for (auto const& r : roots_)
            if (PathKey(r) == key) return false;
        roots_.push_back(std::move(root));
        return true;
    }
    const std::vector<std::unique_ptr<Sess>>& sessions() const { return sessions_; }
    Sess* Find(std::string_view id) {
        for (auto& s : sessions_)
            if (s->state.id == id) return s.get();
        return nullptr;
    }

    // ---- discovery ---------------------------------------------------------------------------------

    // Finds main transcripts changed within the watch window and loads the ones not yet watched. `quietSubs` also
    // looks into quiet sessions' subagent folders (the prototype does on every rescan; here on the first scan, without
    // a folder watch and after it lost events, since the watch reports a subagent file the moment it is written).
    bool Scan(Ms now, bool quietSubs = true) {
        bool changed = false;
        for (auto it = subsCheckedAt_.begin(); it != subsCheckedAt_.end();)
            it = now - it->second >= kQuietSubPollMs ? subsCheckedAt_.erase(it) : std::next(it);
        for (auto const& root : roots_)
        for (auto const& dir : Dirs(root)) {
            if (Cancelled()) return changed;
            std::wstring folder = root + L"\\" + dir;
            WIN32_FIND_DATAW d;
            HANDLE find = FindFirstFileExW((folder + L"\\*.jsonl").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) continue;
            std::vector<std::pair<std::wstring, Ms>> files;
            do {
                std::wstring_view name = d.cFileName;
                if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !lotmap::EndsWith(name, L".jsonl")) continue;
                files.emplace_back(std::wstring(name), FileTimeMs(d.ftLastWriteTime));
            } while (FindNextFileW(find, &d));
            FindClose(find);
            for (auto const& [name, written] : files) {
                std::string id = lotmap::Narrow(std::wstring_view(name).substr(0, name.size() - 6));
                if (Find(id) != nullptr) continue;
                // A quiet main file can still belong to a working session: its subagents and workflow agents write
                // elsewhere.
                if (now - written > watchMs_ && !(quietSubs && SubsActive(root, dir, id, now))) continue;
                if (Load(root, dir, id, folder + L"\\" + name, now)) changed = true;
            }
        }
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            Ms la = (*it)->state.lastActivity;
            if (la < now - 4 * watchMs_ && now - (*it)->main.mtime > watchMs_) {
                it = sessions_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        return changed;
    }

    // Reads a session's main file and its subagents' files from their ends, in time order. Each file's first read is
    // one buffer (no copy a line), within kLoadBudget for them all, shared evenly among the recent subagents.
    bool Load(const std::wstring& root, const std::wstring& proj, const std::string& id, const std::wstring& file,
              Ms now) {
        auto sess = std::make_unique<Sess>();
        sess->state = board::CreateSession(id, windowOf_);
        sess->root = root;
        sess->proj = proj;
        sess->main = MakeTail(file);
        struct Span {
            uint32_t at, len;
        };
        struct Batch {
            std::optional<std::string> agentId;
            std::string buf;
            std::vector<Span> lines;
        };
        auto into = [](Batch& b) {
            return [&b](std::string_view l) { b.lines.push_back({uint32_t(l.data() - b.buf.data()), uint32_t(l.size())}); };
        };
        std::vector<Batch> batches(1);
        if (!sess->main.OpenInto(batches[0].buf, into(batches[0]), kFirstReadMax)) return false;
        uint64_t left = kLoadBudget > batches[0].buf.size() ? kLoadBudget - batches[0].buf.size() : 0;

        struct Found {
            SubFile file;
            FileStat st;
            uint64_t cap = 0;  // what its first read may take; 0: watched from its end
        };
        std::vector<Found> found;
        for (auto& sub : ListSubs(root, proj, id)) {
            FileStat st;
            if (StatFile(sub.path, st)) found.push_back({std::move(sub), st, 0});
        }
        std::vector<size_t> recent;
        for (size_t i = 0; i < found.size(); i++)
            if (now - found[i].st.mtime <= watchMs_) recent.push_back(i);
        // Each recent subagent gets an even share of what is left, at most kSubFirstReadMax, the smallest first so
        // what one does not need goes to the rest: a few read 2 MB each as the prototype does, and with many every
        // one is still read (the crew stays whole), each less far back.
        std::stable_sort(recent.begin(), recent.end(),
                         [&](size_t a, size_t b) { return found[a].st.size < found[b].st.size; });
        for (size_t k = 0; k < recent.size(); k++) {
            Found& f = found[recent[k]];
            uint64_t cap = std::min<uint64_t>(kSubFirstReadMax, left / (recent.size() - k));
            f.cap = cap;
            left -= std::min(cap, f.st.size);
        }
        for (auto& f : found) {
            auto s = std::make_unique<Sub>();
            s->path = f.file.path;
            s->tail = MakeTail(f.file.path);
            s->agentId = f.file.agentId;
            s->polledAt = now;
            Batch batch{f.file.agentId, {}, {}};
            if (f.cap > 0) {
                if (!s->tail.OpenInto(batch.buf, into(batch), f.cap)) continue;
            } else {
                // An old subagent (or one past the budget): watched from its end, its past left out.
                s->tail.offset = f.st.size;
                s->tail.size = f.st.size;
                s->tail.mtime = f.st.mtime;
            }
            sess->subByPath[PathKey(s->path)] = s.get();
            sess->subs.push_back(std::move(s));
            batches.push_back(std::move(batch));
        }

        // The prototype parses every line, sorts them stably by time (a line without one takes the time before it in
        // its file) and applies them. Here a line's time comes from a skim (json::SkimTop), so each line is parsed
        // once, when it is applied; only a line the skim cannot read plainly is parsed for its time as well.
        struct Item {
            uint32_t batch, line;
            Ms ts;
        };
        auto text = [&](size_t b, size_t l) {
            return std::string_view(batches[b].buf).substr(batches[b].lines[l].at, batches[b].lines[l].len);
        };
        std::vector<Item> items;
        json::Doc doc;
        for (size_t b = 0; b < batches.size(); b++) {
            Ms last = 0;
            for (size_t l = 0; l < batches[b].lines.size(); l++) {
                if (Cancelled()) return false;
                std::string_view line = text(b, l);
                json::Skim k = json::SkimTop(line, "timestamp");
                if (!k.whole) continue;  // the prototype's parse drops it
                std::string_view ts = k.value;
                bool has = k.has;
                if (!k.plain) {
                    if (counters_) counters_->extraParses++;
                    if (!doc.Parse(line)) continue;
                    json::Val v = doc.Root()["timestamp"];
                    has = v.IsString();
                    ts = v.Str();
                }
                if (has)
                    if (auto t = board::ParseTime(ts)) last = *t;
                items.push_back({uint32_t(b), uint32_t(l), last});
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.ts < b.ts; });
        // `prompt`: a line from before the main file's first read, applied first.
        auto apply = [&](const std::string* prompt) {
            sess->state = board::CreateSession(id, windowOf_);
            board::SetArrival(sess->state, std::nullopt);
            if (prompt != nullptr && doc.Parse(*prompt)) board::ApplyLine(sess->state, doc.Root(), std::nullopt);
            for (auto const& it : items) {
                if (Cancelled()) return false;
                if (!doc.Parse(text(it.batch, it.line))) continue;  // held together, but is not JSON
                if (counters_) counters_->linesParsed++;
                board::ApplyLine(sess->state, doc.Root(), batches[it.batch].agentId);
            }
            board::EndChunk(sess->state, std::nullopt);
            for (auto const& s : sess->subs) board::EndChunk(sess->state, s->agentId);
            return true;
        };
        if (!apply(nullptr)) return false;
        // A turn longer than the first read: its prompt is further back, so the board saw the turn run but not start.
        // That prompt is looked for (bounded) and the read applied again with it first; between it and the window
        // nothing is read, so only the turn's start comes from there.
        uint64_t from = sess->main.offset - batches[0].buf.size();
        const Board& b = sess->state.board;
        if (from > 0 && sess->state.turnOpen && !b.turnStartedAt && !b.turnEndedAt) {
            std::string_view head(batches[0].buf);
            head = head.substr(0, std::min(head.find('\n'), head.size()));
            if (auto prompt = TurnPromptBefore(sess->main.path, from, head, kTurnScanMax, counters_, cancel_))
                if (!apply(&*prompt)) return false;
        }
        sessions_.push_back(std::move(sess));
        return true;
    }

    // ---- reading what was added -------------------------------------------------------------------

    enum class Read { Nothing, Lines, Truncated, Gone };

    Read ReadTail(Sess& sess, Tail& tail, const std::optional<std::string>& agentId, Ms now) {
        json::Doc doc;
        size_t applied = 0;
        board::SetArrival(sess.state, now);
        Tail::Got got = tail.Read([&](std::string_view l) {
            if (!doc.Parse(l)) return;
            if (counters_) counters_->linesParsed++;
            applied++;
            board::ApplyLine(sess.state, doc.Root(), agentId);
        });
        if (applied > 0) board::EndChunk(sess.state, agentId);
        board::SetArrival(sess.state, std::nullopt);
        if (got == Tail::Got::Truncated) return Read::Truncated;
        if (got == Tail::Got::Gone) return Read::Gone;
        return applied > 0 ? Read::Lines : Read::Nothing;
    }

    // A subagent file not watched yet: read from its start (at most kSubFirstReadMax from its end) as new lines.
    // False when it cannot be read; `changed` is set when a line was applied.
    bool OpenSub(Sess& sess, const std::wstring& path, const std::string& agentId, Ms now, bool& changed) {
        auto s = std::make_unique<Sub>();
        s->path = path;
        s->tail = MakeTail(path);
        s->agentId = agentId;
        s->polledAt = now;
        json::Doc doc;
        size_t applied = 0;
        std::optional<std::string> agent = agentId;
        board::SetArrival(sess.state, now);
        bool ok = s->tail.Open(
            [&](std::string_view l) {
                if (!doc.Parse(l)) return;
                if (counters_) counters_->linesParsed++;
                applied++;
                board::ApplyLine(sess.state, doc.Root(), agent);
            },
            kSubFirstReadMax);
        if (ok && applied > 0) board::EndChunk(sess.state, agent);
        board::SetArrival(sess.state, std::nullopt);
        if (!ok) return false;
        sess.subByPath[PathKey(path)] = s.get();
        sess.subs.push_back(std::move(s));
        if (applied > 0) changed = true;
        return true;
    }

    // New subagent files of a watched session, by a walk of its subagents folder: the rescan's way, not the watch's.
    bool RescanSubs(Sess& sess, Ms now) {
        bool changed = false;
        for (auto const& sub : ListSubs(sess.root, sess.proj, sess.state.id)) {
            if (Cancelled()) return changed;
            if (sess.subByPath.count(PathKey(sub.path))) continue;
            OpenSub(sess, sub.path, sub.agentId, now, changed);
        }
        return changed;
    }

    // Reads what was added to sess.subs[i]: a file gone is let go, one that got shorter read again from its start.
    // False when the sub was let go (index i then names the next one).
    bool PollSub(Sess& sess, size_t i, Ms now, bool& changed) {
        Sub& sub = *sess.subs[i];
        sub.polledAt = now;
        std::optional<std::string> agent = sub.agentId;
        Read r = ReadTail(sess, sub.tail, agent, now);
        if (r == Read::Gone) {
            DropSub(sess, i);
            return false;
        }
        if (r == Read::Truncated) {
            sub.tail = MakeTail(sub.path);
            json::Doc doc;
            board::SetArrival(sess.state, now);
            bool ok = sub.tail.Open(
                [&](std::string_view l) {
                    if (!doc.Parse(l)) return;
                    board::ApplyLine(sess.state, doc.Root(), agent);
                },
                kSubFirstReadMax);
            if (ok) board::EndChunk(sess.state, agent);
            board::SetArrival(sess.state, std::nullopt);
            changed = true;
            if (!ok) {
                DropSub(sess, i);
                return false;
            }
        } else if (r == Read::Lines) {
            changed = true;
        }
        return true;
    }

    // One subagent file the folder watch saw change: that Sub read, or (an agent file not watched yet) opened. Nothing
    // else of the session is looked at.
    bool ReadSub(Sess& sess, const std::wstring& path, Ms now) {
        bool changed = false;
        auto known = sess.subByPath.find(PathKey(path));
        if (known != sess.subByPath.end()) {
            for (size_t i = 0; i < sess.subs.size(); i++) {
                if (sess.subs[i].get() != known->second) continue;
                PollSub(sess, i, now, changed);
                break;
            }
            return changed;
        }
        size_t slash = path.find_last_of(L"\\/");
        std::wstring_view name = slash == std::wstring::npos ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1);
        if (auto agentId = SubAgentId(name)) OpenSub(sess, path, *agentId, now, changed);
        return changed;
    }

    // A session read again: its main file and, unless `mainOnly`, its subagents (`all`: the quiet ones too).
    bool PollSession(const std::string& id, bool all, Ms now, bool mainOnly = false) {
        Sess* sess = Find(id);
        if (sess == nullptr) return false;
        bool changed = false;
        Read main = ReadTail(*sess, sess->main, std::nullopt, now);
        if (main == Read::Truncated) {
            // Rewritten from the start: read again from scratch.
            std::wstring root = sess->root, proj = sess->proj, path = sess->main.path;
            Remove(id);
            Load(root, proj, id, path, now);
            return true;
        }
        if (main == Read::Gone) {
            Remove(id);
            return true;
        }
        if (main == Read::Lines) changed = true;
        if (mainOnly) return changed;
        for (size_t i = 0; i < sess->subs.size();) {
            if (Cancelled()) return changed;
            Sub& sub = *sess->subs[i];
            bool quiet = now - sub.tail.mtime > watchMs_;
            if (!all && quiet && now - sub.polledAt < kQuietSubPollMs) {
                i++;
                continue;
            }
            if (PollSub(*sess, i, now, changed)) i++;
        }
        return changed;
    }

    // Every watched session read again (the prototype's 1 s poll; here only without a folder watch or after the
    // watch lost events). `all` also reads quiet subagents.
    bool PollAll(Ms now, bool all = false) {
        bool changed = false;
        std::vector<std::string> ids;
        for (auto const& s : sessions_) ids.push_back(s->state.id);
        for (auto const& id : ids)
            if (PollSession(id, all, now)) changed = true;
        return changed;
    }

    bool Rescan(Ms now, bool quietSubs = true) {
        bool changed = Scan(now, quietSubs);
        for (auto const& s : sessions_)
            if (RescanSubs(*s, now)) changed = true;
        return changed;
    }

    // ---- watching -----------------------------------------------------------------------------------

    // A changed path under a root (by its index), from that root's folder watch: the session it belongs to is read soon.
    void Touched(std::wstring_view relative, size_t rootIndex = 0) {
        if (rootIndex >= roots_.size()) return;
        if (!lotmap::EndsWith(relative, L".jsonl")) return;
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (size_t i = 0; i <= relative.size(); i++) {
            if (i == relative.size() || relative[i] == L'\\' || relative[i] == L'/') {
                parts.emplace_back(relative.substr(start, i - start));
                while (i + 1 < relative.size() && (relative[i + 1] == L'\\' || relative[i + 1] == L'/')) i++;
                start = i + 1;
            }
        }
        if (parts.size() < 2) return;
        Pending p;
        if (parts.size() == 2) {
            p = {false, rootIndex, parts[0], lotmap::Narrow(std::wstring_view(parts[1]).substr(0, parts[1].size() - 6)), {}};
        } else if (parts.size() >= 4 && parts.size() - 4 <= size_t(kSubDepth) && parts[2] == L"subagents" &&
                   SubAgentId(parts.back())) {
            // The file itself is kept, so only it is read, at the depth the folder walk goes to.
            std::wstring path = roots_[rootIndex];
            for (auto const& part : parts) path += L"\\" + part;
            p = {true, rootIndex, parts[0], lotmap::Narrow(parts[1]), std::move(path)};
        } else {
            return;
        }
        for (auto const& q : pending_)
            if (q.sub == p.sub && q.root == p.root && q.proj == p.proj && q.id == p.id && q.path == p.path) return;
        pending_.push_back(std::move(p));
    }
    bool HasPending() const { return !pending_.empty(); }

    bool FlushPending(Ms now) {
        bool changed = false;
        std::vector<Pending> pending;
        pending.swap(pending_);
        for (auto const& p : pending) {
            if (Cancelled()) return changed;
            Sess* sess = Find(p.id);
            if (sess == nullptr) {
                // A main file written now, or a subagent file of a session whose main file is quiet: load the session.
                if (p.root >= roots_.size()) continue;
                const std::wstring& root = roots_[p.root];
                std::wstring file = root + L"\\" + p.proj + L"\\" + lotmap::Widen(p.id) + L".jsonl";
                FileStat st;
                if (p.sub || (StatFile(file, st) && now - st.mtime <= watchMs_))
                    if (Load(root, p.proj, p.id, file, now)) changed = true;
                continue;
            }
            // The watch names every file that changed, so one file's write costs that file; the walks and
            // the polls of every subagent are for the rescan, after the watch lost events, and without a watch.
            if (p.sub ? ReadSub(*sess, p.path, now) : PollSession(p.id, false, now, true)) changed = true;
        }
        return changed;
    }

    // ---- the State --------------------------------------------------------------------------------

    // Finds and keeps sessions as far back as `ms`; a wider window scans at once.
    bool SetWatch(Ms ms, Ms now) {
        if (ms == watchMs_) return false;
        bool grew = ms > watchMs_;
        watchMs_ = ms;
        if (!grew) return false;
        subsCheckedAt_.clear();
        return Rescan(now);
    }

    static bool HasOpenTask(const Sess& sess) {
        for (auto const& e : sess.state.board.entries)
            if (e.status == Status::Run && !e.task.empty()) return true;
        return false;
    }
    static bool IsActive(const Sess& sess, Ms now, Ms activeMs) {
        Ms la = sess.state.lastActivity;
        if (la >= now - activeMs) return true;
        return la >= now - 4 * activeMs && HasOpenTask(sess);
    }
    // The next moment a session may leave the active set by time alone (0: none).
    Ms NextExpiry(Ms now, Ms activeMs) const {
        Ms next = 0;
        auto take = [&](Ms t) {
            if (t > now && (next == 0 || t < next)) next = t;
        };
        for (auto const& s : sessions_) {
            take(s->state.lastActivity + activeMs + 1);
            if (HasOpenTask(*s)) take(s->state.lastActivity + 4 * activeMs + 1);
        }
        return next;
    }

    // The active sessions, most recent first, at most kMaxSessions (prototype stateAt without the drawing).
    std::vector<Sess*> Active(Ms now, Ms activeMs) {
        std::vector<Sess*> out;
        for (auto& s : sessions_)
            if (IsActive(*s, now, activeMs)) out.push_back(s.get());
        std::stable_sort(out.begin(), out.end(),
                         [](Sess* a, Sess* b) { return a->state.lastActivity > b->state.lastActivity; });
        if (out.size() > size_t(kMaxSessions)) out.resize(size_t(kMaxSessions));
        return out;
    }
    std::vector<SessionView> StateAt(Ms now, Ms activeMs) {
        std::vector<SessionView> views;
        for (Sess* s : Active(now, activeMs)) views.push_back(board::ViewOf(s->state));
        return views;
    }

    std::vector<SubFile> ListSubs(const std::wstring& root, const std::wstring& proj, const std::string& id) const {
        if (counters_) counters_->dirsWalked++;
        std::vector<SubFile> out;
        Walk(root + L"\\" + proj + L"\\" + lotmap::Widen(id) + L"\\subagents", 0, out);
        return out;
    }

private:
    struct Pending {
        bool sub = false;
        size_t root = 0;  // index into roots_
        std::wstring proj;
        std::string id;
        std::wstring path;  // a subagent file's own path
    };

    std::vector<std::wstring> roots_;
    Ms watchMs_;
    board::WindowOf windowOf_;
    const std::atomic<bool>* cancel_;
    Counters* counters_;
    std::vector<std::unique_ptr<Sess>> sessions_;
    std::vector<Pending> pending_;
    std::unordered_map<std::string, Ms> subsCheckedAt_;  // session id -> when its quiet main file's subagents were looked at

    bool Cancelled() const { return cancel_ != nullptr && cancel_->load(std::memory_order_relaxed); }

    Tail MakeTail(const std::wstring& path) const {
        Tail t(path);
        t.cancel = cancel_;
        t.counters = counters_;
        return t;
    }

    static void DropSub(Sess& sess, size_t i) {
        sess.subByPath.erase(PathKey(sess.subs[i]->path));
        sess.subs.erase(sess.subs.begin() + std::ptrdiff_t(i));
    }

    void Remove(const std::string& id) {
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if ((*it)->state.id == id) {
                sessions_.erase(it);
                return;
            }
        }
    }

    static std::vector<std::wstring> Dirs(const std::wstring& dir) {
        std::vector<std::wstring> out;
        WIN32_FIND_DATAW d;
        HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchLimitToDirectories,
                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return out;
        do {
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                wcscmp(d.cFileName, L".") != 0 && wcscmp(d.cFileName, L"..") != 0)
                out.emplace_back(d.cFileName);
        } while (FindNextFileW(find, &d));
        FindClose(find);
        return out;
    }

    static void Walk(const std::wstring& dir, int depth, std::vector<SubFile>& out) {
        WIN32_FIND_DATAW d;
        HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> dirs;
        std::vector<std::pair<std::wstring, std::string>> files;
        // Entries in listing order, a folder walked where it is listed (readdirSync + recursion).
        struct Item {
            bool isDir;
            std::wstring name;
            Ms written;
        };
        std::vector<Item> order;
        do {
            if (wcscmp(d.cFileName, L".") == 0 || wcscmp(d.cFileName, L"..") == 0) continue;
            bool isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir && (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            order.push_back({isDir, d.cFileName, FileTimeMs(d.ftLastWriteTime)});
        } while (FindNextFileW(find, &d));
        FindClose(find);
        for (auto const& [isDir, name, written] : order) {
            std::wstring p = dir + L"\\" + name;
            if (isDir) {
                if (depth < kSubDepth) Walk(p, depth + 1, out);
            } else if (auto id = SubAgentId(name)) {
                out.push_back({p, *id, written});
            }
        }
    }

    // Whether any of a not-watched session's subagent files changed within the watch window (looked at most
    // every 30 s).
    bool SubsActive(const std::wstring& root, const std::wstring& proj, const std::string& id, Ms now) {
        auto last = subsCheckedAt_.find(id);
        if (last != subsCheckedAt_.end() && now - last->second < kQuietSubPollMs) return false;
        subsCheckedAt_[id] = now;
        if (!IsDir(root + L"\\" + proj + L"\\" + lotmap::Widen(id) + L"\\subagents")) return false;
        for (auto const& sub : ListSubs(root, proj, id)) {
            if (now - sub.listed <= watchMs_) return true;
        }
        return false;
    }
};

// The folder watch: ReadDirectoryChangesW on the projects folder and everything under it, overlapped, so the
// worker waits on its event with the others.
class Watcher {
public:
    ~Watcher() { Close(); }

    bool Open(const std::wstring& root) {
        Close();
        dir_ = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir_ == INVALID_HANDLE_VALUE) return false;
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            Close();
            return false;
        }
        buf_.assign(64 * 1024 / sizeof(DWORD), 0);
        if (!Arm()) {
            Close();
            return false;
        }
        return true;
    }
    bool IsOpen() const { return dir_ != INVALID_HANDLE_VALUE; }
    HANDLE event() const { return event_; }

    enum class Got { Names, Overflow, Broken };
    // After the event fired: hands each changed relative path to `touched` and arms the next read.
    template <class F>
    Got Collect(F&& touched) {
        DWORD bytes = 0;
        pending_ = false;
        if (!GetOverlappedResult(dir_, &ov_, &bytes, FALSE)) {
            DWORD e = GetLastError();
            if (e == ERROR_NOTIFY_ENUM_DIR && Arm()) return Got::Overflow;
            Close();
            return Got::Broken;
        }
        Got got = Got::Names;
        if (bytes == 0) {
            got = Got::Overflow;  // more changed than the buffer held
        } else {
            const BYTE* p = reinterpret_cast<const BYTE*>(buf_.data());
            const BYTE* end = p + std::min<size_t>(bytes, buf_.size() * sizeof(DWORD));
            for (;;) {
                if (p + sizeof(FILE_NOTIFY_INFORMATION) > end) break;
                auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                size_t n = info->FileNameLength / sizeof(WCHAR);
                if (reinterpret_cast<const BYTE*>(info->FileName) + n * sizeof(WCHAR) > end) break;
                touched(std::wstring_view(info->FileName, n));
                if (info->NextEntryOffset == 0) break;
                p += info->NextEntryOffset;
            }
        }
        if (!Arm()) {
            Close();
            return got == Got::Names ? Got::Broken : got;
        }
        return got;
    }

    void Close() {
        if (dir_ != INVALID_HANDLE_VALUE) {
            if (pending_) {
                CancelIoEx(dir_, &ov_);
                DWORD b = 0;
                GetOverlappedResult(dir_, &ov_, &b, TRUE);
                pending_ = false;
            }
            CloseHandle(dir_);
            dir_ = INVALID_HANDLE_VALUE;
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        std::vector<DWORD>().swap(buf_);
    }

private:
    HANDLE dir_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED ov_{};
    std::vector<DWORD> buf_;  // DWORD-aligned, as ReadDirectoryChangesW wants
    bool pending_ = false;

    bool Arm() {
        ov_ = OVERLAPPED{};
        ResetEvent(event_);
        ov_.hEvent = event_;
        constexpr DWORD kFilter =
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
        if (!ReadDirectoryChangesW(dir_, buf_.data(), DWORD(buf_.size() * sizeof(DWORD)), TRUE, kFilter, nullptr, &ov_,
                                   nullptr))
            return false;
        pending_ = true;
        return true;
    }
};

inline std::wstring EnvVar(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring v(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got < n ? got : 0);
    return v;
}

// Every Claude Code projects folder to read: each CLAUDE_CONFIG_DIR entry (a comma list, as PillTaskbar's ClaudeRoots
// reads it), %USERPROFILE%\.claude, every %USERPROFILE%\.claude-* (programs that start Claude Code with a config
// folder of their own, T3 Code among them) and Bloom's %LOCALAPPDATA%\Bloom\accounts\claude-* folders, each without
// repeats. A folder counts when its projects folder exists; with none, the first candidate stays so the Reader still
// waits for it as it always did. The first entry is the primary one (its settings.json is read).
inline std::vector<std::wstring> DefaultRoots() {
    std::vector<std::wstring> all, out;
    auto add = [&](std::wstring dir) {
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        if (dir.empty()) return;
        std::wstring root = dir + L"\\projects";
        std::wstring key = PathKey(root);
        for (auto const& r : all)
            if (PathKey(r) == key) return;
        all.push_back(std::move(root));
    };
    std::wstring list = EnvVar(L"CLAUDE_CONFIG_DIR");
    for (size_t start = 0; start <= list.size();) {
        size_t comma = list.find(L',', start);
        if (comma == std::wstring::npos) comma = list.size();
        std::wstring dir = list.substr(start, comma - start);
        while (!dir.empty() && (dir.front() == L' ')) dir.erase(0, 1);
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/' || dir.back() == L' ')) dir.pop_back();
        if (dir.size() >= 3 && dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/')) add(dir);
        start = comma + 1;
    }
    std::wstring home = EnvVar(L"USERPROFILE");
    auto each = [&](const std::wstring& parent, const wchar_t* pattern) {
        WIN32_FIND_DATAW d;
        HANDLE f = FindFirstFileExW((parent + L"\\" + pattern).c_str(), FindExInfoBasic, &d,
                                    FindExSearchLimitToDirectories, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) add(parent + L"\\" + d.cFileName);
        } while (FindNextFileW(f, &d));
        FindClose(f);
    };
    if (!home.empty()) {
        add(home + L"\\.claude");
        each(home, L".claude-*");
    }
    std::wstring local = EnvVar(L"LOCALAPPDATA");
    if (!local.empty()) each(local + L"\\Bloom\\accounts", L"claude-*");
    for (auto const& r : all)
        if (IsDir(r)) out.push_back(r);
    if (out.empty() && !all.empty()) out.push_back(all.front());
    return out;
}

// The primary projects folder (the first of DefaultRoots).
inline std::wstring DefaultRoot() {
    std::vector<std::wstring> roots = DefaultRoots();
    return roots.empty() ? std::wstring() : roots.front();
}

// settings.json beside the projects folder (window.mjs: the projects folder's parent).
inline std::wstring SettingsPathFor(std::wstring root) {
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    size_t cut = root.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"settings.json" : root.substr(0, cut) + L"\\settings.json";
}

}  // namespace store

// ---- the Reader: one worker thread ----------------------------------------------------------------------

struct ReaderOptions {
    std::wstring root;      // one ...\.claude\projects; empty = every Claude config folder (store::DefaultRoots)
    int hideAfter = 15;     // minutes a quiet session stays (1..1440), prototype `--active` / `hideAfter`
    // Tests only: runs on the worker at the start of every pass (false) and while the store is built again after a
    // pass failed (true); what it throws is what the worker meets there.
    std::function<void(bool rebuilding)> fault = nullptr;
};

class Reader {
public:
    using Callback = std::function<void(std::shared_ptr<const Snapshot>)>;

    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader() { Stop(); }

    // onSnapshot runs on the reader thread and only hands the snapshot on (the caller posts it on). Should the
    // worker meet something it cannot get past, its last snapshot has `stopped` set and no sessions.
    bool Start(ReaderOptions o, Callback onSnapshot) {
        if (thread_ != nullptr) return false;
        options_ = std::move(o);
        followConfigs_ = options_.root.empty();
        if (followConfigs_) options_.root = store::DefaultRoot();
        hideAfter_.store(Clamp(options_.hideAfter));
        callback_ = std::move(onSnapshot);
        lastWord_ = std::make_shared<Snapshot>();  // made now, so saying it never needs memory
        lastWord_->stopped = true;
        stop_.store(false);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stopEvent_ == nullptr || wakeEvent_ == nullptr) {
            CloseEvents();
            return false;
        }
        thread_ = CreateThread(nullptr, 1024 * 1024, &Reader::ThreadMain, this, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread_ == nullptr) {
            CloseEvents();
            return false;
        }
        return true;
    }

    // Any thread; wakes, joins, and frees everything it held.
    void Stop() {
        if (thread_ == nullptr) return;
        stop_.store(true);
        SetEvent(stopEvent_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        CloseEvents();
        callback_ = nullptr;
        lastWord_ = nullptr;
    }

    static constexpr uint64_t kFlushMs = 20;           // store.mjs: touched -> read after 20 ms
    static constexpr uint64_t kThrottleMs = 125;       // server.mjs THROTTLE_MS: at most 8 snapshots a second
    // The slow rescan (the prototype rescans every 5 s). With a healthy folder watch it is only a safety net (review
    // L16); for a while after the watch lost events, and without one, it comes as often as before.
    static constexpr uint64_t kRescanMs = 5 * 60000;
    static constexpr uint64_t kRescanSoonMs = 30000;   // after the watch overflowed
    static constexpr uint64_t kRescanBlindMs = 5000;   // without it: store.mjs RESCAN_MS, plus a poll of every file
    static constexpr size_t kMaxRoots = 16;            // config folders read at once (one folder watch each)
    static constexpr uint64_t kRetryMs = 1000;        // after a failed pass: at once, then 1 s, 2 s, ... up to 30 s

private:
    ReaderOptions options_;
    bool followConfigs_ = false;  // no root given: every Claude config folder is read, new ones found on the rescan
    Callback callback_;
    std::shared_ptr<Snapshot> lastWord_;
    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<int> hideAfter_{15};
    store::Counters counters_;
    std::atomic<uint64_t> snapshots_{0}, passes_{0}, sessions_{0}, lastPassUs_{0}, maxPassUs_{0}, failures_{0},
        rescanEvery_{0};

    static int Clamp(int minutes) { return std::clamp(minutes, 1, 1440); }

    void CloseEvents() {
        if (stopEvent_ != nullptr) CloseHandle(stopEvent_);
        if (wakeEvent_ != nullptr) CloseHandle(wakeEvent_);
        stopEvent_ = wakeEvent_ = nullptr;
    }

    static DWORD WINAPI ThreadMain(void* p) {
        auto* self = static_cast<Reader*>(p);
        try {
            self->Run();
        } catch (...) {
            // Nothing crosses the thread's edge.
        }
        // Run returns by itself only when asked to stop; anything else is the end of reading, and the caller is told,
        // so it stops drawing a board that no longer moves.
        if (!self->stop_.load()) self->SendLastWord();
        return 0;
    }

    void SendLastWord() noexcept {
        try {
            if (!lastWord_) return;
            lastWord_->now = store::WallMs();
            snapshots_++;
            if (callback_) callback_(std::shared_ptr<const Snapshot>(std::move(lastWord_)));
        } catch (...) {
        }
    }

    struct Version {
        Board map;
        uint64_t version = 0;
    };

    void Run() {
        auto windows = std::make_shared<store::ContextWindows>(store::SettingsPathFor(options_.root));
        board::WindowOf windowOf = [windows](const std::optional<std::string>& m) { return (*windows)(m); };
        int hide = hideAfter_.load();
        // One store reads every projects folder; each has its own folder watch (watchers[i] is root i's).
        std::vector<std::wstring> roots;
        if (followConfigs_) roots = store::DefaultRoots();
        if (roots.empty()) roots.push_back(options_.root);
        if (roots.size() > kMaxRoots) roots.resize(kMaxRoots);
        auto makeStore = [&] {
            store::Store made(roots[0], Ms(hide) * 60000, windowOf, &stop_, &counters_);
            for (size_t i = 1; i < roots.size(); i++) made.AddRoot(roots[i]);
            return made;
        };
        store::Store st = makeStore();
        std::vector<std::unique_ptr<store::Watcher>> watchers;
        auto watchAll = [&] {  // opens every watch not open yet
            bool all = true;
            for (size_t i = 0; i < roots.size(); i++) {
                if (watchers.size() <= i) watchers.push_back(std::make_unique<store::Watcher>());
                if (!watchers[i]->IsOpen() && !watchers[i]->Open(roots[i])) all = false;
            }
            return all;
        };
        auto allWatched = [&] {
            for (auto const& w : watchers)
                if (!w->IsOpen()) return false;
            return watchers.size() == roots.size();
        };
        // A config folder that appeared since: read from now on. True when one was added.
        auto findRoots = [&] {
            if (!followConfigs_) return false;
            bool added = false;
            for (auto const& r : store::DefaultRoots()) {
                if (roots.size() >= kMaxRoots) break;
                if (st.AddRoot(r)) {
                    roots.push_back(st.root(st.rootCount() - 1));
                    added = true;
                }
            }
            return added;
        };

        std::vector<SessionView> sent;
        bool sentAny = false;
        std::unordered_map<std::string, Version> versions;
        uint64_t nextVersion = 1;

        uint64_t tick = GetTickCount64();
        uint64_t flushAt = 0, emitAt = 0, expiryAt = 0, rescanAt = 0, lastEmit = 0;
        bool dirty = true, overflow = false, lost = false;
        // After a pass failed: the store is empty until a full rescan fills it again.
        bool rebuilt = false;
        int failures = 0;  // in a row (forgotten after kRescanMs without one)
        uint64_t failedAt = 0;

        auto every = [&] {
            uint64_t ms = !allWatched() ? kRescanBlindMs : lost ? kRescanSoonMs : kRescanMs;
            rescanEvery_.store(ms);
            return ms;
        };
        // Whatever a pass threw (bad_alloc, a std::exception, anything): its store may be half built, so it is built
        // again empty and filled by a full rescan, at once the first time and then after 1 s, 2 s, ... up to 30 s,
        // while the watch's news is let go (the rescan finds it all). A line that throws on every read so costs a
        // rescan now and then, not one on every write.
        auto rebuild = [&] {
            failures_++;
            if (failures > 0 && tick - failedAt > kRescanMs) failures = 0;
            uint64_t wait = failures == 0 ? 0 : std::min<uint64_t>(kRetryMs << std::min(failures - 1, 15), kRescanSoonMs);
            failures++;
            failedAt = tick;
            if (options_.fault) options_.fault(true);
            st = makeStore();
            rebuilt = true;
            flushAt = 0;
            overflow = false;
            rescanAt = tick + wait;
        };
        auto pass = [&](auto&& work) {
            uint64_t t0 = Micros();
            bool changed = false;
            try {
                if (options_.fault) options_.fault(false);
                changed = work();
            } catch (...) {
                rebuild();
                // The first time the rescan comes before anything is sent; again and again, the empty store is sent
                // rather than a last board that stays.
                changed = failures > 1;
            }
            uint64_t us = Micros() - t0;
            passes_++;
            lastPassUs_.store(us);
            if (us > maxPassUs_.load()) maxPassUs_.store(us);
            sessions_.store(st.sessions().size());
            return changed;
        };

        watchAll();
        pass([&] { return st.Scan(store::WallMs()); });
        if (!rebuilt) rescanAt = GetTickCount64() + every();

        while (!stop_.load()) {
            tick = GetTickCount64();
            uint64_t deadline = rescanAt;
            for (uint64_t t : {flushAt, emitAt, expiryAt})
                if (t != 0 && t < deadline) deadline = t;
            if (dirty && emitAt == 0) deadline = tick;
            DWORD timeout = deadline <= tick ? 0 : DWORD(std::min<uint64_t>(deadline - tick, 0x7FFFFFFF));
            // stop, wake, then each open watch's event; owner[k] is the watch behind handles[2 + k].
            HANDLE handles[2 + kMaxRoots] = {stopEvent_, wakeEvent_};
            size_t owner[kMaxRoots];
            DWORD count = 2;
            for (size_t i = 0; i < watchers.size(); i++)
                if (watchers[i]->IsOpen()) {
                    owner[count - 2] = i;
                    handles[count++] = watchers[i]->event();
                }
            DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
            if (stop_.load() || r == WAIT_OBJECT_0) break;
            tick = GetTickCount64();
            try {
                if (r == WAIT_FAILED) {
                    for (auto& w : watchers) w->Close();  // a broken handle: go on without the watches
                    rescanAt = std::min(rescanAt, tick + kRescanBlindMs);
                    continue;
                }
                Ms wall = store::WallMs();

                if (r == WAIT_OBJECT_0 + 1) {
                    int h = hideAfter_.load();
                    if (h != hide) {
                        hide = h;
                        pass([&] { return st.SetWatch(Ms(hide) * 60000, wall); });
                        dirty = true;
                    }
                }
                if (r >= WAIT_OBJECT_0 + 2 && r < WAIT_OBJECT_0 + count) {
                    size_t which = owner[r - WAIT_OBJECT_0 - 2];
                    store::Watcher& watcher = *watchers[which];
                    bool hold = rebuilt && tick < rescanAt;
                    bool missed = false;
                    auto got = store::Watcher::Got::Broken;
                    try {
                        got = watcher.Collect([&](std::wstring_view rel) {
                            if (hold) return;
                            try {
                                st.Touched(rel, which);
                            } catch (...) {
                                missed = true;  // read it all instead
                            }
                        });
                    } catch (...) {
                        watcher.Close();  // left in a state not known: the rescan opens it again
                    }
                    if (got == store::Watcher::Got::Overflow || missed) {
                        overflow = lost = true;
                        rescanAt = std::min(rescanAt, tick + kRescanSoonMs);
                    }
                    if (!watcher.IsOpen()) rescanAt = std::min(rescanAt, tick + kRescanBlindMs);
                    if ((st.HasPending() || overflow) && flushAt == 0) flushAt = tick + kFlushMs;
                }
                if (flushAt != 0 && tick >= flushAt) {
                    flushAt = 0;
                    bool all = overflow;
                    overflow = false;
                    if (pass([&] {
                            bool c = all ? st.Rescan(wall) : false;
                            if (all && st.PollAll(wall, true)) c = true;
                            if (st.FlushPending(wall)) c = true;
                            return c;
                        }))
                        dirty = true;
                }
                if (tick >= rescanAt) {
                    bool blind = !allWatched();
                    bool fresh = findRoots();  // a new config folder: its sessions are looked into like a first scan's
                    if (blind || fresh) watchAll();  // a folder may exist now
                    bool full = blind || fresh || rebuilt;
                    rebuilt = false;
                    if (pass([&] {
                            // With the watch on, a quiet session's agents announce themselves; without it (or just
                            // after it came back, or after the store was built again) their folders are looked into,
                            // as the prototype always does.
                            bool c = st.Rescan(wall, full);
                            if (full && st.PollAll(wall)) c = true;
                            return c;
                        }))
                        dirty = true;
                    if (!rebuilt) {
                        lost = false;
                        rescanAt = GetTickCount64() + every();
                    }
                }
                if (expiryAt != 0 && tick >= expiryAt) {
                    expiryAt = 0;
                    dirty = true;
                }
                if (dirty && emitAt == 0) emitAt = std::max(tick, lastEmit == 0 ? tick : lastEmit + kThrottleMs);
                if (emitAt != 0 && tick >= emitAt) {
                    emitAt = 0;
                    dirty = false;
                    lastEmit = tick;
                    Emit(st, hide, sent, sentAny, versions, nextVersion, expiryAt);
                }
            } catch (...) {
                // Outside a pass (the emit, the watch's names): the same way back. If even that throws, the thread
                // ends and the caller hears it.
                rebuild();
                dirty = failures > 1;
            }
        }
    }

    void Emit(store::Store& st, int hide, std::vector<SessionView>& sent, bool& sentAny,
              std::unordered_map<std::string, Version>& versions, uint64_t& nextVersion, uint64_t& expiryAt) {
        Ms now = store::WallMs();
        Ms activeMs = Ms(hide) * 60000;
        std::vector<SessionView> views = st.StateAt(now, activeMs);
        std::unordered_map<std::string, Version> kept;
        for (auto& v : views) {
            auto old = versions.find(v.id);
            if (old != versions.end() && board::SameBoard(old->second.map, v.map)) {
                v.mapVersion = old->second.version;
            } else {
                v.mapVersion = nextVersion++;
            }
            kept[v.id] = Version{v.map, v.mapVersion};
        }
        versions.swap(kept);
        // Session by session in order, so a new order is a change (the ids differ) though lastActivity is not
        // compared.
        bool same = sentAny && sent.size() == views.size();
        for (size_t i = 0; same && i < views.size(); i++)
            same = sent[i].mapVersion == views[i].mapVersion && board::SameView(sent[i], views[i]);
        if (!same) {
            auto snap = std::make_shared<Snapshot>();
            snap->now = now;
            snap->sessions = views;
            sent = std::move(views);
            sentAny = true;
            snapshots_++;
            if (callback_) {
                try {
                    callback_(std::move(snap));
                } catch (...) {
                }
            }
        }
        Ms next = st.NextExpiry(now, activeMs);
        expiryAt = next == 0 ? 0 : GetTickCount64() + uint64_t(std::max<Ms>(1, next - store::WallMs()));
    }

    static uint64_t Micros() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        uint64_t q = uint64_t(c.QuadPart), hz = uint64_t(f.QuadPart);
        return q / hz * 1000000ull + q % hz * 1000000ull / hz;
    }
};

}  // namespace si
