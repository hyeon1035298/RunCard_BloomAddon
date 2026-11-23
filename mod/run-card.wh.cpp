// ==WindhawkMod==
// @id              run-card
// @name            Run Card
// @description     A live card for each running Claude Code session: what it does now, its steps, and its questions to answer
// @version         0.1.0
// @author          Hyeon
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lgdi32 -ld2d1 -ld3d11 -ldwrite -ldwmapi -ldxgi -lole32 -loleaut32 -lruntimeobject -lshell32 -lshlwapi -luuid -lcoremessaging -lshcore -ladvapi32
// ==/WindhawkMod==

// SPDX-License-Identifier: GPL-3.0-only
// Source code is published under The GNU General Public License v3.0.
// Third-party sources and licenses: THIRD_PARTY.md in the repository root.

// ==WindhawkModReadme==
/*
# Run Card

A card on a corner or edge of a monitor for each Claude Code session that is working: what it does now and
for how long, the model, a bar of its steps and the last few of them. At rest a card is a pill; point at it and it
opens. When a session asks for a permission or puts a question, Bloom passes it on and the card shows it with
Allow and Deny or the choices to pick; when the turn is done the card says how long it took and how many files
changed, then goes. Several sessions stack from the chosen place.

Nothing is installed into Claude Code: the cards read the transcripts Claude Code writes (Session island's reader)
and the status files beside them, and use no tokens. At rest nothing moves or redraws.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- card:
  - monitor: primary
    $name: 모니터
    $description: 카드를 띄울 모니터. 번호는 Windows 디스플레이 설정의 번호입니다
    $options:
    - primary: 주 모니터
    - display1: 1번
    - display2: 2번
    - display3: 3번
    - display4: 4번
  - position: topRight
    $name: 자리
    $description: 카드가 놓이는 곳. 여러 개면 이 자리에서부터 차례로 쌓입니다
    $options:
    - topLeft: 왼쪽 위
    - topCenter: 위 가운데
    - topRight: 오른쪽 위
    - middleLeft: 왼쪽 가운데
    - middleRight: 오른쪽 가운데
    - bottomLeft: 왼쪽 아래
    - bottomCenter: 아래 가운데
    - bottomRight: 오른쪽 아래
  - margin: 16
    $name: 가장자리 여백
    $description: 화면 가장자리와 카드 사이 (0~200)
  - expand: hover
    $name: 펼치기
    $description: 평소에는 한 줄 알약이고 포인터를 올리면 카드로 펼칩니다. 승인·질문과 완료는 저절로 펼칩니다
    $options:
    - hover: 포인터를 올리면
    - always: 늘 펼침
  - doneSeconds: 20
    $name: 완료 카드 남기기(초)
    $description: 턴이 끝난 뒤 카드가 이만큼 남았다가 사라집니다 (0~600). 포인터를 올리고 있으면 기다립니다
  - approvals: true
    $name: 카드에서 승인·질문 답하기
    $description: Bloom의 '알림 카드에서 승인'이 받은 요청을 이 카드에서 답합니다
  $name: 카드
- pill:
  - theme: system
    $name: 테마
    $description: 카드의 라이트·다크. Bloom 애드온으로 쓰면 Bloom의 테마를 따릅니다
    $options:
    - system: Windows 작업표시줄 테마
    - dark: 다크
    - light: 라이트
  $name: 모양
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dispatcherqueue.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#include "si/si_store.h"
#include "rc_draw.h"
#include "rc_link.h"
#include "rc_model.h"
#include "rc_place.h"

namespace wfn = winrt::Windows::Foundation::Numerics;
namespace wdj = winrt::Windows::Data::Json;
namespace wuc = winrt::Windows::UI::Composition;
namespace wucd = winrt::Windows::UI::Composition::Desktop;
namespace wgd = winrt::Windows::Graphics::DirectX;
using winrt::check_hresult;
using winrt::com_ptr;

// ---- Composition interop, as PillTaskbar declares it (pill-taskbar.wh.cpp, namespace island) -------------

namespace interop {
#undef INTERFACE
#define INTERFACE ICompositorDesktopInterop
DECLARE_INTERFACE_IID_(ICompositorDesktopInterop, IUnknown, "29E691FA-4567-4DCA-B319-D0F207EB6807") {
    STDMETHOD(CreateDesktopWindowTarget)(HWND hwnd, BOOL isTopmost, void** result) PURE;
    STDMETHOD(EnsureOnThread)(DWORD threadId) PURE;
};

#undef INTERFACE
#define INTERFACE ICompositorInterop
DECLARE_INTERFACE_IID_(ICompositorInterop, IUnknown, "25297D5C-3AD4-4C9C-B5CF-E36A38512330") {
    STDMETHOD(CreateCompositionSurfaceForHandle)(HANDLE swapChain, void** result) PURE;
    STDMETHOD(CreateCompositionSurfaceForSwapChain)(IUnknown * swapChain, void** result) PURE;
    STDMETHOD(CreateGraphicsDevice)(IUnknown * renderingDevice, void** result) PURE;
};

#undef INTERFACE
#define INTERFACE ICompositionDrawingSurfaceInterop
DECLARE_INTERFACE_IID_(ICompositionDrawingSurfaceInterop, IUnknown, "FD04E6E3-FE0C-4C3C-AB19-A07601A576EE") {
    STDMETHOD(BeginDraw)(const RECT* updateRect, REFIID iid, void** updateObject, POINT* updateOffset) PURE;
    STDMETHOD(EndDraw)() PURE;
    STDMETHOD(Resize)(SIZE sizePixels) PURE;
    STDMETHOD(Scroll)(const RECT* scrollRect, const RECT* clipRect, int offsetX, int offsetY) PURE;
    STDMETHOD(ResumeDraw)() PURE;
    STDMETHOD(SuspendDraw)() PURE;
};
#undef INTERFACE
}  // namespace interop

template <>
inline constexpr winrt::guid winrt::impl::guid_v<interop::ICompositorDesktopInterop>{
    0x29E691FA, 0x4567, 0x4DCA, {0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07}};
template <>
inline constexpr winrt::guid winrt::impl::guid_v<interop::ICompositorInterop>{
    0x25297D5C, 0x3AD4, 0x4C9C, {0xB5, 0xCF, 0xE3, 0x6A, 0x38, 0x51, 0x23, 0x30}};
template <>
inline constexpr winrt::guid winrt::impl::guid_v<interop::ICompositionDrawingSurfaceInterop>{
    0xFD04E6E3, 0xFE0C, 0x4C3C, {0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE}};

namespace {

// ============================================================================
// Basics
// ============================================================================

bool g_korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;

std::wstring Env(PCWSTR name) {
    wchar_t buffer[1024];
    DWORD n = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    return n && n < ARRAYSIZE(buffer) ? std::wstring(buffer, n) : std::wstring();
}

// Only the test program (tools/run-host.cpp) sets this; explorer never does.
bool TestMode() {
    static bool test = !Env(L"RUN_CARD_TEST").empty();
    return test;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

HINSTANCE ModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ModuleInstance, &module);
    return module;
}

std::optional<std::wstring> ReadText(const std::wstring& path, DWORD maxBytes = 256 * 1024) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= maxBytes;
    if (ok) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
    }
    CloseHandle(file);
    if (!ok) return std::nullopt;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    return text;
}

FILETIME FileTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ? data.ftLastWriteTime : FILETIME{};
}

bool SameTime(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

bool IsDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Milliseconds since 1970 (UTC), the clock the transcripts' times are on.
double NowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    int64_t t = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return double(t - 116444736000000000LL) / 10000.0;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

// ============================================================================
// Settings, as Bloom sets them (values.json, read through the runtime)
// ============================================================================

enum class ThemeChoice { System, Dark, Light };

struct Config {
    int monitor = 0;  // Windows' display number; 0 the main monitor
    rc::Anchor anchor = rc::Anchor::TopRight;
    float margin = 16;
    bool always = false;  // card.expand: always (else on hover)
    int doneSeconds = 20;
    bool approvals = true;
    ThemeChoice theme = ThemeChoice::System;
};

std::wstring StringSetting(PCWSTR name) {
    PCWSTR value = Wh_GetStringSetting(name);
    std::wstring out = value ? value : L"";
    Wh_FreeStringSetting(value);
    return out;
}

Config ReadConfig() {
    Config c;
    std::wstring monitor = StringSetting(L"card.monitor");
    c.monitor = monitor.starts_with(L"display") ? std::clamp(_wtoi(monitor.c_str() + 7), 0, 16) : 0;
    c.anchor = rc::AnchorOf(StringSetting(L"card.position"));
    c.margin = float(std::clamp(Wh_GetIntSetting(L"card.margin"), 0, 200));
    c.always = StringSetting(L"card.expand") == L"always";
    c.doneSeconds = std::clamp(Wh_GetIntSetting(L"card.doneSeconds"), 0, 600);
    c.approvals = Wh_GetIntSetting(L"card.approvals") != 0;
    std::wstring theme = StringSetting(L"pill.theme");
    c.theme = theme == L"dark" ? ThemeChoice::Dark : theme == L"light" ? ThemeChoice::Light : ThemeChoice::System;
    return c;
}

// ============================================================================
// Bloom's accent (its settings.json: "accent", a preset, or "custom" with "accentCustom" #RRGGBB, the light
// theme's colour), with Bloom's own pairs, as the other add-ons read it (Session island ReadAccent).
// ============================================================================

struct AccentPair {
    uint32_t dark, light;
};

void ToHsl(uint32_t argb, double& h, double& s, double& l) {
    double r = BYTE(argb >> 16) / 255.0, g = BYTE(argb >> 8) / 255.0, b = BYTE(argb) / 255.0;
    double hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    l = (hi + lo) / 2;
    s = d == 0 ? 0 : d / (1 - std::fabs(2 * l - 1));
    h = d == 0 ? 0 : hi == r ? 60 * std::fmod((g - b) / d + 6, 6) : hi == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
}

uint32_t FromHsl(double h, double s, double l) {
    double c = (1 - std::fabs(2 * l - 1)) * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2) - 1)), m = l - c / 2;
    double r = 0, g = 0, b = 0;
    int k = int(h / 60) % 6;
    if (k == 0) r = c, g = x;
    else if (k == 1) r = x, g = c;
    else if (k == 2) g = c, b = x;
    else if (k == 3) g = x, b = c;
    else if (k == 4) r = x, b = c;
    else r = c, b = x;
    auto byte = [&](double v) { return uint32_t(std::lround(std::clamp(v + m, 0.0, 1.0) * 255)); };
    return 0xFF000000u | byte(r) << 16 | byte(g) << 8 | byte(b);
}

std::optional<AccentPair> BloomAccent(const std::wstring& key, const std::wstring& custom) {
    struct Preset {
        PCWSTR key;
        AccentPair pair;
    };
    static constexpr Preset kPresets[] = {
        {L"rose", {0xFFFF7A9E, 0xFFE04E78}},      {L"lavender", {0xFFB49CFF, 0xFF7B5BDB}},
        {L"sky", {0xFF72B6FF, 0xFF2F72D6}},       {L"mint", {0xFF5ED3AE, 0xFF168F6C}},
        {L"tangerine", {0xFFFF9B5E, 0xFFD9652A}}, {L"lemon", {0xFFEDC94F, 0xFFA87C00}},
        {L"gray", {0xFFE4E4E4, 0xFF2E2E2E}},
    };
    if (key == L"custom") {
        std::wstring t = custom;
        if (!t.empty() && t[0] == L'#') t.erase(0, 1);
        if (t.size() != 6 || t.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return std::nullopt;
        uint32_t light = 0xFF000000u | uint32_t(wcstoul(t.c_str(), nullptr, 16));
        double h, s, l;
        ToHsl(light, h, s, l);
        return AccentPair{FromHsl(h, s, std::max(l, std::min(0.86, l + 0.18))), light};
    }
    for (auto& p : kPresets)
        if (key == p.key) return p.pair;
    return std::nullopt;
}

std::wstring BloomSettingsPath() {
    return KnownFolder(FOLDERID_RoamingAppData) + L"\\Bloom\\settings.json";
}

AccentPair ReadAccent() {
    if (auto text = ReadText(BloomSettingsPath(), 4 * 1024 * 1024)) {
        wdj::JsonObject o{nullptr};
        if (wdj::JsonObject::TryParse(*text, o)) {
            auto str = [&](PCWSTR name, PCWSTR fallback) {
                return o.HasKey(name) && o.GetNamedValue(name).ValueType() == wdj::JsonValueType::String
                           ? std::wstring(o.GetNamedString(name))
                           : std::wstring(fallback);
            };
            return BloomAccent(str(L"accent", L"rose"), str(L"accentCustom", L"")).value_or(AccentPair{0xFFFF7A9E, 0xFFE04E78});
        }
    }
    return {0xFFFF7A9E, 0xFFE04E78};
}

// The taskbar's own theme, for pill.theme = system.
bool SystemLight() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

winrt::Windows::UI::Color WinColor(uint32_t argb) {
    return {BYTE(argb >> 24), BYTE(argb >> 16), BYTE(argb >> 8), BYTE(argb)};
}

// ============================================================================
// Claude Code's folders: each settings folder with transcripts (projects\) and status files (sessions\)
// ============================================================================
// ~/.claude, any ~/.claude-<name> (programs that start Claude Code with a CLAUDE_CONFIG_DIR of their own), and
// Bloom's account folders. RUN_CARD_ROOT (the test program) names one projects folder instead.

std::vector<std::wstring> ConfigFolders() {
    std::vector<std::wstring> out;
    std::wstring test = Env(L"RUN_CARD_ROOT");
    if (!test.empty()) return {test};
    std::wstring home = KnownFolder(FOLDERID_Profile);
    auto add = [&](const std::wstring& dir) {
        if (IsDir(dir + L"\\projects") && std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(dir);
    };
    add(home + L"\\.claude");
    auto each = [&](const std::wstring& pattern, const std::wstring& parent) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) add(parent + L"\\" + fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    };
    each(home + L"\\.claude-*", home);
    std::wstring accounts = KnownFolder(FOLDERID_LocalAppData) + L"\\Bloom\\accounts";
    each(accounts + L"\\claude-*", accounts);
    return out;
}

// What <config>\sessions\<pid>.json says of a running Claude Code: written by Claude Code itself.
struct Status {
    std::string session;
    DWORD pid = 0;
    bool interactive = true;
    bool bloomRun = false;  // one of Bloom's own runs (its AI chats, its usage looks): never a card, as in Bloom's notices
    bool waiting = false;  // its status: it waits for the person (a permission, a question)
    uint64_t procStart = 0;
    FILETIME seen{};
    bool alive = true;
};

// The folder Bloom's own Claude Code runs work in (Bloom's OneShot.WorkDir), full and without a trailing slash.
const std::wstring& BloomWorkDir() {
    static const std::wstring dir = [] {
        std::wstring d = Env(L"LOCALAPPDATA");
        return d.empty() ? d : d + L"\\Bloom\\ai";
    }();
    return dir;
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    auto full = [](const std::wstring& p) {
        wchar_t out[MAX_PATH * 2];
        DWORD n = GetFullPathNameW(p.c_str(), ARRAYSIZE(out), out, nullptr);
        std::wstring f = n && n < ARRAYSIZE(out) ? std::wstring(out, n) : p;
        while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
        return f;
    };
    return !a.empty() && !b.empty() && _wcsicmp(full(a).c_str(), full(b).c_str()) == 0;
}

// Bloom's ClaudeWatch reads the same status words.
bool WaitingWord(const std::wstring& s) {
    for (PCWSTR w : {L"wait", L"permission", L"approval", L"input", L"blocked"})
        if (s.find(w) != std::wstring::npos) return true;
    return false;
}

// The process is the one that wrote the file (its start time matches; a reused pid does not).
bool StillRunning(DWORD pid, uint64_t procStart) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    if (alive && procStart && GetProcessTimes(p, &created, &exited, &kernel, &user)) {
        uint64_t c = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        alive = (c > procStart ? c - procStart : procStart - c) < 20000000ull;  // 2 s
    }
    CloseHandle(p);
    return alive;
}

std::optional<Status> ReadStatus(const std::wstring& path) {
    auto text = ReadText(path, 64 * 1024);
    wdj::JsonObject o{nullptr};
    if (!text || !wdj::JsonObject::TryParse(*text, o)) return std::nullopt;
    Status s;
    try {
        s.session = winrt::to_string(o.GetNamedString(L"sessionId", L""));
        s.pid = DWORD(o.GetNamedNumber(L"pid", 0));
        std::wstring kind(o.GetNamedString(L"kind", L"interactive"));
        s.interactive = kind == L"interactive";
        s.bloomRun = SameFolder(std::wstring(o.GetNamedString(L"cwd", L"")), BloomWorkDir());
        std::wstring status(o.GetNamedString(L"status", L""));
        for (auto& ch : status) ch = towlower(ch);
        s.waiting = WaitingWord(status);
        if (o.HasKey(L"procStart")) {
            auto v = o.GetNamedValue(L"procStart");
            s.procStart = v.ValueType() == wdj::JsonValueType::String ? _wcstoui64(v.GetString().c_str(), nullptr, 10)
                          : v.ValueType() == wdj::JsonValueType::Number ? uint64_t(v.GetNumber())
                                                                         : 0;
        }
    } catch (...) {
        return std::nullopt;
    }
    if (s.session.empty() || !s.pid) return std::nullopt;
    return s;
}

// ============================================================================
// Opening a session's window when Bloom is not there to do it: the first ancestor of the Claude Code process
// with a window of its own (a terminal, an editor).
// ============================================================================

HWND MainWindowOf(DWORD pid) {
    struct Find {
        DWORD pid;
        HWND found;
    } f{pid, nullptr};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD owner = 0;
            GetWindowThreadProcessId(w, &owner);
            if (owner != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            f->found = w;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

void RaiseSession(DWORD pid) {
    std::map<DWORD, DWORD> parent;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            parent[pe.th32ProcessID] = pe.th32ParentProcessID;
        CloseHandle(snap);
    }
    for (int hop = 0; pid && hop < 12; hop++) {
        if (HWND w = MainWindowOf(pid)) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
            return;
        }
        auto it = parent.find(pid);
        pid = it == parent.end() || it->second == pid ? 0 : it->second;
    }
}

// ============================================================================
// The cards
// ============================================================================

constexpr PCWSTR kClass = L"RunCardWindow_" WH_MOD_ID;
constexpr UINT kSnapshotMsg = WM_APP + 1;  // wParam: the folder's index, lParam: a boxed snapshot
constexpr UINT kSettingsMsg = WM_APP + 2;
constexpr UINT kLinkMsg = WM_APP + 3;      // rc::Link's messages
constexpr UINT_PTR kTickTimer = 1, kLeaveTimer = 2, kRegionTimer = 3, kMorphTimer = 4;
constexpr UINT kTickMs = 1000;    // the clocks move by the second
constexpr int kStatusEvery = 2;   // status files are looked at every second tick
constexpr UINT kLeaveMs = 300;    // a card the pointer left folds this much later
constexpr int kMorphMs = 220;     // a card opening or folding, the stack moving
constexpr int kFadeMs = 160;      // a card coming or going
constexpr double kOrphanMs = 180000;  // an open turn with no session file and no line for this long has ended
constexpr float kGap = 8;         // between stacked cards
constexpr double kRingTurnMs = 1000;
constexpr double kFrameMs = 1000.0 / 60;  // the ring turns at most 60 times a second
constexpr int kMaxCards = 6;
constexpr int kRescanTicks = 300;  // the Claude folders are looked for again every five minutes
constexpr float kEase[4] = {0.1f, 0.9f, 0.2f, 1.0f};  // PillTaskbar kIslandEase, "Fluent decelerate"

std::atomic<HWND> g_window{};
HANDLE g_stop = nullptr;

class App {
public:
    bool Start();
    void Stop();
    HANDLE FrameTimer() const { return frameTimer_; }
    void OnFrame();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Folder {
        std::wstring dir;  // the settings folder (projects\ and sessions\ under it)
        std::unique_ptr<si::Reader> reader;
        std::shared_ptr<const si::Snapshot> snap;
        std::map<std::wstring, Status> status;  // by file name
    };
    struct Slot {
        std::string id;
        uint64_t order = 0;  // when it came: the stack's order
        rc::Card card;
        DWORD pid = 0;
        bool wantOpen = false, open = false;  // a card (else a pill), as wanted and as drawn
        bool hover = false;
        bool gone = false;  // fading out
        rc::View drawn = rc::View::Pill;
        rc::Pointer ptr;
        rc::Laid laid;
        rc::Box box;          // where it is going, DIPs in the window
        rc::Size clipTo;      // the size its clip shows or is animating to
        bool placed = false;  // has been put somewhere once
        std::string key;      // what the picture was drawn from
        double foldAt = 0;    // a folding card is drawn as a pill from then on
        wuc::ContainerVisual root{nullptr};
        wuc::SpriteVisual sprite{nullptr};
        wuc::CompositionDrawingSurface surf{nullptr};
        wuc::CompositionRoundedRectangleGeometry clip{nullptr};
        wuc::ShapeVisual ring{nullptr};
        wuc::CompositionEllipseGeometry ringGeom{nullptr};
        wuc::CompositionColorBrush ringBrush{nullptr};
        bool spinning = false;
    };

    Config cfg_;
    rc::Palette look_{};
    uint64_t lookId_ = 0;
    FILETIME bloomSeen_{};
    std::vector<Folder> folders_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::map<std::string, rc::Ask> asks_;  // Bloom's requests, by session
    std::map<std::string, rc::Ms> dismissed_;  // finished cards closed by hand: the session and its turn's end
    std::set<std::string> bloomRuns_;          // sessions seen working in Bloom's own folder
    uint64_t arrivals_ = 0;
    int ticks_ = 0;
    bool hidden_ = false;

    HWND hwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    RECT work_{};
    float scale_ = 1;
    rc::Box area_;  // the work area in the window, DIPs
    bool tracking_ = false;
    std::optional<std::pair<std::string, rc::Act>> press_;
    rc::Link link_;

    com_ptr<ID2D1Factory1> d2d_;
    com_ptr<IDWriteFactory> dwrite_;
    com_ptr<ID3D11Device> d3d_;
    com_ptr<ID2D1Device> device_;
    std::unique_ptr<rc::Painter> painter_;
    HANDLE frameTimer_ = nullptr;
    bool framing_ = false;

    winrt::Windows::System::DispatcherQueueController queue_{nullptr};
    wuc::Compositor comp_{nullptr};
    wucd::DesktopWindowTarget target_{nullptr};
    wuc::CompositionGraphicsDevice graphics_{nullptr};
    wuc::ContainerVisual rootVisual_{nullptr};
    wuc::ImplicitAnimationCollection moves_{nullptr};

    // ---- helpers ----
    void ReadLook(bool force);
    void StartReaders();
    void StopReaders();
    void ReadStatus();
    const Status* StatusOf(const std::string& session) const;
    void Place();
    void Update();
    void Layout();
    void Draw(Slot& s, rc::View view);
    void MakeVisuals(Slot& s);
    void Spin(Slot& s, bool on);
    void AnimateClip(Slot& s, rc::Size from, float fromR, rc::Size to, float toR);
    void FadeOut(Slot& s);
    void SetRegion(bool moving);
    void ArmFrames();
    std::string KeyOf(const Slot& s, rc::View view) const;
    Slot* Find(const std::string& id);
    Slot* At(POINT client, rc::Hit* hit);
    void OnMouse(UINT msg, LPARAM lParam);
    void OnLeave();
    void Act(Slot& s, const rc::Hit& hit);
    void Open(Slot& s);
    void Answer(Slot& s, const std::string& json);
    void OnLink(WPARAM what, std::string* line);
    void OnSnapshot(size_t folder, std::shared_ptr<const si::Snapshot> s);
    void OnSettings();
    void CheckFullscreen();
    bool DeviceLost(HRESULT hr) const {
        return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
    }
    void Recover();
    double Now() const { return NowMs(); }
};

App* g_app = nullptr;

com_ptr<ID2D1Device> CreateDevice(ID2D1Factory1* d2d, com_ptr<ID3D11Device>& d3d) {
    d3d = nullptr;
    HRESULT hr = E_FAIL;
    for (auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                               d3d.put(), nullptr, nullptr);
        if (SUCCEEDED(hr)) break;
    }
    check_hresult(hr);
    com_ptr<ID2D1Device> device;
    check_hresult(d2d->CreateDevice(d3d.as<IDXGIDevice>().get(), device.put()));
    return device;
}

// ---- start and stop -------------------------------------------------------------------------------------

bool App::Start() {
    DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    if (FAILED(CreateDispatcherQueueController(options,
                                               reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(winrt::put_abi(queue_))))) {
        Wh_Log(L"no dispatcher queue");
        return false;
    }
    D2D1_FACTORY_OPTIONS fo{};
    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, d2d_.put_void()));
    check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(dwrite_.put_void())));
    device_ = CreateDevice(d2d_.get(), d3d_);
    painter_ = std::make_unique<rc::Painter>(d2d_.get(), dwrite_.get());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ModuleInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) {
        Wh_Log(L"window class not registered: %u", GetLastError());
        return false;
    }
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    hwnd_ = CreateWindowExW(ex, kClass, L"Run Card", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, ModuleInstance(), this);
    if (!hwnd_) {
        Wh_Log(L"window failed: %u", GetLastError());
        return false;
    }
    g_window = hwnd_;
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    comp_ = wuc::Compositor();
    check_hresult(
        comp_.as<interop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd_, TRUE, winrt::put_abi(target_)));
    check_hresult(comp_.as<interop::ICompositorInterop>()->CreateGraphicsDevice(device_.get(), winrt::put_abi(graphics_)));
    rootVisual_ = comp_.CreateContainerVisual();
    target_.Root(rootVisual_);
    // A card moving to its new place in the stack glides there.
    auto move = comp_.CreateVector3KeyFrameAnimation();
    move.InsertExpressionKeyFrame(1, L"this.FinalValue", comp_.CreateCubicBezierEasingFunction({kEase[0], kEase[1]}, {kEase[2], kEase[3]}));
    move.Duration(std::chrono::milliseconds(kMorphMs));
    move.Target(L"Offset");
    moves_ = comp_.CreateImplicitAnimationCollection();
    moves_.Insert(L"Offset", move);

    frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameTimer_) frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    cfg_ = ReadConfig();
    ReadLook(true);
    Place();
    SetRegion(false);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    StartReaders();
    ReadStatus();
    link_.Start(hwnd_, kLinkMsg);
    SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
    Wh_Log(L"started: %d folders", int(folders_.size()));
    return true;
}

void App::Stop() {
    auto step = [](PCWSTR what, auto&& f) {
        try {
            f();
        } catch (...) {
            Wh_Log(L"stop: %ls failed", what);
        }
    };
    step(L"link", [&] { link_.Stop(); });
    step(L"readers", [&] { StopReaders(); });
    MSG msg;
    while (hwnd_ && PeekMessageW(&msg, hwnd_, kSnapshotMsg, kSnapshotMsg, PM_REMOVE))
        delete reinterpret_cast<std::shared_ptr<const si::Snapshot>*>(msg.lParam);
    while (hwnd_ && PeekMessageW(&msg, hwnd_, kLinkMsg, kLinkMsg, PM_REMOVE))
        delete reinterpret_cast<std::string*>(msg.lParam);
    step(L"cards", [&] { slots_.clear(); });
    step(L"composition", [&] {
        rootVisual_ = nullptr;
        moves_ = nullptr;
        if (target_) target_.Root(nullptr);
        target_ = nullptr;
        graphics_ = nullptr;
        comp_ = nullptr;
    });
    if (hwnd_) {
        g_window = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kClass, ModuleInstance());
    if (frameTimer_) CloseHandle(frameTimer_);
    frameTimer_ = nullptr;
    step(L"queue", [&] {
        if (queue_) queue_.ShutdownQueueAsync();
        queue_ = nullptr;
    });
    painter_.reset();
    device_ = nullptr;
    d3d_ = nullptr;
}

// ---- look --------------------------------------------------------------------------------------------------

void App::ReadLook(bool force) {
    FILETIME t = FileTime(BloomSettingsPath());
    if (!force && SameTime(t, bloomSeen_)) return;
    bloomSeen_ = t;
    AccentPair a = ReadAccent();
    bool light = cfg_.theme == ThemeChoice::Light || (cfg_.theme == ThemeChoice::System && SystemLight());
    rc::Palette p = rc::MakePalette(light, light ? a.light : a.dark);
    if (!force && memcmp(&p, &look_, sizeof(p)) == 0) return;
    look_ = p;
    lookId_++;
}

// ---- Claude Code's folders ----------------------------------------------------------------------------------

void App::StartReaders() {
    std::vector<std::wstring> dirs = ConfigFolders();
    for (size_t i = 0; i < dirs.size(); i++) {
        Folder f;
        f.dir = dirs[i];
        f.reader = std::make_unique<si::Reader>();
        si::ReaderOptions ro;
        ro.root = Env(L"RUN_CARD_ROOT").empty() ? f.dir + L"\\projects" : f.dir;
        HWND hwnd = hwnd_;
        WPARAM index = i;
        bool ok = f.reader->Start(ro, [hwnd, index](std::shared_ptr<const si::Snapshot> s) {
            auto* box = new std::shared_ptr<const si::Snapshot>(std::move(s));
            if (!PostMessageW(hwnd, kSnapshotMsg, index, reinterpret_cast<LPARAM>(box))) delete box;
        });
        if (!ok) Wh_Log(L"reader not started for a folder");
        folders_.push_back(std::move(f));
    }
}

void App::StopReaders() {
    for (auto& f : folders_)
        if (f.reader) f.reader->Stop();
    folders_.clear();
}

void App::ReadStatus() {
    for (auto& f : folders_) {
        std::wstring dir = f.dir + L"\\sessions";
        std::map<std::wstring, Status> next;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*.json").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = fd.cFileName;
                auto old = f.status.find(name);
                if (old != f.status.end() && SameTime(old->second.seen, fd.ftLastWriteTime)) {
                    next[name] = old->second;
                } else if (auto s = ::ReadStatus(dir + L"\\" + name)) {
                    s->seen = fd.ftLastWriteTime;
                    next[name] = *s;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        for (auto& [name, s] : next) {
            s.alive = StillRunning(s.pid, s.procStart);
            if (s.bloomRun) bloomRuns_.insert(s.session);
        }
        f.status = std::move(next);
    }
}

const Status* App::StatusOf(const std::string& session) const {
    for (auto& f : folders_)
        for (auto& [name, s] : f.status)
            if (s.session == session) return &s;
    return nullptr;
}

// ---- the window on its monitor --------------------------------------------------------------------------------

void App::Place() {
    monitor_ = rc::MonitorByNumber(cfg_.monitor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor_, &mi);
    work_ = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy);
    float scale = dx / 96.0f;
    bool rescaled = scale != scale_;
    scale_ = scale;
    int w = work_.right - work_.left, h = work_.bottom - work_.top;
    area_ = {0, 0, w / scale_, h / scale_};
    SetWindowPos(hwnd_, HWND_TOPMOST, work_.left, work_.top, w, h, SWP_NOACTIVATE);
    if (rescaled)
        for (auto& s : slots_) {
            s->key.clear();
            s->clipTo = {};  // the clip is in pixels: set again at the next layout
        }
}

// ---- the cards from the sessions -----------------------------------------------------------------------------

App::Slot* App::Find(const std::string& id) {
    for (auto& s : slots_)
        if (s->id == id && !s->gone) return s.get();
    return nullptr;
}

void App::Update() {
    if (!comp_) return;
    double now = Now();
    bool ko = g_korean;
    // The sessions worth a card: one whose turn runs, or ended a little while ago.
    struct Seen {
        rc::Card card;
        DWORD pid;
    };
    std::map<std::string, Seen> seen;
    for (auto& f : folders_) {
        if (!f.snap) continue;
        for (const si::SessionView& v : f.snap->sessions) {
            if (!v.map.turnStartedAt || seen.count(v.id)) continue;
            const Status* st = StatusOf(v.id);
            if (st && !st->interactive) continue;  // `claude -p` runs, programs' sessions
            if (bloomRuns_.count(v.id)) continue;   // Bloom's own chats: Bloom shows them itself
            rc::Card c = rc::CardOf(v, ko);
            if (!c.turnEnd && st && !st->alive) {
                // Claude Code is gone with the turn open: closed or killed.
                c.phase = rc::Phase::Stopped;
                c.turnEnd = v.lastActivity;
            }
            if (!c.turnEnd && !st && now - double(v.lastActivity) > kOrphanMs) {
                // No session file (gone with its Claude Code) and nothing written for minutes: a turn left open by
                // a Claude Code that was killed; it ended at its last line.
                c.phase = rc::Phase::Stopped;
                c.turnEnd = v.lastActivity;
            }
            if (!c.turnEnd && st && st->waiting) {
                c.asking = true;
                c.phase = rc::Phase::Ask;
            }
            // Closed by hand: stays closed until the session's next turn ends differently.
            if (auto d = dismissed_.find(v.id); d != dismissed_.end()) {
                if (c.turnEnd && *c.turnEnd == d->second) continue;
                dismissed_.erase(d);
            }
            Slot* slot = Find(v.id);
            bool held = slot && slot->hover;
            if (c.turnEnd && now >= double(*c.turnEnd) + cfg_.doneSeconds * 1000.0 && !held) continue;
            seen[v.id] = {std::move(c), st ? st->pid : 0};
        }
    }
    // Bloom's requests for sessions no longer shown go back to Bloom.
    for (auto it = asks_.begin(); it != asks_.end();) {
        if (!seen.count(it->first)) {
            link_.Send("{\"t\":\"none\",\"id\":" + rc::JsonString(it->second.id) + "}");
            it = asks_.erase(it);
        } else {
            ++it;
        }
    }
    // Cards in, out and changed.
    for (auto& s : slots_)
        if (!s->gone && !seen.count(s->id)) FadeOut(*s);
    for (auto& [id, sn] : seen) {
        Slot* s = Find(id);
        if (!s) {
            auto made = std::make_unique<Slot>();
            made->id = id;
            made->order = ++arrivals_;
            s = made.get();
            MakeVisuals(*s);
            slots_.push_back(std::move(made));
        }
        s->card = std::move(sn.card);
        s->pid = sn.pid;
        if (asks_.count(id)) s->card.phase = rc::Phase::Approve;
        rc::Phase p = s->card.phase;
        bool pinned = p == rc::Phase::Approve || p == rc::Phase::Ask || p == rc::Phase::Done || p == rc::Phase::Stopped;
        s->wantOpen = cfg_.always || s->hover || pinned;
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(), [&](const std::unique_ptr<Slot>& s) {
                     return s->gone && !s->root;
                 }),
                 slots_.end());
    Layout();
}

std::string App::KeyOf(const Slot& s, rc::View view) const {
    const rc::Card& c = s.card;
    std::string k = std::to_string(int(view)) + "|" + std::to_string(int(c.phase)) + "|" + std::to_string(lookId_) + "|" +
                    std::to_string(scale_) + "|" + std::to_string(c.filesChanged) + "|" + std::to_string(c.asking) + "|" +
                    Narrow(c.model) + "|" + std::to_string(int(s.ptr.hover)) + "," + std::to_string(s.ptr.q) + "," +
                    std::to_string(s.ptr.choice);
    for (const rc::Step& st : c.steps) k += "|" + Narrow(st.done) + (st.endedAt ? std::to_string(*st.endedAt) : "r");
    // A clock that moves (the turn runs) is part of the picture.
    if (!c.turnEnd) k += "|t" + std::to_string(int64_t(Now() / 1000));
    else k += "|e" + std::to_string(*c.turnEnd);
    auto a = asks_.find(s.id);
    if (a != asks_.end()) {
        k += "|ask" + a->second.id;
        for (auto& p : a->second.picks)
            for (int i : p) k += "," + std::to_string(i);
    }
    return k;
}

void App::Layout() {
    double now = Now();
    std::vector<Slot*> list;
    for (auto& s : slots_)
        if (!s->gone) list.push_back(s.get());
    std::sort(list.begin(), list.end(), [](Slot* a, Slot* b) { return a->order < b->order; });
    // At most kMaxCards; past that the oldest that are over go first, then the oldest.
    while (int(list.size()) > kMaxCards) {
        auto over = std::find_if(list.begin(), list.end(), [](Slot* s) {
            return s->card.phase == rc::Phase::Done || s->card.phase == rc::Phase::Stopped;
        });
        FadeOut(**(over != list.end() ? over : list.begin()));
        list.erase(over != list.end() ? over : list.begin());
    }
    auto sizeOf = [&](Slot* s) {
        auto a = asks_.find(s->id);
        rc::Laid L = painter_->Run(nullptr, s->card, a != asks_.end() ? &a->second : nullptr,
                                   s->wantOpen ? rc::View::Card : rc::View::Pill, look_, rc::Ms(now), s->ptr, g_korean);
        return rc::Size{L.w, L.h};
    };
    std::vector<rc::Size> sizes;
    for (Slot* s : list) sizes.push_back(sizeOf(s));
    // Too tall for the monitor: fold open cards (not one being pointed at or asking), oldest first.
    auto total = [&] {
        float t = 0;
        for (auto& z : sizes) t += z.h + kGap;
        return t - kGap;
    };
    for (size_t i = 0; i < list.size() && total() > rc::Room(area_, cfg_.margin); i++) {
        Slot* s = list[i];
        if (!s->wantOpen || s->hover || s->card.phase == rc::Phase::Approve) continue;
        s->wantOpen = false;
        sizes[i] = sizeOf(s);
    }
    std::vector<rc::Box> boxes = rc::Stack(sizes, cfg_.anchor, area_, cfg_.margin, kGap);
    bool moved = false;
    for (size_t i = 0; i < list.size(); i++) {
        Slot& s = *list[i];
        rc::Box b = boxes[i];
        bool morphing = false;  // its clip is being animated from this layout on
        if (s.wantOpen != s.open) {
            morphing = s.placed;
            rc::Size from{s.box.w, s.box.h}, to{b.w, b.h};
            if (s.wantOpen) {
                // Opens: the card drawn now, shown from the pill's size out.
                s.open = true;
                s.foldAt = 0;
                Draw(s, rc::View::Card);
                if (s.placed) AnimateClip(s, from, rc::kPillH / 2, to, rc::kRadius);
            } else {
                // Folds: the card stays drawn while its clip closes to the pill, then the pill is drawn.
                s.open = false;
                if (s.placed) {
                    AnimateClip(s, from, rc::kRadius, to, rc::kPillH / 2);
                    s.foldAt = now + kMorphMs;
                    SetTimer(hwnd_, kMorphTimer, kMorphMs + 20, nullptr);
                } else {
                    Draw(s, rc::View::Pill);
                }
            }
        }
        rc::View view = s.open ? rc::View::Card : s.foldAt > now ? rc::View::Card : rc::View::Pill;
        if (KeyOf(s, view) != s.key) Draw(s, view);
        // A card at rest is shown whole by its clip (a card that grew or shrank in place, a pill whose text changed
        // while it folded).
        if (s.placed && !morphing && (s.open || s.foldAt <= now) && (b.w != s.clipTo.w || b.h != s.clipTo.h)) {
            float r = (s.open ? rc::kRadius : rc::kPillH / 2) * scale_;
            s.clip.StopAnimation(L"Size");
            s.clip.StopAnimation(L"CornerRadius");
            s.clip.Size({b.w * scale_, b.h * scale_});
            s.clip.CornerRadius({r, r});
            s.clipTo = {b.w, b.h};
        }
        bool newPlace = !s.placed || s.box.x != b.x || s.box.y != b.y || s.box.w != b.w || s.box.h != b.h;
        moved = moved || newPlace;
        s.box = b;
        wfn::float3 at{std::round(b.x * scale_), std::round(b.y * scale_), 0};
        if (!s.placed) {
            s.root.Offset(at);
            s.root.ImplicitAnimations(moves_);
            s.clip.Size({b.w * scale_, b.h * scale_});
            s.clipTo = {b.w, b.h};
            s.clip.CornerRadius(s.open ? wfn::float2{rc::kRadius * scale_, rc::kRadius * scale_}
                                       : wfn::float2{rc::kPillH / 2 * scale_, rc::kPillH / 2 * scale_});
            auto in = comp_.CreateScalarKeyFrameAnimation();
            in.InsertKeyFrame(0, 0);
            in.InsertKeyFrame(1, 1);
            in.Duration(std::chrono::milliseconds(kFadeMs));
            s.root.Opacity(1);
            s.root.StartAnimation(L"Opacity", in);
            s.placed = true;
        } else {
            s.root.Offset(at);
        }
        Spin(s, s.card.phase == rc::Phase::Work && !hidden_);
    }
    if (moved) SetRegion(true);
    ArmFrames();
}

void App::MakeVisuals(Slot& s) {
    s.root = comp_.CreateContainerVisual();
    s.root.Opacity(0);
    s.clip = comp_.CreateRoundedRectangleGeometry();
    s.root.Clip(comp_.CreateGeometricClip(s.clip));
    s.surf = graphics_.CreateDrawingSurface({1, 1}, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                            wgd::DirectXAlphaMode::Premultiplied);
    s.sprite = comp_.CreateSpriteVisual();
    auto brush = comp_.CreateSurfaceBrush(s.surf);
    brush.Stretch(wuc::CompositionStretch::None);
    brush.HorizontalAlignmentRatio(0);
    brush.VerticalAlignmentRatio(0);
    s.sprite.Brush(brush);
    s.root.Children().InsertAtTop(s.sprite);
    s.ring = comp_.CreateShapeVisual();
    s.ringGeom = comp_.CreateEllipseGeometry();
    s.ringGeom.TrimEnd(0.28f);
    s.ringBrush = comp_.CreateColorBrush();
    auto shape = comp_.CreateSpriteShape(s.ringGeom);
    shape.StrokeBrush(s.ringBrush);
    shape.StrokeStartCap(wuc::CompositionStrokeCap::Round);
    shape.StrokeEndCap(wuc::CompositionStrokeCap::Round);
    s.ring.Shapes().Append(shape);
    s.ring.IsVisible(false);
    s.root.Children().InsertAtTop(s.ring);
    rootVisual_.Children().InsertAtTop(s.root);
}

void App::Draw(Slot& s, rc::View view) {
    double now = Now();
    auto a = asks_.find(s.id);
    const rc::Ask* ask = a != asks_.end() ? &a->second : nullptr;
    rc::Laid L = painter_->Run(nullptr, s.card, ask, view, look_, rc::Ms(now), s.ptr, g_korean);
    int w = std::max(1, int(std::ceil(L.w * scale_))), h = std::max(1, int(std::ceil(L.h * scale_)));
    auto size = s.surf.SizeInt32();
    if (size.Width != w || size.Height != h) s.surf.Resize({w, h});
    auto sdi = s.surf.as<interop::ICompositionDrawingSurfaceInterop>();
    com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = sdi->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
    if (FAILED(hr)) {
        if (DeviceLost(hr)) Recover();
        return;
    }
    dc->SetDpi(96, 96);
    D2D1_COLOR_F clear{};
    dc->SetTransform(D2D1::Matrix3x2F::Translation(float(offset.x), float(offset.y)));
    dc->Clear(&clear);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) * D2D1::Matrix3x2F::Translation(float(offset.x), float(offset.y)));
    try {
        s.laid = painter_->Run(dc.get(), s.card, ask, view, look_, rc::Ms(now), s.ptr, g_korean);
    } catch (...) {
        Wh_Log(L"paint failed");
    }
    hr = sdi->EndDraw();
    if (DeviceLost(hr)) Recover();
    s.drawn = view;
    s.key = KeyOf(s, view);
    s.sprite.Size({float(w), float(h)});
    s.ring.Size({float(w), float(h)});
    s.ringGeom.Center({s.laid.markAt.x * scale_, s.laid.markAt.y * scale_});
    s.ringGeom.Radius({rc::kRingR * scale_, rc::kRingR * scale_});
    s.ring.CenterPoint({s.laid.markAt.x * scale_, s.laid.markAt.y * scale_, 0});
    s.ringBrush.Color(WinColor(look_.text));
    s.ring.Shapes().GetAt(0).as<wuc::CompositionSpriteShape>().StrokeThickness(rc::kRingW * scale_);
}

void App::Spin(Slot& s, bool on) {
    if (on == s.spinning) return;
    s.spinning = on;
    s.ring.IsVisible(on);
}

void App::AnimateClip(Slot& s, rc::Size from, float fromR, rc::Size to, float toR) {
    auto ease = comp_.CreateCubicBezierEasingFunction({kEase[0], kEase[1]}, {kEase[2], kEase[3]});
    auto size = comp_.CreateVector2KeyFrameAnimation();
    size.InsertKeyFrame(0, {from.w * scale_, from.h * scale_});
    size.InsertKeyFrame(1, {to.w * scale_, to.h * scale_}, ease);
    size.Duration(std::chrono::milliseconds(kMorphMs));
    s.clip.StartAnimation(L"Size", size);
    s.clipTo = to;
    auto radius = comp_.CreateVector2KeyFrameAnimation();
    radius.InsertKeyFrame(0, {fromR * scale_, fromR * scale_});
    radius.InsertKeyFrame(1, {toR * scale_, toR * scale_}, ease);
    radius.Duration(std::chrono::milliseconds(kMorphMs));
    s.clip.StartAnimation(L"CornerRadius", radius);
}

void App::FadeOut(Slot& s) {
    if (s.gone) return;
    s.gone = true;
    s.hover = false;
    Spin(s, false);
    if (!s.root) return;
    auto out = comp_.CreateScalarKeyFrameAnimation();
    out.InsertKeyFrame(1, 0);
    out.Duration(std::chrono::milliseconds(kFadeMs));
    auto batch = comp_.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    s.root.StartAnimation(L"Opacity", out);
    batch.End();
    Slot* gone = &s;
    batch.Completed([this, gone](auto&&, auto&&) {
        for (auto& k : slots_)
            if (k.get() == gone) {
                rootVisual_.Children().Remove(k->root);
                k->root = nullptr;
            }
        slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                    [](const std::unique_ptr<Slot>& k) { return k->gone && !k->root; }),
                     slots_.end());
        SetRegion(false);
    });
}

// The window takes clicks only on the cards: its region is their shapes (their boxes while they move).
void App::SetRegion(bool moving) {
    HRGN all = CreateRectRgn(0, 0, 0, 0);
    for (auto& s : slots_) {
        if (s->gone || !s->placed) continue;
        const rc::Box& b = s->box;
        int l = int(std::floor(b.x * scale_)), t = int(std::floor(b.y * scale_));
        int r = int(std::ceil(b.Right() * scale_)), btm = int(std::ceil(b.Bottom() * scale_));
        HRGN one;
        if (moving) {
            one = CreateRectRgn(l, t, r, btm);
        } else {
            int d = int(std::ceil(2 * (s->open ? rc::kRadius : rc::kPillH / 2) * scale_));
            one = CreateRoundRectRgn(l, t, r + 1, btm + 1, d, d);
        }
        CombineRgn(all, all, one, RGN_OR);
        DeleteObject(one);
    }
    if (moving) {
        // The boxes the cards come from as well, while they glide.
        RECT now{};
        HRGN old = CreateRectRgn(0, 0, 0, 0);
        if (GetWindowRgn(hwnd_, old) != ERROR) CombineRgn(all, all, old, RGN_OR);
        DeleteObject(old);
        (void)now;
        KillTimer(hwnd_, kRegionTimer);
        SetTimer(hwnd_, kRegionTimer, kMorphMs + 40, nullptr);
    }
    SetWindowRgn(hwnd_, all, TRUE);
}

// The ring turns while a card works: a frame every 1/60 s.
void App::ArmFrames() {
    bool any = false;
    for (auto& s : slots_) any = any || s->spinning;
    if (any == framing_) return;
    framing_ = any;
    if (any) {
        LARGE_INTEGER due{};
        due.QuadPart = -int64_t(kFrameMs * 10000);
        SetWaitableTimer(frameTimer_, &due, 0, nullptr, nullptr, FALSE);
    } else {
        CancelWaitableTimer(frameTimer_);
    }
}

void App::OnFrame() {
    if (!framing_) return;
    double now = Now();
    float angle = float(std::fmod(now / kRingTurnMs, 1.0) * 360.0);
    for (auto& s : slots_)
        if (s->spinning) s->ring.RotationAngleInDegrees(angle);
    // The next frame on the next 1/60 s boundary, so the turning keeps one beat.
    double next = kFrameMs - std::fmod(now, kFrameMs);
    LARGE_INTEGER due{};
    due.QuadPart = -int64_t(std::max(1.0, next) * 10000);
    SetWaitableTimer(frameTimer_, &due, 0, nullptr, nullptr, FALSE);
}

void App::Recover() {
    // A lost device: make a new one and draw every card again.
    try {
        device_ = CreateDevice(d2d_.get(), d3d_);
        graphics_.as<interop::ICompositorInterop>();
        com_ptr<IUnknown> unknown;
        winrt::Windows::UI::Composition::CompositionGraphicsDevice g{nullptr};
        check_hresult(comp_.as<interop::ICompositorInterop>()->CreateGraphicsDevice(device_.get(), winrt::put_abi(g)));
        graphics_ = g;
        for (auto& s : slots_) {
            s->surf = graphics_.CreateDrawingSurface({1, 1}, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                     wgd::DirectXAlphaMode::Premultiplied);
            s->sprite.Brush().as<wuc::CompositionSurfaceBrush>().Surface(s->surf);
            s->key.clear();
        }
    } catch (...) {
        Wh_Log(L"recover failed");
    }
}

// ---- snapshots, settings, Bloom -------------------------------------------------------------------------------

void App::OnSnapshot(size_t folder, std::shared_ptr<const si::Snapshot> s) {
    if (folder >= folders_.size()) return;
    folders_[folder].snap = std::move(s);
    Update();
}

void App::OnSettings() {
    Config old = cfg_;
    cfg_ = ReadConfig();
    if (old.approvals != cfg_.approvals)
        link_.Send(std::string("{\"t\":\"prefs\",\"approvals\":") + (cfg_.approvals ? "true" : "false") + "}");
    ReadLook(true);
    for (auto& s : slots_) s->key.clear();
    Place();
    Update();
    SetRegion(false);
}

void App::OnLink(WPARAM what, std::string* line) {
    std::unique_ptr<std::string> own(line);
    if (what == rc::Link::kUp) {
        link_.Send(std::string("{\"t\":\"hello\",\"v\":1,\"approvals\":") + (cfg_.approvals ? "true" : "false") + "}");
        return;
    }
    if (what == rc::Link::kDown) {
        asks_.clear();
        Update();
        return;
    }
    if (!line) return;
    wdj::JsonObject o{nullptr};
    if (!wdj::JsonObject::TryParse(Widen(*line), o)) return;
    try {
        std::wstring t(o.GetNamedString(L"t", L""));
        if (t == L"ask") {
            rc::Ask a;
            a.id = winrt::to_string(o.GetNamedString(L"id", L""));
            std::string session = winrt::to_string(o.GetNamedString(L"session", L""));
            a.tool = std::wstring(o.GetNamedString(L"tool", L""));
            a.body = std::wstring(o.GetNamedString(L"body", L""));
            if (o.HasKey(L"questions") && o.GetNamedValue(L"questions").ValueType() == wdj::JsonValueType::Array) {
                for (auto const& qv : o.GetNamedArray(L"questions")) {
                    auto q = qv.GetObject();
                    rc::Question question;
                    question.header = std::wstring(q.GetNamedString(L"header", L""));
                    question.text = std::wstring(q.GetNamedString(L"text", L""));
                    question.multi = q.GetNamedBoolean(L"multi", false);
                    for (auto const& cv : q.GetNamedArray(L"choices")) {
                        auto ch = cv.GetObject();
                        question.choices.push_back(
                            {std::wstring(ch.GetNamedString(L"label", L"")), std::wstring(ch.GetNamedString(L"description", L""))});
                    }
                    a.questions.push_back(std::move(question));
                }
                a.picks.resize(a.questions.size());
            }
            // Shown only whole (Fits; the card no taller than the room), and one at a time per session: a second one
            // (parallel subagents ask under their session's id) goes to Bloom's card, which shows several at once.
            Slot* s = Find(session);
            bool show = cfg_.approvals && s && !hidden_ && !a.id.empty() && !asks_.count(session) &&
                        painter_->Fits(a, g_korean) &&
                        painter_->Run(nullptr, s->card, &a, rc::View::Card, look_, rc::Ms(Now()), {}, g_korean).h <=
                            rc::Room(area_, cfg_.margin);
            link_.Send(std::string("{\"t\":\"") + (show ? "shown" : "none") + "\",\"id\":" + rc::JsonString(a.id) + "}");
            if (!show) return;
            asks_[session] = std::move(a);
            Update();
        } else if (t == L"cancel") {
            std::string id = winrt::to_string(o.GetNamedString(L"id", L""));
            for (auto it = asks_.begin(); it != asks_.end(); ++it)
                if (it->second.id == id) {
                    asks_.erase(it);
                    Update();
                    break;
                }
        }
    } catch (...) {
        Wh_Log(L"a line from Bloom was not understood");
    }
}

// ---- input -------------------------------------------------------------------------------------------------------

App::Slot* App::At(POINT client, rc::Hit* hit) {
    float x = client.x / scale_, y = client.y / scale_;
    for (auto& s : slots_) {
        if (s->gone || !s->placed) continue;
        const rc::Box& b = s->box;
        if (x < b.x || y < b.y || x >= b.Right() || y >= b.Bottom()) continue;
        if (hit) {
            *hit = {};
            float lx = x - b.x, ly = y - b.y;
            // The buttons first (the header's whole row is the last resort).
            for (int pass = 0; pass < 2 && hit->act == rc::Act::None; pass++)
                for (const rc::Hit& h : s->laid.hits) {
                    if ((h.act == rc::Act::Header) != (pass == 1)) continue;
                    if (lx >= h.r.left && lx < h.r.right && ly >= h.r.top && ly < h.r.bottom) {
                        *hit = h;
                        break;
                    }
                }
        }
        return s.get();
    }
    return nullptr;
}

void App::OnMouse(UINT msg, LPARAM lParam) {
    POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    rc::Hit hit;
    Slot* s = At(p, &hit);
    if (msg == WM_MOUSEMOVE) {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme);
        }
        bool changed = false;
        for (auto& k : slots_) {
            bool on = k.get() == s;
            if (k->hover != on) {
                k->hover = on;
                changed = true;
            }
            rc::Pointer ptr = on ? rc::Pointer{hit.act, hit.q, hit.choice, false} : rc::Pointer{};
            if (ptr.hover != k->ptr.hover || ptr.q != k->ptr.q || ptr.choice != k->ptr.choice) {
                k->ptr = ptr;
                changed = true;
            }
        }
        if (s) KillTimer(hwnd_, kLeaveTimer);
        if (changed) {
            if (s && !s->open && !cfg_.always) Update();  // opens now
            else if (!s) SetTimer(hwnd_, kLeaveTimer, kLeaveMs, nullptr);
            else Layout();
        }
        return;
    }
    if (msg == WM_LBUTTONDOWN) {
        press_.reset();
        if (s && hit.act != rc::Act::None) press_ = {s->id, hit.act};
        return;
    }
    if (msg == WM_LBUTTONUP) {
        auto was = press_;
        press_.reset();
        if (s && was && was->first == s->id && was->second == hit.act) Act(*s, hit);
        return;
    }
    if (msg == WM_RBUTTONUP && s && (s->card.phase == rc::Phase::Done || s->card.phase == rc::Phase::Stopped)) {
        if (s->card.turnEnd) dismissed_[s->id] = *s->card.turnEnd;
        FadeOut(*s);
        Layout();
    }
}

void App::OnLeave() {
    tracking_ = false;
    SetTimer(hwnd_, kLeaveTimer, kLeaveMs, nullptr);
}

void App::Act(Slot& s, const rc::Hit& hit) {
    auto a = asks_.find(s.id);
    switch (hit.act) {
        case rc::Act::Header:
        case rc::Act::Open:
            if (a != asks_.end()) Answer(s, "\"value\":\"terminal\"");
            else Open(s);
            break;
        case rc::Act::Allow: Answer(s, "\"value\":\"allow\""); break;
        case rc::Act::Deny: Answer(s, "\"value\":\"deny\""); break;
        case rc::Act::Pick:
            if (a != asks_.end() && hit.q >= 0 && hit.q < int(a->second.questions.size())) {
                auto& picks = a->second.picks[hit.q];
                if (a->second.questions[hit.q].multi) {
                    auto at = std::find(picks.begin(), picks.end(), hit.choice);
                    if (at == picks.end()) picks.push_back(hit.choice);
                    else picks.erase(at);
                } else {
                    picks = {hit.choice};
                }
                Layout();
            }
            break;
        case rc::Act::Send:
            if (a != asks_.end() && a->second.Answered()) {
                std::string picks = "[";
                for (size_t q = 0; q < a->second.picks.size(); q++) {
                    picks += q ? ",[" : "[";
                    for (size_t i = 0; i < a->second.picks[q].size(); i++)
                        picks += (i ? "," : "") + std::to_string(a->second.picks[q][i]);
                    picks += "]";
                }
                Answer(s, "\"picks\":" + picks + "]");
            }
            break;
        default: break;
    }
}

void App::Answer(Slot& s, const std::string& json) {
    auto a = asks_.find(s.id);
    if (a == asks_.end()) return;
    // "In terminal": Bloom brings the session's window forward, with the right to that this click gave explorer.
    if (DWORD bloom = link_.BloomPid()) AllowSetForegroundWindow(bloom);
    link_.Send("{\"t\":\"answer\",\"id\":" + rc::JsonString(a->second.id) + "," + json + "}");
    asks_.erase(a);
    Update();
}

// Open: as a click on Bloom's own card for the session does (its chat in the taskbar, or its window); without
// Bloom, the session's window.
void App::Open(Slot& s) {
    if (link_.Up()) {
        if (DWORD bloom = link_.BloomPid()) AllowSetForegroundWindow(bloom);
        if (link_.Send("{\"t\":\"open\",\"session\":" + rc::JsonString(s.id) + ",\"pid\":" + std::to_string(s.pid) + "}"))
            return;
    }
    if (s.pid && !TestMode()) RaiseSession(s.pid);
}

// ---- fullscreen ---------------------------------------------------------------------------------------------
// A fullscreen app (a game, a video) on the cards' monitor keeps them away, as Session island does.

bool CoversMonitor(HWND window, const RECT& monitor) {
    RECT rc;
    return window && IsWindow(window) && IsWindowVisible(window) && !IsIconic(window) && GetWindowRect(window, &rc) &&
           rc.left <= monitor.left && rc.top <= monitor.top && rc.right >= monitor.right && rc.bottom >= monitor.bottom;
}

bool IsShellWindow(HWND window) {
    WCHAR name[64];
    return GetClassNameW(window, name, ARRAYSIZE(name)) &&
           (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 || wcscmp(name, L"Shell_TrayWnd") == 0);
}

void App::CheckFullscreen() {
    HWND fg = GetForegroundWindow();
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor_, &mi);
    bool full = false;
    if (fg && fg != hwnd_ && MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == monitor_ && !IsShellWindow(fg) &&
        CoversMonitor(fg, mi.rcMonitor) && !IsZoomed(fg)) {
        QUERY_USER_NOTIFICATION_STATE st{};
        full = SUCCEEDED(SHQueryUserNotificationState(&st)) &&
               (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_BUSY || st == QUNS_PRESENTATION_MODE);
    }
    if (full == hidden_) return;
    hidden_ = full;
    ShowWindow(hwnd_, full ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (full) {
        // Bloom's requests go back to Bloom's own cards while the cards are away.
        for (auto& [session, a] : asks_) link_.Send("{\"t\":\"none\",\"id\":" + rc::JsonString(a.id) + "}");
        asks_.clear();
    }
    for (auto& s : slots_) Spin(*s, !full && !s->gone && s->card.phase == rc::Phase::Work);
    ArmFrames();
}

// ---- window procedure -------------------------------------------------------------------------------------

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    try {
        switch (msg) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
                if (self) self->OnMouse(msg, lParam);
                return 0;
            case WM_MOUSELEAVE:
                if (self) self->OnLeave();
                return 0;
            case WM_SETCURSOR: {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                rc::Hit hit;
                bool hand = self && self->At(p, &hit) && hit.act != rc::Act::None;
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            case WM_TIMER:
                if (!self) break;
                if (wParam == kTickTimer) {
                    if (++self->ticks_ % kStatusEvery == 0) self->ReadStatus();
                    if (self->ticks_ % kRescanTicks == 0) {
                        auto now = ConfigFolders();
                        std::vector<std::wstring> had;
                        for (auto& f : self->folders_) had.push_back(f.dir);
                        if (now != had) {
                            self->StopReaders();
                            self->StartReaders();
                        }
                    }
                    self->ReadLook(false);
                    self->CheckFullscreen();
                    self->Update();
                } else if (wParam == kLeaveTimer) {
                    KillTimer(hwnd, kLeaveTimer);
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(hwnd, &p);
                    App::Slot* under = self->At(p, nullptr);
                    for (auto& s : self->slots_)
                        if (s.get() != under) s->hover = false, s->ptr = {};
                    self->Update();
                } else if (wParam == kRegionTimer) {
                    KillTimer(hwnd, kRegionTimer);
                    self->SetRegion(false);
                } else if (wParam == kMorphTimer) {
                    KillTimer(hwnd, kMorphTimer);
                    self->Layout();
                    self->SetRegion(false);
                }
                return 0;
            case kSnapshotMsg: {
                auto* box = reinterpret_cast<std::shared_ptr<const si::Snapshot>*>(lParam);
                std::shared_ptr<const si::Snapshot> s = std::move(*box);
                delete box;
                if (self) self->OnSnapshot(size_t(wParam), std::move(s));
                return 0;
            }
            case kSettingsMsg:
                if (self) self->OnSettings();
                return 0;
            case kLinkMsg:
                if (self) self->OnLink(wParam, reinterpret_cast<std::string*>(lParam));
                else delete reinterpret_cast<std::string*>(lParam);
                return 0;
            case WM_DISPLAYCHANGE:
            case WM_DPICHANGED:
                if (self) self->OnSettings();
                return 0;
            case WM_SETTINGCHANGE:
                if (self && (wParam == SPI_SETWORKAREA || (lParam && wcscmp(reinterpret_cast<PCWSTR>(lParam), L"ImmersiveColorSet") == 0)))
                    self->OnSettings();
                break;
        }
    } catch (...) {
        Wh_Log(L"window message %u failed", msg);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// The UI thread: its own message loop, DispatcherQueue and window, so the taskbar's thread is never held up.
// ============================================================================

HANDLE g_thread = nullptr;

DWORD WINAPI UiThread(void* param) {
    HANDLE stop = param;  // StopUi closes it only after this thread has ended
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    {
        App app;
        bool ok = false;
        try {
            ok = app.Start();
        } catch (const winrt::hresult_error& e) {
            Wh_Log(L"start failed: %08X", (unsigned)e.code());
        } catch (...) {
            Wh_Log(L"start failed");
        }
        g_app = ok ? &app : nullptr;
        while (ok) {
            HANDLE handles[2] = {stop, app.FrameTimer()};
            DWORD n = app.FrameTimer() ? 2 : 1;
            DWORD r = MsgWaitForMultipleObjectsEx(n, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
            try {
                if (r == WAIT_OBJECT_0 + 1 && n == 2) app.OnFrame();
            } catch (...) {
                Wh_Log(L"frame failed");
            }
            MSG msg;
            bool quit = false;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    quit = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit) break;
        }
        g_app = nullptr;
        try {
            app.Stop();
        } catch (...) {
            Wh_Log(L"stop failed");
        }
    }
    winrt::uninit_apartment();
    return 0;
}

void StartUi() {
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return;
    g_thread = CreateThread(nullptr, 0, UiThread, g_stop, 0, nullptr);
}

void StopUi() {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        bool ended = WaitForSingleObject(g_thread, 5000) == WAIT_OBJECT_0;
        CloseHandle(g_thread);
        g_thread = nullptr;
        if (!ended) {
            // It may still wait on the stop event: leave it open (and set) rather than hand it a closed handle.
            Wh_Log(L"the cards' thread did not end");
            g_stop = nullptr;
            return;
        }
    }
    if (g_stop) CloseHandle(g_stop);
    g_stop = nullptr;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");
    // The runtime may also be put into other processes (ShellHost.exe for the taskbar add-on): the cards belong to
    // explorer alone (the test program, in test mode, excepted).
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = n > 0 && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!TestMode() && (!name || _wcsicmp(name + 1, L"explorer.exe") != 0)) {
        Wh_Log(L"not explorer: the cards stay off here");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    StartUi();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");
    StopUi();
}

void Wh_ModUninit() {
    Wh_Log(L">");
    StopUi();
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    *bReload = FALSE;
    if (HWND w = g_window) PostMessageW(w, kSettingsMsg, 0, 0);
    return TRUE;
}
