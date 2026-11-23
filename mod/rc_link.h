// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: the line to Bloom. Bloom serves a named pipe (Bloom.run-card.<session>.<user SID>, this Windows user's
// only); the card holds one connection to it on a thread of its own, tries again every few seconds while Bloom is
// not there, and makes sure the other end is this user's Bloom.exe in this Windows session: the pipe's owner is this
// user (a pipe someone else made first under the same name is not), and its server runs in this session. Bloom passes on the permission requests and questions
// Claude Code asks through Bloom's hook, and takes the answers back; the card also asks Bloom to open a session.
// One UTF-8 JSON object per line (the protocol is in the plan, "Bloom 연결 (규약 v1)").
//
// The thread hands each line to the window as a message: (msg, kLine, new std::string) — the window deletes it —
// and (msg, kUp, 0) / (msg, kDown, 0) when the line comes up or goes. Sending is the window's, at any time.

#pragma once

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

namespace rc {

class Link {
public:
    enum : WPARAM { kLine = 0, kUp = 1, kDown = 2 };

    ~Link() { Stop(); }

    // The pipe's name: RUN_CARD_PIPE when set (the test program), else Bloom's.
    static std::wstring PipeName() {
        wchar_t env[256];
        DWORD n = GetEnvironmentVariableW(L"RUN_CARD_PIPE", env, ARRAYSIZE(env));
        if (n && n < ARRAYSIZE(env)) return L"\\\\.\\pipe\\" + std::wstring(env, n);
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        std::wstring sid;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::string buffer(size, '\0');
            LPWSTR text = nullptr;
            if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
                ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
                sid = text;
                LocalFree(text);
            }
            CloseHandle(token);
        }
        return L"\\\\.\\pipe\\Bloom.run-card." + std::to_wstring(session) + L"." + sid;
    }

    bool Start(HWND to, UINT msg) {
        to_ = to;
        msg_ = msg;
        name_ = PipeName();
        test_ = GetEnvironmentVariableW(L"RUN_CARD_PIPE", nullptr, 0) > 0;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) return false;
        thread_ = CreateThread(nullptr, 0, &Link::Main, this, 0, nullptr);
        return thread_ != nullptr;
    }

    void Stop() {
        if (stop_) SetEvent(stop_);
        if (thread_) {
            WaitForSingleObject(thread_, 3000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (stop_) CloseHandle(stop_);
        stop_ = nullptr;
    }

    bool Up() const { return up_; }
    DWORD BloomPid() const { return bloomPid_; }

    // Sends one line (a newline is added); false when there is no line to Bloom or the write did not go through.
    // On explorer's UI thread: waits at most kWriteMs (Bloom reads all the time, so a write normally goes at once).
    bool Send(const std::string& line) {
        std::lock_guard<std::mutex> hold(write_);
        HANDLE pipe = pipe_.load();
        if (!pipe) return false;
        std::string out = line + "\n";
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD wrote = 0;
        bool started = WriteFile(pipe, out.data(), DWORD(out.size()), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING;
        if (started && WaitForSingleObject(ov.hEvent, kWriteMs) != WAIT_OBJECT_0) CancelIoEx(pipe, &ov);
        // Waits for the write, or its cancelling, to finish either way: the system writes into `ov` until then.
        bool ok = started && GetOverlappedResult(pipe, &ov, &wrote, TRUE) && wrote == out.size();
        CloseHandle(ov.hEvent);
        if (!ok) {
            // Part of a line may have gone, and would run into the next: this connection ends (the thread connects
            // again).
            broken_ = true;
            CancelIoEx(pipe, nullptr);
        }
        return ok;
    }

private:
    static constexpr DWORD kRetryMs = 2000;
    static constexpr DWORD kWriteMs = 500;
    static constexpr size_t kMaxLine = 1 << 20;  // a request is a few KB; anything this long is not one

    HWND to_ = nullptr;
    UINT msg_ = 0;
    bool test_ = false;
    std::wstring name_;
    HANDLE stop_ = nullptr, thread_ = nullptr;
    std::atomic<HANDLE> pipe_{nullptr};
    std::atomic<bool> up_{false};
    std::atomic<bool> broken_{false};  // a write failed: the connection ends
    std::atomic<DWORD> bloomPid_{0};
    std::mutex write_;

    static DWORD WINAPI Main(void* self) {
        static_cast<Link*>(self)->Run();
        return 0;
    }

    // A piece of this process's token (TokenUser, TokenOwner) in a buffer of its own; empty when it can't be read.
    static std::vector<uint64_t> TokenPart(TOKEN_INFORMATION_CLASS what) {
        std::vector<uint64_t> out;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
        DWORD size = 0;
        GetTokenInformation(token, what, nullptr, 0, &size);
        out.resize((size + 7) / 8);
        if (!size || !GetTokenInformation(token, what, out.data(), size, &size)) out.clear();
        CloseHandle(token);
        return out;
    }

    // The pipe was made by this user: a pipe of the same name that another user made first would hand its requests
    // to, and take answers from, whoever made it. Bloom's pipe is owned by its token's owner: the user, or the
    // Administrators group for a full administrator token (explorer and Bloom then share it).
    static bool OwnedByMe(HANDLE pipe) {
        auto user = TokenPart(TokenUser), owns = TokenPart(TokenOwner);
        if (user.empty()) return false;
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
            ERROR_SUCCESS)
            return false;
        bool ok = owner && IsValidSid(owner) &&
                  (EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) ||
                   (!owns.empty() && EqualSid(owner, reinterpret_cast<TOKEN_OWNER*>(owns.data())->Owner)));
        LocalFree(sd);
        return ok;
    }

    static bool SameSession(DWORD pid) {
        DWORD mine = 0, theirs = 0;
        return ProcessIdToSessionId(GetCurrentProcessId(), &mine) && ProcessIdToSessionId(pid, &theirs) &&
               mine == theirs;
    }

    // The server is a Bloom.exe.
    static bool IsBloom(DWORD pid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) return false;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        bool ok = QueryFullProcessImageNameW(p, 0, path, &n);
        CloseHandle(p);
        if (!ok) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        return name && _wcsicmp(name + 1, L"Bloom.exe") == 0;
    }

    void Post(WPARAM what, std::string* line = nullptr) {
        if (!PostMessageW(to_, msg_, what, reinterpret_cast<LPARAM>(line))) delete line;
    }

    void Run() {
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            HANDLE pipe = CreateFileW(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            ULONG server = 0;
            // The test program's stand-in Bloom (RUN_CARD_PIPE) is not a Bloom.exe.
            if (pipe != INVALID_HANDLE_VALUE &&
                (!OwnedByMe(pipe) || !GetNamedPipeServerProcessId(pipe, &server) || !SameSession(server) ||
                 (!test_ && !IsBloom(server)))) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
            if (pipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(stop_, kRetryMs) != WAIT_TIMEOUT) return;
                continue;
            }
            bloomPid_ = server;
            broken_ = false;
            pipe_ = pipe;
            up_ = true;
            Post(kUp);
            Read(pipe);
            {
                std::lock_guard<std::mutex> hold(write_);
                pipe_ = nullptr;
            }
            up_ = false;
            CloseHandle(pipe);
            Post(kDown);
            if (WaitForSingleObject(stop_, kRetryMs) != WAIT_TIMEOUT) return;
        }
    }

    // Reads lines until the pipe breaks or the card stops.
    void Read(HANDLE pipe) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return;
        std::string pending;
        char buffer[4096];
        while (!broken_) {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            HANDLE wait[2] = {stop_, ov.hEvent};
            DWORD r = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &got, TRUE);
                break;
            }
            if (!GetOverlappedResult(pipe, &ov, &got, FALSE) || got == 0) break;
            pending.append(buffer, got);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) Post(kLine, new std::string(std::move(line)));
            }
            if (pending.size() > kMaxLine) break;
        }
        CloseHandle(ov.hEvent);
    }
};

// A JSON string literal for UTF-8 text.
inline std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char u[8];
                    snprintf(u, sizeof(u), "\\u%04x", c);
                    out += u;
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace rc
