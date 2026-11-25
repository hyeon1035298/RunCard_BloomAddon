// ==WindhawkMod==
// @id              run-card
// @name            Task Cards
// @description     A live card for each running Claude Code session: what it does now, its steps, and its questions to answer
// @version         0.1.4
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

Nothing is installed into Claude Code: the cards read the transcripts Claude Code writes and the status files
beside them, and use no tokens. At rest nothing moves or redraws.
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
  - arrange: stack
    $name: 늘어놓기
    $description: 위·아래 자리에서 카드를 세로로 쌓거나 가로로 한 줄에 놓습니다. 가운데 자리는 세로뿐입니다
    $options:
    - stack: 세로
    - row: 가로
  - margin: 16
    $name: 가장자리 여백
    $description: 화면 가장자리와 카드 사이 (0~200)
  - size: "100"
    $name: 크기
    $description: 카드의 크기(%). 가장자리 여백은 그대로입니다
    $options:
    - 80: 80%
    - 90: 90%
    - 100: 100%
    - 110: 110%
    - 125: 125%
    - 150: 150%
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
  - subtasks: false
    $name: 하위 작업 보기
    $description: 켜면 하위 에이전트와 하위 세션의 단계도 메인 카드에 보입니다. 하위의 질문·승인은 꺼도 메인 카드에 나옵니다
  $name: 카드
- pill:
  - theme: system
    $name: 테마
    $description: 카드의 라이트·다크. Bloom 애드온으로 쓰면 Bloom의 테마를 따릅니다
    $options:
    - system: Windows 작업표시줄 테마
    - dark: 다크
    - light: 라이트
  - width: same
    $name: 접힌 모양
    $description: 접힌 카드의 모양. 같은 길이면 펼친 카드와 같은 너비이고 넘치는 글은 줄임표로 줄입니다. 아이콘만이면 동그란 아이콘 하나로 줄어들고, 포인터를 올리면 카드로 펼쳐집니다
    $options:
    - same: 같은 길이
    - fit: 내용에 맞춤
    - icon: 아이콘만
  $name: 모양
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <windowsx.h>

// CoreMessaging's CreateDispatcherQueueController, declared here: <dispatcherqueue.h> brings in windows.system.h,
// whose IReference<boolean> and IReference<BYTE> are one type in llvm-mingw's headers and fail to compile.
enum DISPATCHERQUEUE_THREAD_APARTMENTTYPE { DQTAT_COM_NONE = 0, DQTAT_COM_ASTA = 1, DQTAT_COM_STA = 2 };
enum DISPATCHERQUEUE_THREAD_TYPE { DQTYPE_THREAD_DEDICATED = 1, DQTYPE_THREAD_CURRENT = 2 };
struct DispatcherQueueOptions {
    DWORD dwSize;
    DISPATCHERQUEUE_THREAD_TYPE threadType;
    DISPATCHERQUEUE_THREAD_APARTMENTTYPE apartmentType;
};
extern "C" HRESULT WINAPI CreateDispatcherQueueController(DispatcherQueueOptions options, void** controller);

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
#include "rc_glass.h"
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

// Windows' animation effects (Settings > Accessibility > Visual effects), read as each change begins.
bool Animations() {
    BOOL on = TRUE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
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

// Milliseconds on a clock that only goes forward (since the PC started): the cards' motions and waits are timed on
// it, so the wall clock set back or on does not stop one halfway or stretch it.
double TickMs() {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / perMs;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Nothing but spaces and line breaks.
bool Blank(const std::wstring& s) {
    return s.find_first_not_of(L" \t\r\n\u3000") == std::wstring::npos;
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
    bool row = false;  // card.arrange: row (else stack); only the top and bottom places (rc::Across)
    float margin = 16;
    int size = 100;       // card.size, %: the cards drawn this much larger than at the monitor's own scale
    bool always = false;  // card.expand: always (else on hover)
    int doneSeconds = 20;
    bool approvals = true;
    bool subtasks = false;  // card.subtasks: subs' steps on the main card (else its own only)
    ThemeChoice theme = ThemeChoice::System;
    // pill.width, a folded card's shape: a pill as wide as a card, one as wide as its words, or the mark alone.
    enum class Fold { Same, Fit, Icon } fold = Fold::Same;
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
    c.row = StringSetting(L"card.arrange") == L"row";
    c.margin = float(std::clamp(Wh_GetIntSetting(L"card.margin"), 0, 200));
    int size = _wtoi(StringSetting(L"card.size").c_str());
    c.size = size ? std::clamp(size, 80, 150) : 100;
    c.always = StringSetting(L"card.expand") == L"always";
    c.doneSeconds = std::clamp(Wh_GetIntSetting(L"card.doneSeconds"), 0, 600);
    c.approvals = Wh_GetIntSetting(L"card.approvals") != 0;
    c.subtasks = Wh_GetIntSetting(L"card.subtasks") != 0;
    std::wstring theme = StringSetting(L"pill.theme");
    c.theme = theme == L"dark" ? ThemeChoice::Dark : theme == L"light" ? ThemeChoice::Light : ThemeChoice::System;
    std::wstring fold = StringSetting(L"pill.width");
    c.fold = fold == L"fit" ? Config::Fold::Fit : fold == L"icon" ? Config::Fold::Icon : Config::Fold::Same;
    return c;
}

// ============================================================================
// Bloom's accent (its settings.json: "accent", a preset, or "custom" with "accentCustom" #RRGGBB, the light
// theme's colour), with Bloom's own pairs.
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

// What the cards take from Bloom's settings.json: its accent, and its glass (유리 효과 "transparency": "system" or
// "off"; 유리 투명도 "glassTransparency", 15-75, default 30).
struct BloomPrefs {
    AccentPair accent{0xFFFF7A9E, 0xFFE04E78};
    bool transparency = true;
    int glassTransparency = 30;
};

BloomPrefs ReadBloom() {
    BloomPrefs b;
    auto text = ReadText(BloomSettingsPath(), 4 * 1024 * 1024);
    wdj::JsonObject o{nullptr};
    if (!text || !wdj::JsonObject::TryParse(*text, o)) return b;
    auto str = [&](PCWSTR name, PCWSTR fallback) {
        return o.HasKey(name) && o.GetNamedValue(name).ValueType() == wdj::JsonValueType::String
                   ? std::wstring(o.GetNamedString(name))
                   : std::wstring(fallback);
    };
    b.accent = BloomAccent(str(L"accent", L"rose"), str(L"accentCustom", L"")).value_or(b.accent);
    b.transparency = str(L"transparency", L"system") != L"off";
    if (o.HasKey(L"glassTransparency") && o.GetNamedValue(L"glassTransparency").ValueType() == wdj::JsonValueType::Number)
        b.glassTransparency = int(std::lround(o.GetNamedNumber(L"glassTransparency")));
    return b;
}

// Windows' 투명 효과 (Personalization > Colors), on when it isn't set, as Bloom reads it.
bool SystemTransparency() {
    DWORD value = 1, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS ||
           value != 0;
}

DWORD WindowsBuild() {
    static const DWORD build = [] {
        using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        auto f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return f && f(&v) == 0 ? v.dwBuildNumber : 0ul;
    }();
    return build;
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
    bool cli = false;       // entrypoint "cli": Claude Code in a terminal, reading its console (Stop can reach it)
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
        s.cli = std::wstring(o.GetNamedString(L"entrypoint", L"")) == L"cli";
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
constexpr UINT kEditMsg = WM_APP + 4;      // 보내기's edit box lost the keys
constexpr double kNoteMs = 3000;           // what came of a send stays in its row this long
constexpr double kSayWaitMs = 60000;       // a send Bloom has not answered by then did not go
constexpr int kSayMax = 20000;             // the most characters a send carries (Bloom takes no more)
constexpr UINT_PTR kTickTimer = 1, kLeaveTimer = 2, kRegionTimer = 3, kMorphTimer = 4, kEdgeTimer = 5, kPageTimer = 6;
constexpr UINT kPageMs = 320;     // a question picked, the card turns to the next one this much later (the pick seen)
constexpr UINT kEdgeMs = 120;      // what lies along the edges is read this often while there are cards (they follow it)
constexpr int kEdgeRescan = 4;     // ... and its windows are looked for again every so many reads
// A window of them destroyed (PillTaskbar builds its island window anew on most of its setting changes, Bloom's theme
// among them): they are looked for at every read until as many are there again, this long at most, the cards keeping
// clear of what it covered meanwhile.
constexpr double kEdgeBackMs = 3000;
constexpr UINT kTickMs = 1000;    // the clocks move by the second
constexpr int kStatusEvery = 2;   // status files are looked at every second tick
constexpr UINT kLeaveMs = 300;    // a card the pointer left folds this much later
constexpr int kMorphMs = int(rc::kOpenMs);  // a card opening, the stack moving
constexpr int kFoldMs = int(rc::kFoldMs);   // a card folding
constexpr int kFadeMs = 160;      // a card coming or going
constexpr double kOrphanMs = 180000;  // an open turn with no session file and no line for this long has ended
constexpr double kStopWaitMs = 5000;  // a Stop pressed waits this long for the turn to end before it can be pressed again
constexpr float kGap = 8;         // between stacked cards
constexpr double kRingTurnMs = 1000;
constexpr double kFrameMs = 1000.0 / 60;  // the ring turns at most 60 times a second
constexpr int kMaxCards = 6;
// A session Bloom said takes no words is asked again this long after its card came (T3 Code may not have written the
// thread yet when first asked), then no more until its next turn: each ask is a read of T3 Code's state.
constexpr double kCanAgainMs[] = {2000, 10000};
// A T3 session's question Bloom had no choices for is asked for again this long after the first ask: T3 Code may put
// the question up a moment after the transcript says the session waits.
constexpr double kQuestionAgainMs[] = {1500, 5000, 15000};
constexpr int kRescanTicks = 300;  // the Claude folders are looked for again every five minutes
// Bloom's `kin` (a session's parent and sub-sessions in T3 Code): asked when a card comes and this often while it is
// there; a new card waits this long for the answer (a sub-session's gets none of its own), once Bloom answers `kin`.
constexpr double kKinEveryMs = 3000;
constexpr double kKinWaitMs = 800;

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
        rc::Box from;         // where it was before its latest move (the region covers the way between)
        rc::Size clipTo;      // the size its clip shows or is animating to
        bool placed = false;  // has been put somewhere once
        std::string key;      // what the picture was drawn from
        double foldAt = 0;    // a folding card is drawn as a pill from then on (Tick, as every motion and wait here)
        wuc::ContainerVisual root{nullptr};
        wuc::SpriteVisual sprite{nullptr};
        wuc::CompositionDrawingSurface surf{nullptr};
        wuc::CompositionRoundedRectangleGeometry clip{nullptr};
        wuc::ShapeVisual ring{nullptr};
        wuc::CompositionEllipseGeometry ringGeom{nullptr};
        wuc::CompositionColorBrush ringBrush{nullptr};
        wuc::SpriteVisual glass{nullptr};  // the blur behind the card (Bloom's glass), under its picture
        bool spinning = false;
        double stopAt = 0;  // Stop was pressed then: the button waits a while (a second Esc opens Claude Code's rewind)
        // Its changes (rc::Motion): when each began (Tick) and the card (and Bloom's request) it began from.
        std::optional<rc::Ask> ask;  // Bloom's request as last laid out
        rc::Card titleWas, rowsWas, buttonsWas;
        std::optional<rc::Ask> titleAsk, buttonsAsk;
        double openAt = 0, foldStart = 0, titleAt = 0, rowsAt = 0, buttonsAt = 0, dotAt = 0, checkAt = 0, sizeAt = 0;
        // 보내기's own change (rc::Motion::say): the row opened by the button (sayIntent +1) or closed unsent (-1) since
        // the last change, and the row it began from.
        rc::Card sayWas;
        double sayAt = 0;
        float sayTurn = -1;  // ... it began turned back halfway, at this much of its way, from these fades (rc::SayFades)
        rc::SayFade sayTurnWas;
        bool sayBack = false;
        int sayIntent = 0;
        float wasW = 0;          // a pill's width before its words changed it
        float pillW = 0;         // the pill it opened from or folds into
        bool animating = false;  // drawn every frame until its changes are over
        // The light over a working headline: its words drawn bright, shown through a band sweeping across.
        wuc::SpriteVisual shine{nullptr};
        wuc::CompositionSurfaceBrush shineWords{nullptr};
        wuc::CompositionDrawingSurface shineSurf{nullptr};
        wuc::CompositionLinearGradientBrush band{nullptr};
        std::string shineKey;
        bool shining = false;
        // 보내기 (a T3 session's card): Bloom's word on it, its row to type in, and what came of a send.
        bool canSay = false;     // Bloom said the session takes words
        bool canStop = false;    // ... and a stop (T3 Code interrupts the thread's turn)
        bool canAsked = false;   // ... asked over this connection, for this turn
        rc::Ms canTurn = 0;      // the turn it was asked for
        int canAgain = 0;        // asked again since it came (kCanAgainMs)
        double madeAt = 0;       // when its card came
        bool markHeld = false;   // opened from its mark alone, the pointer still on the mark: not the close button yet
        bool composing = false;  // its row is the box to type in
        bool askOpened = false;  // the row was opened for the question the session waits on (once a question)
        std::string askFor;      // ... the session asking: the card's own, or a sub-session of it (the answer goes there)
        double askAt = 0;        // ... when Bloom was first asked for its choices
        int askAgain = 0;        // ... asked again since, while none came (kQuestionAgainMs)
        double kidsAt = 0;       // when a sub-session of it was last seen at work (Now: the card stays doneSeconds past it)
        std::wstring draft;      // the words typed, while the edit box is elsewhere
        std::string draftFor;    // ... the session the row answered when they were begun (empty: no words); they go there
        bool draftAsk = false;   // ... and it was open for that session's question then (rc::SayTarget)
        std::wstring sent;       // the words gone to Bloom: back in the box when they did not go
        std::string sentFor;     // ... and their draftFor, draftAsk, back with them
        bool sentAsk = false;
        std::wstring note;       // what came of the send, shown a while
        bool noteBad = false;
        bool sending = false;
        double noteAt = 0;
    };

    Config cfg_;
    rc::Palette look_{};
    bool glass_ = false;  // the cards are glass, as Bloom's windows are now
    uint64_t lookId_ = 0;
    FILETIME bloomSeen_{};
    std::vector<Folder> folders_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::map<std::string, rc::Ask> asks_;  // Bloom's requests, by session
    std::map<std::string, rc::Ms> dismissed_;  // cards closed by hand: the session and its turn's start
    std::set<std::string> bloomRuns_;          // sessions seen working in Bloom's own folder
    // Bloom's `kin` answers by session (a T3 session's parent and sub-sessions), and when each session was last asked.
    std::map<std::string, rc::Kin> kin_;
    std::map<std::string, double> kinAsked_;  // (Tick)
    // The sessions a card may be for (Update's, as it last read them): a session's parent and card are looked for
    // among these, by Update and by Bloom's lines alike.
    std::set<std::string> ids_;
    std::map<std::string, rc::SubNumbers> subNos_;  // each card's subs' numbers for its turn ("[S1]"), by session
    bool kinHeard_ = false;  // this Bloom answers `kin`: a new card waits a moment for its answer
    uint64_t arrivals_ = 0;
    int ticks_ = 0;
    bool hidden_ = false;

    HWND hwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    RECT work_{};
    float scale_ = 1;  // the monitor's scale × card.size
    rc::Box area_;  // the work area in the window, in the cards' DIPs (scale_)
    rc::Box mon_;   // the monitor, in the same units
    rc::Box room_;  // the part of the area the cards keep to: clear of what lies along the edges (rc::Clear)
    std::vector<rc::Box> edges_;  // what lies along the monitor's edges now (Obstacles)
    std::vector<HWND> edgeWindows_;  // their windows
    int edgeScan_ = 0;
    double edgeBackUntil_ = 0;  // a window of them destroyed: looked for until then (kEdgeBackMs)
    size_t edgeWant_ = 0;       // ... until there are as many as this again
    bool edgesStale_ = true;    // not read while there were no cards
    bool tracking_ = false;
    std::optional<std::pair<std::string, rc::Act>> press_;
    // A question just picked, the card to turn on from it (kPageTimer): the card, the request, the question.
    struct Turn {
        std::string card, ask;
        int from = 0;
    };
    std::optional<Turn> turn_;
    rc::Link link_;
    // The edit box 보내기 types in: a window of its own (the cards' window takes no keys, nor the IME), over one
    // card's row while it has the keys.
    HWND edit_ = nullptr;
    WNDPROC editProc_ = nullptr;
    std::string editFor_;    // the session whose row it is over
    HWND before_ = nullptr;  // the window that had the keys before it
    HFONT editFont_ = nullptr;
    float editScale_ = 0;
    int editLineH_ = 0;
    RECT editAt_{};
    HBRUSH editBrush_ = nullptr;
    uint32_t editBg_ = 0;

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
    wuc::CompositionBackdropBrush backdrop_{nullptr};  // what is behind the window, blurred (every glass shares it)
    wuc::CompositionEffectBrush frost_{nullptr};       // backdrop_ with the theme's luminosity layer (rc_glass.h)
    bool frostLight_ = false;                          // the theme frost_ was made for
    float frostAmount_ = -1;                           // ... and its luminosity layer's amount (rc::glass::Amount)
    bool plainGlass_ = false;                          // the effect failed: the glass is backdrop_ alone

    // ---- helpers ----
    void ReadLook(bool force);
    void Frost(bool light, float amount);
    wuc::CompositionBrush GlassBrush() const;
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
    void Shine(Slot& s);
    void Changes(Slot& s, const rc::Card& next, const rc::Ask* ask, double now);
    rc::Motion MotionOf(const Slot& s, double now) const;
    bool Moving(const Slot& s, double now) const;
    double SayEnd(const Slot& s) const;
    void AnimateClip(Slot& s, rc::Size from, float fromR, rc::Size to, float toR, double ms, const float (&ease)[4]);
    void FadeOut(Slot& s);
    void SetRegion(bool moving);
    void ArmFrames();
    std::string KeyOf(const Slot& s, rc::View view) const;
    Slot* Find(const std::string& id);
    std::string CardFor(const std::string& session) const;
    std::wstring SubWho(const std::string& session) const;
    Slot* At(POINT client, rc::Hit* hit);
    void OnMouse(UINT msg, LPARAM lParam);
    void OnLeave();
    void Act(Slot& s, const rc::Hit& hit);
    void PageTurn();
    void Open(Slot& s);
    void StopTurn(Slot& s);
    void NotStopped(Slot& s, const std::wstring& why);
    void Dismiss(Slot& s);
    void Answer(Slot& s, const std::string& json);
    bool WantsOpen(const Slot& s) const;
    void SayState(Slot& s, rc::Card& c, double now);
    void SyncSay(Slot& s);
    void Compose(Slot& s);
    void Attach(Slot& s);
    void Detach(bool giveBack);
    void PlaceEdit();
    std::wstring EditText() const;
    void SendSay(Slot& s);
    void Said(Slot& s, bool ok, const std::wstring& word);
    void OnEditKey(WPARAM key);
    void OnEditBlur();
    void OnEditChange();
    LRESULT EditColors(HDC dc);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
    void OnLink(WPARAM what, std::string* line);
    void OnSnapshot(size_t folder, std::shared_ptr<const si::Snapshot> s);
    void OnSettings();
    void CheckFullscreen();
    void FindEdgeWindows();
    std::vector<rc::Box> Obstacles();
    std::vector<rc::Box> EdgesNow();
    void CheckEdges();
    bool DeviceLost(HRESULT hr) const {
        return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
    }
    void Recover();
    // The transcripts' clock (a turn's and a step's times, the clocks drawn), and the one the motions and waits go by.
    double Now() const { return NowMs(); }
    double Tick() const { return TickMs(); }
    // The margin is the screen's (DIPs), whatever the cards' size.
    float Margin() const { return cfg_.margin * 100.0f / float(cfg_.size); }
    // Every pill's width with pill.width = same: a card's, or in a row a shorter one, so folding a card narrows the
    // row; 0 with fit.
    float PillW() const {
        return cfg_.fold != Config::Fold::Same ? 0 : rc::Across(cfg_.anchor, cfg_.row) ? rc::kRowPillW : rc::kCardW;
    }
    // The folded shape the settings ask for, to the painter.
    void FoldShape() {
        painter_->SamePill(PillW());
        painter_->IconPill(cfg_.fold == Config::Fold::Icon);
    }
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
    if (FAILED(CreateDispatcherQueueController(options, winrt::put_abi(queue_)))) {
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
    // The glass: a host backdrop brush draws what is behind the window only once the window allows it.
    DwmSetWindowAttribute(hwnd_, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));

    comp_ = wuc::Compositor();
    check_hresult(
        comp_.as<interop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd_, TRUE, winrt::put_abi(target_)));
    check_hresult(comp_.as<interop::ICompositorInterop>()->CreateGraphicsDevice(device_.get(), winrt::put_abi(graphics_)));
    rootVisual_ = comp_.CreateContainerVisual();
    target_.Root(rootVisual_);
    try {
        backdrop_ = comp_.CreateHostBackdropBrush();
    } catch (...) {
        Wh_Log(L"no host backdrop: the cards stay solid");
    }
    // A card moving to its new place in the stack glides there.
    auto move = comp_.CreateVector3KeyFrameAnimation();
    move.InsertExpressionKeyFrame(1, L"this.FinalValue",
                                  comp_.CreateCubicBezierEasingFunction({rc::kEase[0], rc::kEase[1]}, {rc::kEase[2], rc::kEase[3]}));
    move.Duration(std::chrono::milliseconds(kMorphMs));
    move.Target(L"Offset");
    moves_ = comp_.CreateImplicitAnimationCollection();
    moves_.Insert(L"Offset", move);

    frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameTimer_) frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    cfg_ = ReadConfig();
    FoldShape();
    ReadLook(true);
    Place();
    SetRegion(false);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    StartReaders();
    ReadStatus();
    link_.Start(hwnd_, kLinkMsg);
    SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
    SetTimer(hwnd_, kEdgeTimer, kEdgeMs, nullptr);
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
        frost_ = nullptr;
        backdrop_ = nullptr;
        if (target_) target_.Root(nullptr);
        target_ = nullptr;
        graphics_ = nullptr;
        comp_ = nullptr;
    });
    if (edit_) DestroyWindow(edit_);
    edit_ = nullptr;
    if (hwnd_) {
        g_window = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (editFont_) DeleteObject(editFont_);
    if (editBrush_) DeleteObject(editBrush_);
    editFont_ = nullptr;
    editBrush_ = nullptr;
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
    BloomPrefs b = ReadBloom();
    bool light = cfg_.theme == ThemeChoice::Light || (cfg_.theme == ThemeChoice::System && SystemLight());
    // Glass as Bloom's own windows are (Theme.Acrylic): 유리 효과 on, Windows' transparency on, Windows 11 22H2 on.
    bool glass = backdrop_ && b.transparency && SystemTransparency() && WindowsBuild() >= 22621;
    if (glass) Frost(light, rc::glass::Amount(b.glassTransparency));
    // Without the luminosity layer only a thin see-through keeps the text on any window behind.
    int see = !glass ? -1 : plainGlass_ ? std::min(b.glassTransparency, 13) : b.glassTransparency;
    rc::Palette p = rc::MakePalette(light, light ? b.accent.light : b.accent.dark, see);
    if (!force && glass == glass_ && memcmp(&p, &look_, sizeof(p)) == 0) return;
    look_ = p;
    glass_ = glass;
    lookId_++;
    auto brush = GlassBrush();
    for (auto& s : slots_)
        if (s->glass) {
            s->glass.Brush(brush);
            s->glass.IsVisible(glass_);
        }
}

// The glass brush for the theme and the see-through, made again only when either changes. If composition can't run
// the effect, the glass stays the plain backdrop from then on.
void App::Frost(bool light, float amount) {
    if (plainGlass_ || (frost_ && frostLight_ == light && frostAmount_ == amount)) return;
    try {
        if (TestMode() && Env(L"RUN_CARD_GLASS") == L"plain") winrt::throw_hresult(E_NOTIMPL);
        frost_ = rc::glass::Make(comp_, backdrop_, light, amount);
        frostLight_ = light;
        frostAmount_ = amount;
    } catch (...) {
        frost_ = nullptr;
        plainGlass_ = true;
        Wh_Log(L"no luminosity layer (0x%08X): plain glass, see-through at most 13", unsigned(winrt::to_hresult()));
    }
}

wuc::CompositionBrush App::GlassBrush() const {
    if (frost_) return frost_;
    return backdrop_;
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
    HMONITOR was = monitor_;
    RECT workWas = work_;
    monitor_ = rc::MonitorByNumber(cfg_.monitor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor_, &mi);
    work_ = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy);
    float scale = dx / 96.0f * float(cfg_.size) / 100.0f;
    bool rescaled = scale != scale_;
    scale_ = scale;
    int w = work_.right - work_.left, h = work_.bottom - work_.top;
    area_ = {0, 0, w / scale_, h / scale_};
    const RECT& m = mi.rcMonitor;
    mon_ = {(m.left - work_.left) / scale_, (m.top - work_.top) / scale_, (m.right - m.left) / scale_, (m.bottom - m.top) / scale_};
    edgeScan_ = 0;  // the windows looked for again at the next read
    edgesStale_ = true;
    // Kept clear of while a window is looked for (EdgesNow), in the window's units: none once they change.
    if (monitor_ != was || rescaled || !EqualRect(&work_, &workWas)) edges_.clear();
    room_ = area_;
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

// The session whose card shows a session's requests and questions: its own, or the one it is a sub-session of (among
// the sessions Update took, as its cards have them).
std::string App::CardFor(const std::string& session) const {
    return rc::CardFor(session, kin_, ids_);
}

// The line over a sub-session's request or question on its parent's card: "하위 · <its thread's title>".
std::wstring App::SubWho(const std::string& session) const {
    std::wstring title;
    for (auto& [parent, k] : kin_)
        for (const rc::Kid& kid : k.kids)
            if (kid.session == session && title.empty()) title = kid.title;
    for (auto& f : folders_)
        if (f.snap && title.empty())
            for (const si::SessionView& v : f.snap->sessions)
                if (v.id == session) title = v.title.empty() ? v.project : v.title;
    return rc::Tr(g_korean, L"하위", L"subtask") + (title.empty() ? L"" : L" \x00B7 " + title);
}

void App::Update() {
    if (!comp_) return;
    double now = Now(), tick = Tick();
    bool ko = g_korean;
    // The sessions a card may be for: each one read here with a turn (the first folder's), but `claude -p` runs and
    // programs' sessions, and Bloom's own chats (Bloom shows them itself).
    std::map<std::string, const si::SessionView*> views;
    for (auto& f : folders_) {
        if (!f.snap) continue;
        for (const si::SessionView& v : f.snap->sessions) {
            if (!v.map.turnStartedAt || views.count(v.id)) continue;
            const Status* st = StatusOf(v.id);
            if ((st && !st->interactive) || bloomRuns_.count(v.id)) continue;
            views[v.id] = &v;
        }
    }
    std::set<std::string> ids;
    for (auto& [id, v] : views) ids.insert(id);
    ids_ = ids;
    for (auto it = kin_.begin(); it != kin_.end();) it = ids.count(it->first) ? std::next(it) : kin_.erase(it);
    for (auto it = kinAsked_.begin(); it != kinAsked_.end();) it = ids.count(it->first) ? std::next(it) : kinAsked_.erase(it);
    for (auto it = subNos_.begin(); it != subNos_.end();) it = ids.count(it->first) ? std::next(it) : subNos_.erase(it);
    // The sessions worth a card: one whose turn runs, or ended a little while ago, or whose sub-sessions still work.
    // A sub-session (its line of parents read here) has none of its own: its steps are on the card of the session the
    // line starts from (CardFor), with its parent's and theirs.
    struct Seen {
        rc::Card card;
        DWORD pid;
        std::string subAsker;  // a sub-session of it waits on a question (its transcript says so)
        std::wstring subWho;   // ... the line saying whose
    };
    std::map<std::string, Seen> seen;
    std::vector<std::string> kinFor;  // the sessions Bloom is asked `kin` of now
    std::set<std::string> waiting;    // new cards waiting for Bloom's `kin`
    for (auto& [vid, vp] : views) {
        {
            const si::SessionView& v = *vp;
            if (rc::CardFor(v.id, kin_, ids) != v.id) {
                kinFor.push_back(v.id);
                continue;
            }
            const Status* st = StatusOf(v.id);
            rc::Card c = rc::CardOf(v, ko, rc::Ms(now));
            // Its subagents end with its Claude Code (gone, or no session file and nothing written for minutes).
            if ((st && !st->alive) || (!st && now - double(v.lastActivity) > kOrphanMs)) c.agentsRunning = 0;
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
            // Its sub-sessions' work among its steps (theirs too); the first of them at work waiting on a question, for
            // its row.
            std::vector<rc::Child> kids = rc::ChildrenOf(v.id, kin_, views);
            rc::TakeIn(c, kids, ko);
            rc::WorkOn(c);
            rc::NumberSubs(c, subNos_[v.id]);
            std::string subAsker;
            std::wstring subWho;
            for (const rc::Child& ch : kids) {
                kinFor.push_back(ch.session);
                if (!subAsker.empty() || !ch.working || !ch.view || ch.view->map.turnEndedAt) continue;
                rc::Card cc = rc::CardOf(*ch.view, ko);
                if (cc.asking && !cc.askingPlan) {
                    subAsker = ch.session;
                    subWho = SubWho(ch.session);
                }
            }
            // Closed by hand: stays closed for the rest of that turn, its end included; the next turn shows again.
            if (auto d = dismissed_.find(v.id); d != dismissed_.end()) {
                if (c.turnStart == d->second) continue;
                dismissed_.erase(d);
            }
            Slot* slot = Find(v.id);
            if (slot && rc::SubsAtWork(c) > 0) slot->kidsAt = now;
            // A sub with no transcript here (Codex, Gemini) leaves no step that ends: the time its card last saw subs at
            // work in this turn counts as when they ended (the clock, the time the card says it all took).
            if (slot && c.turnEnd && slot->kidsAt >= double(c.turnStart))
                c.subsEnd = std::max(c.subsEnd, rc::Ms(slot->kidsAt));
            bool held = slot && slot->hover;
            if (!held && !rc::Lingers(c, rc::Ms(now), cfg_.doneSeconds * 1000.0, slot ? rc::Ms(slot->kidsAt) : 0)) continue;
            // card.subtasks off: the main session's steps only (after Lingers, which goes by the subs' last steps too).
            if (!cfg_.subtasks) rc::HideSubs(c);
            kinFor.push_back(v.id);
            // A new card waits a moment for Bloom to say whether the session is a sub-session (then it gets none).
            if (!slot && link_.Up() && kinHeard_ && !kin_.count(v.id)) {
                auto asked = kinAsked_.find(v.id);
                if (asked == kinAsked_.end() || tick - asked->second < kKinWaitMs) waiting.insert(v.id);
            }
            seen[v.id] = {std::move(c), st ? st->pid : 0, std::move(subAsker), std::move(subWho)};
        }
    }
    // Bloom is asked `kin` of each card's session (and its sub-sessions) when it comes, then every few seconds.
    if (link_.Up())
        for (const std::string& id : kinFor) {
            if (id.empty() || !ids.count(id)) continue;
            auto at = kinAsked_.find(id);
            if (at != kinAsked_.end() && tick - at->second < kKinEveryMs) continue;
            if (link_.Send(rc::SessionLine("kin", id))) kinAsked_[id] = tick;
        }
    for (const std::string& id : waiting) seen.erase(id);
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
            made->madeAt = tick;
            s = made.get();
            MakeVisuals(*s);
            slots_.push_back(std::move(made));
        }
        rc::Card next = std::move(sn.card);
        s->pid = sn.pid;
        // A question the session waits on (its transcript says so), else one a sub-session of it waits on: a T3
        // session's is shown with its choices (Bloom's `question`) while it lasts, and answered in the row to type in,
        // the answer going to the session asking.
        bool own = s->canSay && next.phase == rc::Phase::Ask && next.asking && !next.askingPlan;
        std::string asker = own ? id : s->canSay ? sn.subAsker : std::string();
        bool question = !asker.empty();
        auto a = asks_.find(id);
        if (a != asks_.end() && a->second.inRow && (!question || (a->second.from.empty() ? id : a->second.from) != asker)) {
            asks_.erase(a);
            a = asks_.end();
        }
        // Bloom's request under the session's own id: a subagent's, when the tool asking runs only in subagents.
        if (a != asks_.end() && !a->second.inRow && !a->second.sourced)
            if (auto who = rc::SubagentAsking(views[id]->map, a->second.tool, ko)) {
                a->second.sub = *who;
                a->second.sourced = true;
            }
        const rc::Ask* ask = a != asks_.end() ? &a->second : nullptr;
        if (ask) next.phase = rc::Phase::Approve;
        // Stop: Bloom sends Esc into the console of a terminal session, and has T3 Code interrupt a T3 session's turn
        // (Bloom's `can` says so); others (the SDK, an editor's panel) read no console, and nothing outside them can
        // stop a turn.
        const Status* st = StatusOf(id);
        next.stoppable = link_.Up() && ((st && st->cli && st->alive) || s->canStop);
        next.stopping = s->stopAt && tick - s->stopAt < kStopWaitMs;
        // 보내기: Bloom is asked whether the session takes words once a connection and turn, and again a few times
        // shortly after the card came while it said no (kCanAgainMs). A question it waits on opens the row to type the
        // answer in, once a question, without taking the keys from anything.
        if (s->canTurn != next.turnStart) {
            s->canTurn = next.turnStart;
            s->canAsked = false;
        }
        if (s->canAsked && !s->canSay && s->canAgain < int(std::size(kCanAgainMs)) &&
            tick - s->madeAt >= kCanAgainMs[s->canAgain]) {
            s->canAgain++;
            s->canAsked = false;
        }
        if (link_.Up() && !s->canAsked) s->canAsked = link_.Send("{\"t\":\"can\",\"session\":" + rc::JsonString(id) + "}");
        bool inRow = question && (!ask || ask->inRow);
        if (inRow && (!s->askOpened || s->askFor != asker)) {
            s->askOpened = true;
            s->askFor = asker;
            s->askAt = tick;
            s->askAgain = 0;
            s->composing = true;
            link_.Send(rc::SessionLine("question", asker));
        } else if (inRow && !ask && link_.Up() && s->askAgain < int(std::size(kQuestionAgainMs)) &&
                   tick - s->askAt >= kQuestionAgainMs[s->askAgain]) {
            s->askAgain++;
            link_.Send(rc::SessionLine("question", asker));
        } else if (!inRow && s->askOpened) {
            s->askOpened = false;
            s->askFor.clear();
            if (editFor_ != s->id && Blank(s->draft)) s->composing = false;
        }
        next.subAsk = s->askOpened && s->askFor != id ? sn.subWho : std::wstring();
        SayState(*s, next, tick);
        Changes(*s, next, ask, tick);
        s->card = std::move(next);
        s->ask = ask ? std::optional<rc::Ask>(*ask) : std::nullopt;
        s->wantOpen = WantsOpen(*s);
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
                    std::to_string(scale_) + std::to_string(int(PillW())) + "|" + std::to_string(c.filesChanged) + "|" + std::to_string(c.asking) + std::to_string(c.stoppable) +
                    std::to_string(c.stopping) + "|" +
                    Narrow(c.model) + "|" + std::to_string(int(s.ptr.hover)) + "," + std::to_string(s.ptr.q) + "," +
                    std::to_string(s.ptr.choice);
    k += "|say" + std::to_string(c.canSay) + std::to_string(c.composing) + std::to_string(c.focused) + std::to_string(c.typed) +
         std::to_string(c.noteBad) + Narrow(c.draft) + "|" + Narrow(c.note);
    for (const rc::Step& st : c.steps)
        k += "|" + Narrow(st.done) + (st.endedAt ? std::to_string(*st.endedAt) : "r") + (st.sub ? "s" + std::to_string(st.subNo) : "");
    k += "|kids" + std::to_string(c.kidsRunning) + "," + std::to_string(c.agentsRunning) + std::to_string(cfg_.subtasks) +
         Narrow(c.subAsk);
    // A clock that moves (the turn runs, or its subs work on) is part of the picture.
    if (!c.turnEnd || rc::SubsAtWork(c) > 0) k += "|t" + std::to_string(int64_t(Now() / 1000));
    else k += "|e" + std::to_string(std::max(*c.turnEnd, c.subsEnd));
    auto a = asks_.find(s.id);
    if (a != asks_.end()) {
        k += "|ask" + a->second.id + Narrow(a->second.sub) + "|p" + std::to_string(a->second.page);
        for (auto& p : a->second.picks)
            for (int i : p) k += "," + std::to_string(i);
    }
    return k;
}

void App::Layout() {
    double now = Now(), tick = Tick();
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
    // Clear of what lies along the monitor's edges (PillTaskbar's islands, a taskbar slid out).
    if (edgesStale_) {
        edges_ = EdgesNow();
        edgesStale_ = false;
    }
    // A stack, or a row at a top or bottom place.
    bool across = rc::Across(cfg_.anchor, cfg_.row);
    auto lay = [&](rc::Box a) {
        return across ? rc::Row(sizes, cfg_.anchor, a, Margin(), kGap) : rc::Stack(sizes, cfg_.anchor, a, Margin(), kGap);
    };
    room_ = rc::Clear(lay, area_, mon_, Margin(), kGap, edges_);
    // Too tall for the monitor (a row: too wide): fold open cards (not one being pointed at or asking), oldest first.
    auto total = [&] {
        float t = 0;
        for (auto& z : sizes) t += (across ? z.w : z.h) + kGap;
        return t - kGap;
    };
    float room = across ? rc::RoomAcross(room_, Margin()) : rc::Room(room_, Margin());
    for (size_t i = 0; i < list.size() && total() > room; i++) {
        Slot* s = list[i];
        if (!s->wantOpen || s->hover || s->card.phase == rc::Phase::Approve) continue;
        s->wantOpen = false;
        sizes[i] = sizeOf(s);
    }
    std::vector<rc::Box> boxes = lay(room_);
    bool moved = false;
    for (size_t i = 0; i < list.size(); i++) {
        Slot& s = *list[i];
        rc::Box b = boxes[i];
        bool morphing = false;  // its clip is being animated from this layout on
        if (s.wantOpen != s.open) {
            morphing = s.placed;
            rc::Size from{s.box.w, s.box.h}, to{b.w, b.h};
            if (s.wantOpen) {
                // Opens: the card drawn now, shown from the pill's size out, its body coming out of a blur.
                s.open = true;
                s.foldAt = 0;
                if (s.placed) {
                    s.openAt = tick;
                    s.foldStart = 0;
                    s.pillW = from.w;
                    s.animating = true;
                }
                Draw(s, rc::View::Card);
                if (s.placed) AnimateClip(s, from, rc::kPillH / 2, to, rc::kRadius, rc::kOpenMs, rc::kEase);
            } else {
                // Folds: the card stays drawn while its clip closes to the pill (its body going back into the
                // blur), then the pill is drawn.
                s.open = false;
                if (s.placed) {
                    AnimateClip(s, from, rc::kRadius, to, rc::kPillH / 2, rc::kFoldMs, rc::kEase);
                    s.foldAt = tick + kFoldMs;
                    s.foldStart = tick;
                    s.pillW = to.w;
                    s.animating = true;
                    SetTimer(hwnd_, kMorphTimer, kFoldMs + 20, nullptr);
                } else {
                    Draw(s, rc::View::Pill);
                }
            }
        }
        rc::View view = s.open ? rc::View::Card : s.foldAt > tick ? rc::View::Card : rc::View::Pill;
        // A card at rest that grew or shrank in place (a step came in, a request, a pill's words) goes to its new
        // size with its clip, and a pill's picture with it; one never shown whole yet is shown whole at once.
        bool resized = s.placed && !morphing && (s.open || s.foldAt <= tick) && (b.w != s.clipTo.w || b.h != s.clipTo.h);
        if (resized) {
            float r = s.open ? rc::kRadius : rc::kPillH / 2;
            if (s.clipTo.w > 0 && !hidden_) {
                if (!s.open) {
                    s.wasW = s.clipTo.w;
                    s.sizeAt = tick;
                    s.animating = true;
                }
                AnimateClip(s, s.clipTo, r, {b.w, b.h}, r, rc::kRowsMs, rc::kEase);
            } else {
                s.clip.StopAnimation(L"Size");
                s.clip.StopAnimation(L"CornerRadius");
                s.clip.Size({b.w * scale_, b.h * scale_});
                s.clip.CornerRadius({r * scale_, r * scale_});
                s.clipTo = {b.w, b.h};
            }
        }
        if (resized || KeyOf(s, view) != s.key) Draw(s, view);
        bool newPlace = !s.placed || s.box.x != b.x || s.box.y != b.y || s.box.w != b.w || s.box.h != b.h;
        moved = moved || newPlace;
        s.from = s.placed && newPlace ? s.box : b;
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
    PlaceEdit();
}

void App::MakeVisuals(Slot& s) {
    s.root = comp_.CreateContainerVisual();
    s.root.Opacity(0);
    s.clip = comp_.CreateRoundedRectangleGeometry();
    s.root.Clip(comp_.CreateGeometricClip(s.clip));
    s.surf = graphics_.CreateDrawingSurface({1, 1}, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                            wgd::DirectXAlphaMode::Premultiplied);
    if (backdrop_) {
        s.glass = comp_.CreateSpriteVisual();
        s.glass.Brush(GlassBrush());
        s.glass.IsVisible(glass_);
        s.root.Children().InsertAtBottom(s.glass);
    }
    s.sprite = comp_.CreateSpriteVisual();
    auto brush = comp_.CreateSurfaceBrush(s.surf);
    brush.Stretch(wuc::CompositionStretch::None);
    brush.HorizontalAlignmentRatio(0);
    brush.VerticalAlignmentRatio(0);
    s.sprite.Brush(brush);
    s.root.Children().InsertAtTop(s.sprite);
    // The light over a working headline (Shine): its words through a band whose stops are those of the design.
    s.shineSurf = graphics_.CreateDrawingSurface({1, 1}, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                 wgd::DirectXAlphaMode::Premultiplied);
    s.shineWords = comp_.CreateSurfaceBrush(s.shineSurf);
    s.shineWords.Stretch(wuc::CompositionStretch::None);
    s.shineWords.HorizontalAlignmentRatio(0);
    s.shineWords.VerticalAlignmentRatio(0);
    s.band = comp_.CreateLinearGradientBrush();
    s.band.MappingMode(wuc::CompositionMappingMode::Absolute);
    s.band.StartPoint({0, 0});
    s.band.EndPoint({1, 0});
    for (auto [at, alpha] : {std::pair{0.0f, 0}, std::pair{0.357f, 255}, std::pair{0.643f, 255}, std::pair{1.0f, 0}})
        s.band.ColorStops().Append(comp_.CreateColorGradientStop(at, {uint8_t(alpha), 255, 255, 255}));
    auto masked = comp_.CreateMaskBrush();
    masked.Source(s.shineWords);
    masked.Mask(s.band);
    s.shine = comp_.CreateSpriteVisual();
    s.shine.Brush(masked);
    s.shine.IsVisible(false);
    s.root.Children().InsertAtTop(s.shine);
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
    double now = Now(), tick = Tick();
    auto a = asks_.find(s.id);
    const rc::Ask* ask = a != asks_.end() ? &a->second : nullptr;
    rc::Laid L = painter_->Run(nullptr, s.card, ask, view, look_, rc::Ms(now), s.ptr, g_korean);
    // A pill narrowing is drawn wider than it ends while it goes there.
    float lw = view == rc::View::Pill && tick < s.sizeAt + rc::kRowsMs ? std::max(L.w, s.wasW) : L.w;
    int w = std::max(1, int(std::ceil(lw * scale_))), h = std::max(1, int(std::ceil(L.h * scale_)));
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
        rc::Motion m = MotionOf(s, tick);
        s.laid = painter_->Run(dc.get(), s.card, ask, view, look_, rc::Ms(now), s.ptr, g_korean, false, &m);
    } catch (...) {
        Wh_Log(L"paint failed");
    }
    hr = sdi->EndDraw();
    if (DeviceLost(hr)) Recover();
    s.drawn = view;
    s.key = KeyOf(s, view);
    s.sprite.Size({float(w), float(h)});
    if (s.glass) s.glass.Size({float(w), float(h)});  // the clip gives it the card's shape
    s.ring.Size({float(w), float(h)});
    s.ringGeom.Center({s.laid.markAt.x * scale_, s.laid.markAt.y * scale_});
    s.ringGeom.Radius({rc::kRingR * scale_, rc::kRingR * scale_});
    s.ring.CenterPoint({s.laid.markAt.x * scale_, s.laid.markAt.y * scale_, 0});
    s.ringBrush.Color(WinColor(look_.text));
    s.ring.Shapes().GetAt(0).as<wuc::CompositionSpriteShape>().StrokeThickness(rc::kRingW * scale_);
    Shine(s);
}

void App::Spin(Slot& s, bool on) {
    if (on == s.spinning) return;
    s.spinning = on;
    s.ring.IsVisible(on);
}

// The light over a working headline (rc::Laid::shine, drawn in its resting tone): its words drawn bright in a surface
// of their own, shown through a band that sweeps across them. Composition sweeps it; nothing is drawn meanwhile, and
// the sweep stops whenever the light is not shown.
void App::Shine(Slot& s) {
    bool on = s.shine && s.laid.shine && !hidden_ && !s.gone;
    if (!on) {
        if (s.shining) {
            s.shine.IsVisible(false);
            s.band.StopAnimation(L"Offset");
            s.shining = false;
        }
        return;
    }
    const D2D1_RECT_F& r = s.laid.title;
    float x = r.left * scale_, y = r.top * scale_, fx = std::floor(x), fy = std::floor(y);
    float ww = r.right - r.left, hh = r.bottom - r.top;
    int w = std::max(1, int(std::ceil(ww * scale_ + x - fx))), h = std::max(1, int(std::ceil(hh * scale_ + y - fy)));
    std::string key = Narrow(s.laid.titleText) + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" +
                      std::to_string(x) + "," + std::to_string(y) + "|" + std::to_string(lookId_);
    if (key == s.shineKey && s.shining) return;
    if (key != s.shineKey) {
        auto size = s.shineSurf.SizeInt32();
        if (size.Width != w || size.Height != h) s.shineSurf.Resize({w, h});
        auto sdi = s.shineSurf.as<interop::ICompositionDrawingSurfaceInterop>();
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
        // The words where the card's picture has them: at the same fraction of a pixel.
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) *
                         D2D1::Matrix3x2F::Translation(float(offset.x) + x - fx, float(offset.y) + y - fy));
        try {
            painter_->Words(dc.get(), look_, s.laid.titleText, ww, hh, look_.text);
        } catch (...) {
            Wh_Log(L"shine failed");
        }
        hr = sdi->EndDraw();
        if (DeviceLost(hr)) {
            Recover();
            return;
        }
        s.shineKey = key;
        s.shine.Size({float(w), float(h)});
        s.shine.Offset({fx, fy, 0});
    }
    // The band, 0.8 of the words wide, crosses them in three quarters of a sweep and is away for the rest.
    float band = std::max(48.0f, 0.8f * float(w));
    s.band.EndPoint({band, 0});
    auto linear = comp_.CreateLinearEasingFunction();
    auto sweep = comp_.CreateVector2KeyFrameAnimation();
    sweep.InsertKeyFrame(0, {-band, 0}, linear);
    sweep.InsertKeyFrame(0.75f, {float(w), 0}, linear);
    sweep.InsertKeyFrame(1, {float(w), 0}, linear);
    sweep.Duration(std::chrono::milliseconds(int64_t(rc::kShineMs)));
    sweep.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
    s.band.StartAnimation(L"Offset", sweep);
    s.shine.IsVisible(true);
    s.shining = true;
}

// What a new state of a card starts moving (rc::Motion), each change from the card as it was until now. A running
// step whose words only count on ("Reading 3 files", 4 files) changes its headline in place.
void App::Changes(Slot& s, const rc::Card& next, const rc::Ask* ask, double now) {
    int intent = std::exchange(s.sayIntent, 0);
    if (!s.placed || s.gone) return;
    const rc::Card& was = s.card;
    const rc::Ask* wasAsk = s.ask ? &*s.ask : nullptr;
    bool moved = false;
    bool sameStep = next.steps.size() == was.steps.size() && next.phase == was.phase && next.asking == was.asking &&
                    (ask != nullptr) == (wasAsk != nullptr);
    if (!sameStep && painter_->TitleOf(was, wasAsk, g_korean) != painter_->TitleOf(next, ask, g_korean)) {
        s.titleWas = was;
        s.titleAsk = s.ask;
        s.titleAt = now;
        moved = true;
    }
    auto over = [](const rc::Card& c) { return c.phase == rc::Phase::Done || c.phase == rc::Phase::Stopped; };
    if (next.steps.size() >= was.steps.size() && (next.steps.size() > was.steps.size() ||
                                                  (rc::Current(was) && !rc::Current(next)) || over(next) != over(was))) {
        s.rowsWas = was;
        s.rowsAt = now;
        moved = true;
    }
    // 보내기 opening the row to type in, or the row closed unsent going back into it: the button's own change (none
    // with Windows' animation effects off); any other change of the buttons (a send, a question opening the row) the
    // buttons' cross-blur.
    int wasButtons = rc::Painter::ButtonsOf(was, wasAsk), nextButtons = rc::Painter::ButtonsOf(next, ask);
    constexpr int kTyping = 4, kReply = 8;  // ButtonsOf: the row to type in; 보내기 among the buttons
    bool sayOpens = intent > 0 && (wasButtons & kReply) && nextButtons == kTyping;
    bool sayFolds = intent < 0 && wasButtons == kTyping && (nextButtons & kReply);
    if (sayOpens || sayFolds) {
        s.buttonsAt = 0;
        if (Animations()) {
            // Turned back halfway: from where the shape is, the fades going on from where they are (rc::SayTurn).
            double from = 0;
            float turn = -1;
            rc::SayFade turnWas;
            if (s.sayAt > 0 && s.sayBack != sayFolds && now < s.sayAt + (s.sayBack ? rc::kSayFoldMs : rc::kSayOpenMs)) {
                float k = float((now - s.sayAt) / (s.sayBack ? rc::kSayFoldMs : rc::kSayOpenMs));
                rc::SayTurned t = rc::SayTurn(s.sayBack, k, s.sayTurn, s.sayTurnWas);
                turn = t.k;
                turnWas = t.was;
                from = t.k * (sayFolds ? rc::kSayFoldMs : rc::kSayOpenMs);
            }
            s.sayTurn = turn;
            s.sayTurnWas = turnWas;
            s.sayWas = was;
            s.sayBack = sayFolds;
            s.sayAt = now - from;
            moved = true;
        } else {
            s.sayAt = 0;
        }
    } else if (wasButtons != nextButtons) {
        s.sayAt = 0;
        s.buttonsWas = was;
        s.buttonsAsk = s.ask;
        s.buttonsAt = now;
        moved = true;
    }
    if (!rc::Painter::Dotted(was, wasAsk) && rc::Painter::Dotted(next, ask)) {
        s.dotAt = now;
        moved = true;
    }
    if (was.phase != rc::Phase::Done && next.phase == rc::Phase::Done) {
        s.checkAt = now;
        moved = true;
    }
    if (moved) s.animating = true;
}

rc::Motion App::MotionOf(const Slot& s, double now) const {
    auto k = [&](double at, double ms) { return at > 0 ? float(std::clamp((now - at) / ms, 0.0, 1.0)) : 1.0f; };
    rc::Motion m;
    m.open = s.foldStart > s.openAt ? 1 - rc::Bezier(rc::kEase, k(s.foldStart, rc::kFoldMs))
                                    : rc::Bezier(rc::kEase, k(s.openAt, rc::kOpenMs));
    m.title = k(s.titleAt, rc::kTitleMs);
    m.titleWas = &s.titleWas;
    m.titleAsk = s.titleAsk ? &*s.titleAsk : nullptr;
    m.rows = k(s.rowsAt, rc::kRowsMs);
    m.rowsWas = &s.rowsWas;
    m.buttons = k(s.buttonsAt, rc::kButtonsMs);
    m.buttonsWas = &s.buttonsWas;
    m.buttonsAsk = s.buttonsAsk ? &*s.buttonsAsk : nullptr;
    m.say = k(s.sayAt, s.sayBack ? rc::kSayFoldMs : rc::kSayOpenMs);
    m.sayBack = s.sayBack;
    m.sayWas = &s.sayWas;
    m.sayTurn = s.sayTurn;
    m.sayTurnWas = s.sayTurnWas;
    m.dot = k(s.dotAt, rc::kDotMs);
    m.check = k(s.checkAt, rc::kCheckMs);
    m.size = k(s.sizeAt, rc::kRowsMs);
    m.wasW = s.wasW;
    m.pillW = s.pillW;
    m.shine = true;
    return m;
}

bool App::Moving(const Slot& s, double now) const {
    return now < std::max({s.openAt + rc::kOpenMs, s.foldStart + rc::kFoldMs, s.titleAt + rc::kTitleMs,
                           s.rowsAt + rc::kRowsMs, s.buttonsAt + rc::kButtonsMs, s.dotAt + rc::kDotMs,
                           s.checkAt + rc::kCheckMs, s.sizeAt + rc::kRowsMs, SayEnd(s)});
}

// When 보내기's own change ends (0: none began).
double App::SayEnd(const Slot& s) const {
    return s.sayAt > 0 ? s.sayAt + (s.sayBack ? rc::kSayFoldMs : rc::kSayOpenMs) : 0;
}

void App::AnimateClip(Slot& s, rc::Size from, float fromR, rc::Size to, float toR, double ms, const float (&e)[4]) {
    auto ease = comp_.CreateCubicBezierEasingFunction({e[0], e[1]}, {e[2], e[3]});
    auto size = comp_.CreateVector2KeyFrameAnimation();
    size.InsertKeyFrame(0, {from.w * scale_, from.h * scale_});
    size.InsertKeyFrame(1, {to.w * scale_, to.h * scale_}, ease);
    size.Duration(std::chrono::milliseconds(int64_t(ms)));
    s.clip.StartAnimation(L"Size", size);
    s.clipTo = to;
    auto radius = comp_.CreateVector2KeyFrameAnimation();
    radius.InsertKeyFrame(0, {fromR * scale_, fromR * scale_});
    radius.InsertKeyFrame(1, {toR * scale_, toR * scale_}, ease);
    radius.Duration(std::chrono::milliseconds(int64_t(ms)));
    s.clip.StartAnimation(L"CornerRadius", radius);
}

void App::FadeOut(Slot& s) {
    if (s.gone) return;
    if (editFor_ == s.id) Detach(false);
    s.gone = true;
    s.hover = false;
    s.animating = false;
    Spin(s, false);
    if (s.shine) Shine(s);
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
        rc::Box b = s->box;
        if (moving) {
            // A card gliding to its new place shows on the whole way there, not only where it left and lands.
            float x = std::min(b.x, s->from.x), y = std::min(b.y, s->from.y);
            b = {x, y, std::max(b.Right(), s->from.Right()) - x, std::max(b.Bottom(), s->from.Bottom()) - y};
        }
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

// The ring turns while a card works, and a card changing is drawn again: a frame every 1/60 s, none at rest.
void App::ArmFrames() {
    bool any = false;
    for (auto& s : slots_) any = any || s->spinning || s->animating;
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
    double now = Tick();
    float angle = float(std::fmod(now / kRingTurnMs, 1.0) * 360.0);
    bool any = false;
    for (auto& s : slots_) {
        if (s->spinning) s->ring.RotationAngleInDegrees(angle);
        if (s->animating && !s->gone) {
            // Its last frame is the card at rest.
            s->animating = Moving(*s, now);
            Draw(*s, s->open || s->foldAt > now ? rc::View::Card : rc::View::Pill);
        }
        any = any || s->spinning || (s->animating && !s->gone);
    }
    if (!editFor_.empty()) PlaceEdit();  // 보내기's box arrived: its edit box over it (PlaceEdit)
    if (!any) {
        framing_ = false;
        return;
    }
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
            s->shineSurf = graphics_.CreateDrawingSurface({1, 1}, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                          wgd::DirectXAlphaMode::Premultiplied);
            s->shineWords.Surface(s->shineSurf);
            s->shineKey.clear();
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
    FoldShape();
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
        for (auto& s : slots_) s->canAsked = false;  // asked again of this Bloom (Update)
        kinAsked_.clear();
        kinHeard_ = false;
        Update();  // Stop needs Bloom
        return;
    }
    if (what == rc::Link::kDown) {
        asks_.clear();
        kin_.clear();  // no Bloom, nothing known of T3 Code: each session shows its own card
        kinAsked_.clear();
        kinHeard_ = false;
        for (auto& s : slots_) {
            s->canSay = s->canStop = s->canAsked = false;
            if (s->sending) Said(*s, false, L"bloom");
        }
        Update();
        return;
    }
    if (!line) return;
    wdj::JsonObject o{nullptr};
    if (!wdj::JsonObject::TryParse(Widen(*line), o)) return;
    try {
        std::wstring t(o.GetNamedString(L"t", L""));
        // AskUserQuestion's questions as Bloom passes them on (its requests, a T3 session's question).
        auto questions = [&](rc::Ask& a) {
            if (!o.HasKey(L"questions") || o.GetNamedValue(L"questions").ValueType() != wdj::JsonValueType::Array) return;
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
        };
        if (t == L"ask") {
            rc::Ask a;
            a.id = winrt::to_string(o.GetNamedString(L"id", L""));
            std::string session = winrt::to_string(o.GetNamedString(L"session", L""));
            a.tool = std::wstring(o.GetNamedString(L"tool", L""));
            a.body = std::wstring(o.GetNamedString(L"body", L""));
            questions(a);
            // A sub-session's goes on the card of the session it works for, saying whose it is; answered by its id.
            std::string card = CardFor(session);
            if (card != session) {
                a.from = session;
                a.sub = SubWho(session);
                a.sourced = true;
            }
            // Shown only whole (Fits; the card no taller than the room), and one at a time per card: a second one
            // (parallel subagents ask under their session's id) goes to Bloom's card, which shows several at once.
            Slot* s = Find(card);
            bool show = cfg_.approvals && s && !hidden_ && !a.id.empty() && !asks_.count(card) &&
                        painter_->Fits(a, g_korean) &&
                        painter_->AskH(s->card, a, look_, rc::Ms(Now()), g_korean) <= rc::Room(room_, Margin());
            link_.Send(std::string("{\"t\":\"") + (show ? "shown" : "none") + "\",\"id\":" + rc::JsonString(a.id) + "}");
            if (!show) return;
            asks_[card] = std::move(a);
            Update();
        } else if (t == L"question") {
            // A T3 session's question with its choices, when it still waits on it: on its own card, or a sub-session's
            // on the card of the session it works for. No other card shows it, so one too tall for the room takes
            // fewer lines (rc::kRowLines), its longer texts ending in an ellipsis.
            std::string session = winrt::to_string(o.GetNamedString(L"session", L""));
            Slot* s = Find(CardFor(session));
            if (!s || !s->askOpened || s->askFor != session || asks_.count(s->id)) return;
            rc::Ask a;
            a.id = "t3:" + session;
            a.tool = L"AskUserQuestion";
            a.inRow = true;
            a.sourced = true;
            if (session != s->id) {
                a.from = session;
                a.sub = SubWho(session);
            }
            questions(a);
            if (a.questions.empty()) return;
            auto tall = [&] { return painter_->AskH(s->card, a, look_, rc::Ms(Now()), g_korean) > rc::Room(room_, Margin()); };
            while (tall() && a.tight + 1 < int(std::size(rc::kRowLines))) a.tight++;
            if (tall()) return;
            asks_[s->id] = std::move(a);
            Update();
        } else if (t == L"kin") {
            // A session's parent and sub-sessions (si::json reads it: rc::ParseKin, as the test program does).
            auto k = rc::ParseKin(*line);
            if (!k) return;
            kinHeard_ = true;
            std::string session = k->session;
            kin_[session] = std::move(*k);
            Update();
        } else if (t == L"can" || t == L"said" || t == L"stopped") {
            // A send's or a stop's answer may name the sub-session the card's row answered.
            std::string session = winrt::to_string(o.GetNamedString(L"session", L""));
            Slot* s = Find(session);
            if (!s && t != L"can") s = Find(CardFor(session));
            if (!s) return;
            if (t == L"can") {
                s->canSay = o.GetNamedBoolean(L"say", false);
                s->canStop = o.GetNamedBoolean(L"stop", false);
            } else if (t == L"stopped") {
                if (!o.GetNamedBoolean(L"ok", false)) NotStopped(*s, std::wstring(o.GetNamedString(L"why", L"")));
            } else if (s->sending) {
                bool ok = o.GetNamedBoolean(L"ok", false);
                Said(*s, ok, std::wstring(o.GetNamedString(ok ? L"how" : L"why", L"")));
            }
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
    // A card opened from its mark alone (pill.width = icon) may have the pointer right where its mark is: there the
    // mark stays part of the header (a click opens the session) until the pointer has left it once, so that a click
    // meant for the folded mark does not close the card.
    if (s && s->markHeld) {
        if (hit.act == rc::Act::Close) hit.act = rc::Act::Header;
        else s->markHeld = false;
    }
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
                if (!on) k->markHeld = false;
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
            if (s && !s->open && !cfg_.always) {
                s->markHeld = cfg_.fold == Config::Fold::Icon;
                Update();  // opens now
            }
            else if (!s) SetTimer(hwnd_, kLeaveTimer, kLeaveMs, nullptr);
            else Layout();
        }
        return;
    }
    if (msg == WM_LBUTTONDOWN) {
        press_.reset();
        // A click on the card away from the row typed in (its box, its send button) closes the row, as Esc does: the
        // buttons come back, the words are kept for the next 보내기. A row opened for a question stays (OnEditBlur).
        // (보내기 itself, there while its box is still growing out of it, keeps the row.)
        if (s && s->composing && !s->askOpened && hit.act != rc::Act::Field && hit.act != rc::Act::Say &&
            hit.act != rc::Act::Pick && hit.act != rc::Act::Page && hit.act != rc::Act::Reply) {
            if (editFor_ == s->id) Detach(true);
            s->composing = false;
            s->sayIntent = -1;
            SyncSay(*s);
            return;
        }
        // 보내기 and the row's box take the keys as they are pressed, as a text box does: words typed straight after
        // the click are the card's, not the window's before it (by the button's release they would have gone there).
        if (s && (hit.act == rc::Act::Reply || hit.act == rc::Act::Field)) {
            Act(*s, hit);
            return;
        }
        if (s && hit.act != rc::Act::None) press_ = {s->id, hit.act};
        return;
    }
    if (msg == WM_LBUTTONUP) {
        auto was = press_;
        press_.reset();
        if (s && was && was->first == s->id && was->second == hit.act) Act(*s, hit);
        return;
    }
    if (msg == WM_RBUTTONUP && s && (s->card.phase == rc::Phase::Done || s->card.phase == rc::Phase::Stopped)) Dismiss(*s);
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
            if (a != asks_.end() && !a->second.inRow) Answer(s, "\"value\":\"terminal\"");
            else Open(s);
            break;
        case rc::Act::Allow: Answer(s, "\"value\":\"allow\""); break;
        case rc::Act::Reply: Compose(s); break;
        case rc::Act::Field: Attach(s); break;
        case rc::Act::Say:
            if (s.composing) SendSay(s);  // not the send button of a box folding back unsent
            break;
        case rc::Act::Stop: StopTurn(s); break;
        case rc::Act::Close: Dismiss(s); break;
        case rc::Act::Deny: Answer(s, "\"value\":\"deny\""); break;
        case rc::Act::Pick:
            if (a != asks_.end() && hit.q >= 0 && hit.q < int(a->second.questions.size())) {
                auto& picks = a->second.picks[hit.q];
                if (a->second.questions[hit.q].multi) {
                    auto at = std::find(picks.begin(), picks.end(), hit.choice);
                    if (at == picks.end()) picks.push_back(hit.choice);
                    else picks.erase(at);
                } else {
                    // One picked: a moment later the card turns to the next question not answered (PageTurn).
                    picks = {hit.choice};
                    if (a->second.questions.size() > 1) {
                        turn_ = {s.id, a->second.id, hit.q};
                        SetTimer(hwnd_, kPageTimer, kPageMs, nullptr);
                    }
                }
                Layout();
            }
            break;
        case rc::Act::Page:
            if (a != asks_.end() && hit.q >= 0 && hit.q < int(a->second.questions.size())) {
                a->second.page = hit.q;
                turn_.reset();
                KillTimer(hwnd_, kPageTimer);
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

// A moment after a question was picked: the card turns to the next one not answered (after it, else before it), unless
// another page was turned to meanwhile or the request is gone. With every one answered it stays, 보내기 lit.
void App::PageTurn() {
    auto turn = std::move(turn_);
    turn_.reset();
    if (!turn) return;
    auto a = asks_.find(turn->card);
    if (a == asks_.end() || a->second.id != turn->ask || a->second.page != turn->from) return;
    int next = a->second.NextOpen(turn->from);
    if (next < 0) return;
    a->second.page = next;
    Layout();
}

void App::Answer(Slot& s, const std::string& json) {
    auto a = asks_.find(s.id);
    if (a == asks_.end() || a->second.inRow) return;
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

// Closed by hand (its X, or a right click on a finished card): gone for the rest of its turn. A request Bloom passed on
// for it goes back to Bloom's own card (Update: a session not shown).
void App::Dismiss(Slot& s) {
    dismissed_[s.id] = s.card.turnStart;
    FadeOut(s);
    Update();
}

// Stop: Bloom checks the session is the terminal one it says and sends Esc into its console (Claude Code stops the
// turn), or has T3 Code interrupt a T3 session's turn; the card ends with the turn, as for an Esc typed there.
void App::StopTurn(Slot& s) {
    if (!s.card.stoppable || s.card.stopping) return;
    if (!link_.Send("{\"t\":\"stop\",\"session\":" + rc::JsonString(s.id) + ",\"pid\":" + std::to_string(s.pid) + "}")) return;
    s.stopAt = Tick();
    Update();
}

// Bloom could not stop it: Stop can be pressed again, and the row says so a while (where a send's result goes).
void App::NotStopped(Slot& s, const std::wstring& why) {
    bool ko = g_korean;
    s.stopAt = 0;
    s.noteAt = Tick();
    s.noteBad = true;
    s.note = why == L"t3-off" ? rc::Tr(ko, L"T3가 꺼져 있어 중단하지 못했습니다", L"Could not stop because T3 is not running")
                              : rc::Tr(ko, L"중단하지 못했습니다", L"Could not stop");
}

// ---- 보내기 ---------------------------------------------------------------------------------------------------
// A T3 session's card takes words for the session: its 보내기 turns the buttons' row into a box to type in, Enter
// sends the words to Bloom, which puts them into the session's thread (the answer to a question it waits on, else
// into the running turn, else a new one) and says what came of it. The cards' window takes no keys, so the typing is
// done in an edit box of its own over the row.

// A card stays open while it asks, is over, is pointed at, or is being typed into.
bool App::WantsOpen(const Slot& s) const {
    rc::Phase p = s.card.phase;
    bool pinned = p == rc::Phase::Approve || p == rc::Phase::Ask || p == rc::Phase::Done || p == rc::Phase::Stopped;
    return cfg_.always || s.hover || pinned || s.composing;
}

// The card's 보내기 parts from the slot, as Update lays out (and what came of a send, gone after a while).
void App::SayState(Slot& s, rc::Card& c, double now) {
    if (s.sending && now - s.noteAt > kSayWaitMs) Said(s, false, L"failed");
    if (!s.sending && !s.note.empty() && now - s.noteAt > kNoteMs) s.note.clear();
    bool can = s.canSay && link_.Up();
    if (!can && s.composing) {
        if (editFor_ == s.id) Detach(false);
        s.composing = false;
    }
    c.canSay = can;
    c.composing = s.composing;
    c.focused = editFor_ == s.id;
    c.draft = s.draft;
    c.typed = !Blank(c.focused ? EditText() : s.draft);
    c.note = s.note;
    c.noteBad = s.noteBad;
}

// A change of the card's 보내기 parts drawn now (the buttons changing as Update's changes do).
void App::SyncSay(Slot& s) {
    double now = Tick();
    rc::Card next = s.card;
    SayState(s, next, now);
    auto a = asks_.find(s.id);
    Changes(s, next, a != asks_.end() ? &a->second : nullptr, now);
    s.card = std::move(next);
    s.wantOpen = WantsOpen(s);
    Layout();
}

// 보내기 pressed: the row becomes the box, taking the keys.
void App::Compose(Slot& s) {
    s.composing = true;
    s.sayIntent = 1;  // the button grows into the box (Changes)
    SyncSay(s);
    Attach(s);
}

// The edit box over `s`'s row, with its words, taking the keys (a click on the card gave explorer the right to).
void App::Attach(Slot& s) {
    if (hidden_ || s.gone || !s.composing) return;
    if (!editFor_.empty() && editFor_ != s.id) Detach(false);
    if (!edit_) {
        edit_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", L"", WS_POPUP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 1, 1,
                                hwnd_, nullptr, ModuleInstance(), nullptr);
        if (!edit_) {
            Wh_Log(L"no edit box: %u", GetLastError());
            return;
        }
        editProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
        SendMessageW(edit_, EM_LIMITTEXT, kSayMax, 0);
    }
    if (editFor_ != s.id) {
        editFor_ = s.id;
        SetWindowTextW(edit_, s.draft.c_str());
        SendMessageW(edit_, EM_SETSEL, s.draft.size(), s.draft.size());
    }
    SyncSay(s);  // drawn with the box focused; PlaceEdit shows the edit box over it
    HWND fg = GetForegroundWindow();
    if (fg != edit_) before_ = fg;
    SetForegroundWindow(edit_);
    SetFocus(edit_);
}

// The edit box goes: its words stay in its card's row (drawn there). `giveBack`: the keys go back to the window
// that had them.
void App::Detach(bool giveBack) {
    if (editFor_.empty()) return;
    if (Slot* s = Find(editFor_)) s->draft = EditText();
    editFor_.clear();
    if (giveBack && before_ && IsWindow(before_) && GetForegroundWindow() == edit_) SetForegroundWindow(before_);
    before_ = nullptr;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    editAt_ = {};
}

// The edit box over its card's words (after every layout: the cards move), in the cards' size of the words.
void App::PlaceEdit() {
    if (editFor_.empty() || !edit_) return;
    Slot* s = Find(editFor_);
    if (!s || s->gone || hidden_ || !s->open || !s->card.composing) {
        Detach(false);
        return;
    }
    if (editScale_ != scale_) {
        if (editFont_) DeleteObject(editFont_);
        editFont_ = CreateFontW(-std::lround(12.5f * scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
        SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        HDC dc = GetDC(edit_);
        HGDIOBJ was = SelectObject(dc, editFont_);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, was);
        ReleaseDC(edit_, dc);
        editLineH_ = tm.tmHeight;
        editScale_ = scale_;
    }
    const D2D1_RECT_F& f = s->laid.field;
    int x = work_.left + int(std::lround((s->box.x + f.left) * scale_));
    int top = work_.top + int(std::lround((s->box.y + f.top) * scale_));
    int w = int(std::lround((f.right - f.left) * scale_)), fh = int(std::lround((f.bottom - f.top) * scale_));
    int h = std::min(fh, editLineH_ + 2);
    RECT at{x, top + (fh - h) / 2, x + w, top + (fh - h) / 2 + h};
    // While 보내기's box grows the edit box has the keys but no size: no caret or words over a moving shape (OnFrame
    // places it once the box has arrived).
    if (!s->sayBack && Tick() < SayEnd(*s)) at = {x, top, x, top};
    w = at.right - at.left;
    h = at.bottom - at.top;
    if (EqualRect(&at, &editAt_) && IsWindowVisible(edit_)) return;
    bool grew = editAt_.right <= editAt_.left && w > 0;
    editAt_ = at;
    SetWindowPos(edit_, HWND_TOPMOST, at.left, at.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (grew) {
        // Keys typed while it had no width scrolled its words out of sight: from their start again, the caret where
        // it was.
        DWORD from = 0, to = 0;
        SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        SendMessageW(edit_, EM_SETSEL, 0, 0);
        SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
        SendMessageW(edit_, EM_SETSEL, from, to);
        SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    }
}

std::wstring App::EditText() const {
    if (!edit_) return {};
    int n = GetWindowTextLengthW(edit_);
    std::wstring t(size_t(n) + 1, L'\0');
    t.resize(size_t(GetWindowTextW(edit_, t.data(), n + 1)));
    return t;
}

// The words in `s`'s row to Bloom, with the choices picked for its T3 question; the row is the buttons again, saying
// it is sending.
void App::SendSay(Slot& s) {
    if (editFor_ == s.id) Detach(true);
    auto a = asks_.find(s.id);
    const rc::Ask* q = a != asks_.end() && a->second.inRow ? &a->second : nullptr;
    if ((Blank(s.draft) && !(q && q->Answered())) || s.sending) {
        SyncSay(s);
        return;
    }
    // The words go to the session the row answered when they were begun (a sub-session's question, on its parent's
    // card, or the card's own), else to the one it answers now; words begun for a question that is over go nowhere
    // (as Bloom's no-question: back in the box, saying so).
    rc::SayTo to = rc::SayTarget(s.id, s.draftFor, s.draftAsk, Blank(s.draft), s.askOpened, s.askFor);
    s.sent = s.draft;
    s.sentFor = s.draftFor;
    s.sentAsk = s.draftAsk;
    s.draft.clear();
    s.draftFor.clear();
    s.draftAsk = false;
    s.composing = false;
    s.noteAt = Tick();
    if (to.over) {
        s.sentFor.clear();
        Said(s, false, L"no-question");
        SyncSay(s);
        return;
    }
    // The choices picked go with words to the session whose question they are of.
    std::optional<std::vector<std::vector<int>>> picks;
    if (q && (q->from.empty() ? s.id : q->from) == to.session) {
        picks = q->picks;
        asks_.erase(a);  // its choices go with the send; a send that fails asks Bloom for them again (Said)
    }
    s.sending = true;
    s.note = rc::Tr(g_korean, L"보내는 중…", L"Sending…");
    s.noteBad = false;
    if (!link_.Send(rc::SayLine(to.session, Narrow(s.sent), picks ? &*picks : nullptr))) Said(s, false, L"bloom");
    SyncSay(s);
}

// What came of a send (Bloom's `said`: how it went, or why not); words that did not go are back in the box.
void App::Said(Slot& s, bool ok, const std::wstring& word) {
    bool ko = g_korean;
    s.sending = false;
    s.noteAt = Tick();
    s.noteBad = !ok;
    if (ok)
        s.note = word == L"answered" ? rc::Tr(ko, L"질문에 답변했습니다", L"Answered the question")
                 : word == L"steered" ? rc::Tr(ko, L"현재 턴에 전달했습니다", L"Sent to the current turn")
                 : word == L"started" ? rc::Tr(ko, L"보냈습니다", L"Sent")
                                      : rc::Tr(ko, L"다음 턴으로 보냈습니다", L"Queued for the next turn");
    else
        s.note = word == L"t3-off"      ? rc::Tr(ko, L"T3가 꺼져 있어 보내지 못했습니다", L"Could not send because T3 is not running")
                 : word == L"no-thread" ? rc::Tr(ko, L"T3에서 이 세션을 찾을 수 없습니다", L"Could not find this session in T3")
                 : word == L"bloom"     ? rc::Tr(ko, L"Bloom에 연결되지 않아 보내지 못했습니다", L"Could not send because Bloom is unreachable")
                 : word == L"no-question" ? rc::Tr(ko, L"이미 종료된 질문입니다", L"This question has already ended")
                                        : rc::Tr(ko, L"보내지 못했어요", L"Failed to send");
    if (!ok && Blank(s.draft)) {
        // Back in the box for the session they were for; for a question over, for whatever the row answers next.
        s.draft = s.sent;
        s.draftFor = word == L"no-question" ? std::string() : s.sentFor;
        s.draftAsk = !s.draftFor.empty() && s.sentAsk;
    }
    s.sent.clear();
    s.sentFor.clear();
    // the question still waits: its choices again
    if (!ok && s.askOpened && link_.Up()) link_.Send(rc::SessionLine("question", s.askFor.empty() ? s.id : s.askFor));
}

// Enter sends, Esc gives the row back to the buttons (the words kept for the next 보내기).
void App::OnEditKey(WPARAM key) {
    Slot* s = Find(editFor_);
    if (!s) return;
    if (key == VK_RETURN) {
        SendSay(*s);
        return;
    }
    Detach(true);
    s->composing = false;
    s->sayIntent = -1;
    SyncSay(*s);
}

// The keys went elsewhere: the words stay drawn in the row; an empty row the person left goes back to the buttons
// (one opened for a question stays).
void App::OnEditBlur() {
    if (editFor_.empty() || !edit_ || GetFocus() == edit_) return;
    Slot* s = Find(editFor_);
    Detach(false);
    if (!s) return;
    if (Blank(s->draft) && !s->askOpened) {
        s->composing = false;
        s->sayIntent = -1;
    }
    SyncSay(*s);
}

// Words came or went: the send button lights or dims. The first words take the session the row answers then as
// theirs (SendSay sends them there, whatever asks by then); none left, none.
void App::OnEditChange() {
    Slot* s = Find(editFor_);
    if (!s) return;
    bool blank = Blank(EditText());
    if (blank) {
        s->draftFor.clear();
        s->draftAsk = false;
    } else if (s->draftFor.empty()) {
        s->draftFor = rc::RowFor(s->id, s->askOpened, s->askFor);
        s->draftAsk = s->askOpened;
    }
    if (s->card.typed != !blank) SyncSay(*s);
}

LRESULT App::EditColors(HDC dc) {
    auto ref = [](uint32_t argb) { return RGB(BYTE(argb >> 16), BYTE(argb >> 8), BYTE(argb)); };
    if (!editBrush_ || editBg_ != look_.btn) {
        if (editBrush_) DeleteObject(editBrush_);
        editBrush_ = CreateSolidBrush(ref(look_.btn));
        editBg_ = look_.btn;
    }
    SetTextColor(dc, ref(look_.text));
    SetBkColor(dc, ref(look_.btn));
    return reinterpret_cast<LRESULT>(editBrush_);
}

LRESULT CALLBACK App::EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = g_app;
    if (!self || !self->editProc_) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                self->OnEditKey(wParam);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wParam == L'\r' || wParam == L'\n' || wParam == 0x1B) return 0;  // handled at the key, with no beep
            break;
        case WM_KILLFOCUS:
            PostMessageW(self->hwnd_, kEditMsg, 0, 0);
            break;
    }
    return CallWindowProcW(self->editProc_, hwnd, msg, wParam, lParam);
}

// ---- fullscreen ---------------------------------------------------------------------------------------------
// A fullscreen app (a game, a video) on the cards' monitor keeps them away.

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

// ---- what lies along the monitor's edges -------------------------------------------------------------------------
// The cards keep clear of it (rc::Clear) and follow it as it changes: a hidden taskbar sliding out from PillTaskbar's
// island and back.

// Their windows: the taskbars, and in this process PillTaskbar's islands.
void App::FindEdgeWindows() {
    edgeWindows_.clear();
    for (PCWSTR cls : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, cls, nullptr));) edgeWindows_.push_back(w);
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* list = reinterpret_cast<std::vector<HWND>*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            WCHAR name[64];
            if (pid != GetCurrentProcessId() || !GetClassNameW(w, name, ARRAYSIZE(name))) return TRUE;
            if (wcsncmp(name, L"PillTaskbarIsland_", 18) == 0) list->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&edgeWindows_));
}

// Their shapes on the cards' monitor now, in the window's units (a window's region where it has one, else the window).
std::vector<rc::Box> App::Obstacles() {
    // A window of them destroyed (kEdgeBackMs): seen on the list as it was, before FindEdgeWindows makes it anew.
    double tick = Tick();
    if (tick >= edgeBackUntil_) edgeWant_ = 0;
    if (std::any_of(edgeWindows_.begin(), edgeWindows_.end(), [](HWND w) { return !IsWindow(w); })) {
        edgeWant_ = std::max(edgeWant_, edgeWindows_.size());
        edgeBackUntil_ = tick + kEdgeBackMs;
    }
    bool looking = tick < edgeBackUntil_;
    if (looking || edgeScan_-- <= 0) {
        edgeScan_ = kEdgeRescan;
        FindEdgeWindows();
        // Back: as many as before, and shown (a window is built hidden, then placed and shown).
        if (looking && edgeWindows_.size() >= edgeWant_ &&
            std::all_of(edgeWindows_.begin(), edgeWindows_.end(), [](HWND w) { return IsWindowVisible(w); }))
            edgeBackUntil_ = 0;
    }
    std::vector<rc::Box> out;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor_, &mi)) return out;
    for (HWND w : edgeWindows_) {
        DWORD cloaked = 0;
        if (!IsWindowVisible(w) || (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked))
            continue;
        RECT win{}, box{}, on{};
        if (!GetWindowRect(w, &win)) continue;
        int kind = GetWindowRgnBox(w, &box);
        if (kind == NULLREGION) continue;
        if (kind == ERROR) box = win;
        else OffsetRect(&box, win.left, win.top);
        if (!IntersectRect(&on, &box, &mi.rcMonitor)) continue;
        out.push_back({(on.left - work_.left) / scale_, (on.top - work_.top) / scale_, (on.right - on.left) / scale_,
                       (on.bottom - on.top) / scale_});
    }
    return out;
}

bool SameBox(const rc::Box& a, const rc::Box& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// What lies along the edges now; while a window of them destroyed is looked for (kEdgeBackMs), what they covered before
// too, so the cards hold still for an island built anew instead of moving under it and back.
std::vector<rc::Box> App::EdgesNow() {
    std::vector<rc::Box> now = Obstacles();
    if (Tick() < edgeBackUntil_)
        for (const rc::Box& b : edges_)
            if (std::none_of(now.begin(), now.end(), [&](const rc::Box& n) { return SameBox(n, b); })) now.push_back(b);
    return now;
}

// Every kEdgeMs: with cards shown, a shape that opened, closed or moved lays them out again; without, nothing is read
// (the next layout reads it first).
void App::CheckEdges() {
    if (hidden_ || std::none_of(slots_.begin(), slots_.end(), [](const auto& s) { return !s->gone; })) {
        edgesStale_ = true;
        return;
    }
    std::vector<rc::Box> now = EdgesNow();
    if (!edgesStale_ && now.size() == edges_.size() && std::equal(now.begin(), now.end(), edges_.begin(), SameBox)) return;
    edges_ = std::move(now);
    edgesStale_ = false;
    Layout();
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
        Detach(false);
        // Bloom's requests go back to Bloom's own cards while the cards are away.
        for (auto& [session, a] : asks_)
            if (!a.inRow) link_.Send("{\"t\":\"none\",\"id\":" + rc::JsonString(a.id) + "}");
        asks_.clear();
    }
    for (auto& s : slots_) {
        Spin(*s, !full && !s->gone && s->card.phase == rc::Phase::Work);
        Shine(*s);
    }
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
                        if (s.get() != under) s->hover = false, s->markHeld = false, s->ptr = {};
                    self->Update();
                } else if (wParam == kEdgeTimer) {
                    self->CheckEdges();
                } else if (wParam == kRegionTimer) {
                    KillTimer(hwnd, kRegionTimer);
                    self->SetRegion(false);
                } else if (wParam == kMorphTimer) {
                    // The pill drawn; the region follows once the moves are over (kRegionTimer).
                    KillTimer(hwnd, kMorphTimer);
                    self->Layout();
                } else if (wParam == kPageTimer) {
                    KillTimer(hwnd, kPageTimer);
                    self->PageTurn();
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
            case kEditMsg:
                if (self) self->OnEditBlur();
                return 0;
            case WM_CTLCOLOREDIT:
                if (self && self->edit_ && reinterpret_cast<HWND>(lParam) == self->edit_)
                    return self->EditColors(reinterpret_cast<HDC>(wParam));
                break;
            case WM_COMMAND:
                if (self && self->edit_ && reinterpret_cast<HWND>(lParam) == self->edit_ && HIWORD(wParam) == EN_CHANGE)
                    self->OnEditChange();
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
