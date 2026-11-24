// SPDX-License-Identifier: GPL-3.0-only
//
// Run card test program: runs RunCardRuntime.dll and RunCard.dll in this process instead of explorer, playing the
// add-on host as PillTaskbar's tools/runtime-harness.cpp does (the config mapping, the events, a WH_GETMESSAGE hook
// on this thread), with the cards on the monitor above the main one (else another, else the main one). It also
// stands in for Bloom on a pipe of its own (RUN_CARD_PIPE), printing what the cards send and sending requests.
//
//   run-host.exe <runtime dll> <mod dll> <data folder> [--root <projects>] [--values <json>] [step ...]
// steps:
//   wait <ms>            pump messages for a while
//   values <json>        write values.json (as Bloom does); "$log": true and the monitor are added
//   point <i>            a pointer over the i-th card's header (posted mouse messages; the real pointer stays)
//   away                 the pointer leaves the cards
//   click <i> <dx> <dy>  a click on the i-th card, dx/dy DIPs from its top left (a negative dy: from its bottom)
//   move <i> <dx> <dy>   the pointer over that place of the i-th card (posted), as click places it
//   hoverat <i> <dx> <dy> the real cursor there (put back when the steps end)
//   ask <session>        Bloom passes on a permission request for that session (a Bash command)
//   askq <session>       Bloom passes on a question for that session
//   askqs <session>      Bloom passes on three questions for that session (shown one at a time)
//   shot <png>           the cards' area from the screen (DWM's picture) over a grey backdrop
//   shotc <png>          the same over coloured bands (the glass's blur shows)
//   shotw <png>          the same over white (shotk: over black), the brightest and darkest windows behind the glass
//   island <w> <h>       a stand-in for PillTaskbar's island (its window class) at the monitor's top centre, in pixels
//   reisland <gap> <hid> the island's window destroyed and built anew as PillTaskbar does on a setting change: none
//                        for <gap> ms, then made hidden, shown <hid> ms later
//   noisland             the island gone
//   clear <ms> <tol>     for <ms> ms, the cards must not overlap the island (shown) for more than <tol> ms in all

#include <windows.h>
#include <wincodec.h>

#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "addon_protocol.h"

namespace {

int g_failures;
std::wstring g_data;
HOOKPROC g_proc;
HANDLE g_loaded, g_unload, g_unloaded;
HMONITOR g_monitor;
int g_display = 0;
int g_size = 100;  // card.size, as the values say (the cards' DIPs are this much larger)

void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    g_failures += !ok;
}

std::wstring Named(PCWSTR format) {
    wchar_t name[96];
    swprintf(name, 96, format, GetCurrentProcessId());
    return name;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(file);
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string ReadText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read) text.append(buffer, read);
    CloseHandle(file);
    return text;
}

template <typename Done>
bool WaitFor(Done done, DWORD timeoutMs) {
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (!done()) {
        if (GetTickCount64() > end) return false;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
    }
    return true;
}

void Pump(DWORD ms) {
    WaitFor([] { return false; }, ms);
}

bool IsSet(HANDLE event) {
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

bool StateIs(const char* state) {
    std::string json = ReadText(g_data + L"\\" + addon::kRuntimeStateFile);
    return json.find(std::string("\"state\":\"") + state + "\"") != std::string::npos;
}

bool Inject() {
    ResetEvent(g_unload);
    ResetEvent(g_unloaded);
    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, g_proc, nullptr, GetCurrentThreadId());
    ULONGLONG nudged = 0;
    bool ok = WaitFor(
        [&] {
            if (GetTickCount64() - nudged >= 200) {
                PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
                nudged = GetTickCount64();
            }
            return IsSet(g_loaded);
        },
        8000);
    UnhookWindowsHookEx(hook);
    return ok;
}

bool Unload(DWORD timeoutMs) {
    SetEvent(g_unload);
    return WaitFor([] { return IsSet(g_unloaded); }, timeoutMs);
}

// The cards' window (on the mod's own thread: found by its class), this process's own: an installed copy of the add-on
// has one of the same class in explorer.
HWND Cards() {
    struct Find {
        DWORD pid;
        HWND found;
    } f{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD pid = 0;
            wchar_t cls[64];
            GetWindowThreadProcessId(h, &pid);
            if (pid != f->pid || !GetClassNameW(h, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"RunCardWindow_run-card") != 0)
                return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// Pixels per card DIP: the monitor's scale and card.size.
float Scale() {
    UINT dpi = GetDpiForWindow(Cards());
    return (dpi ? dpi / 96.0f : 1.0f) * float(g_size) / 100.0f;
}

// The i-th card's box in the window, from the window's region (the cards' shapes, top to bottom).
bool CardBox(int i, RECT& out) {
    HWND w = Cards();
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (!w || GetWindowRgn(w, rgn) == ERROR) {
        DeleteObject(rgn);
        return false;
    }
    DWORD size = GetRegionData(rgn, 0, nullptr);
    std::vector<BYTE> data(size);
    auto* rd = reinterpret_cast<RGNDATA*>(data.data());
    GetRegionData(rgn, size, rd);
    DeleteObject(rgn);
    // Rows of rectangles: join those that touch into cards.
    std::vector<RECT> cards;
    auto* rs = reinterpret_cast<RECT*>(rd->Buffer);
    for (DWORD k = 0; k < rd->rdh.nCount; k++) {
        RECT r = rs[k];
        bool joined = false;
        for (RECT& c : cards)
            if (r.top <= c.bottom + 1 && r.bottom >= c.top - 1 && r.left < c.right && r.right > c.left) {
                UnionRect(&c, &c, &r);
                joined = true;
                break;
            }
        if (!joined) cards.push_back(r);
    }
    std::sort(cards.begin(), cards.end(), [](const RECT& a, const RECT& b) { return a.top < b.top; });
    if (i < 0 || i >= int(cards.size())) return false;
    out = cards[i];
    return true;
}

void PostMouse(UINT msg, WPARAM keys, POINT client) {
    PostMessageW(Cards(), msg, keys, MAKELPARAM(client.x, client.y));
}

// ---- the stand-in Bloom -----------------------------------------------------------------------------------

std::wstring g_pipeName;
std::atomic<HANDLE> g_pipe{nullptr};
std::mutex g_write;
std::vector<std::string> g_heard;
std::mutex g_heardLock;

// Overlapped: the reading thread and ToCards use the one handle at once.
bool Wait(HANDLE pipe, OVERLAPPED& ov, BOOL started, DWORD& got) {
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    return GetOverlappedResult(pipe, &ov, &got, TRUE);
}

DWORD WINAPI Bloom(void*) {
    for (;;) {
        HANDLE p = CreateNamedPipeW(g_pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT,
                                    1, 65536, 65536, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) return 1;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0;
        BOOL c = ConnectNamedPipe(p, &ov);
        if (!c && GetLastError() != ERROR_PIPE_CONNECTED && !Wait(p, ov, c, got)) {
            CloseHandle(ov.hEvent);
            CloseHandle(p);
            continue;
        }
        g_pipe = p;
        std::string pending;
        char buffer[4096];
        for (;;) {
            ResetEvent(ov.hEvent);
            BOOL r = ReadFile(p, buffer, sizeof(buffer), nullptr, &ov);
            if (!Wait(p, ov, r, got) || !got) break;
            pending.append(buffer, got);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                printf("card> %s\n", line.c_str());
                fflush(stdout);
                std::lock_guard<std::mutex> hold(g_heardLock);
                g_heard.push_back(line);
            }
        }
        {
            std::lock_guard<std::mutex> hold(g_write);
            g_pipe = nullptr;
        }
        CloseHandle(ov.hEvent);
        CloseHandle(p);
    }
}

void ToCards(const std::string& line) {
    std::lock_guard<std::mutex> hold(g_write);
    HANDLE p = g_pipe;
    if (!p) {
        printf("bloom: no card connected\n");
        return;
    }
    std::string out = line + "\n";
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD wrote = 0;
    BOOL w = WriteFile(p, out.data(), DWORD(out.size()), nullptr, &ov);
    Wait(p, ov, w, wrote);
    CloseHandle(ov.hEvent);
    printf("bloom> %s\n", line.c_str());
    fflush(stdout);
}

bool Heard(const std::string& part) {
    std::lock_guard<std::mutex> hold(g_heardLock);
    for (auto& l : g_heard)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

// ---- pictures ---------------------------------------------------------------------------------------------

bool SavePng(const std::wstring& path, HBITMAP bmp, int w, int h) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmap* bitmap = nullptr;
    bool ok = SUCCEEDED(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &bitmap));
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    ok = ok && SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
         SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->WriteSource(bitmap, nullptr)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    for (IUnknown* u : {(IUnknown*)frame, (IUnknown*)enc, (IUnknown*)stream, (IUnknown*)bitmap, (IUnknown*)wic})
        if (u) u->Release();
    return ok;
}

// Coloured bands with white and black between, for the glass: what is behind a card shows through it blurred.
LRESULT CALLBACK BandsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_PAINT) return DefWindowProcW(h, msg, wp, lp);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    const COLORREF bands[] = {RGB(30, 79, 140), RGB(244, 241, 234), RGB(224, 122, 58), RGB(20, 22, 27), RGB(90, 170, 110)};
    int bw = 70;
    for (int x = 0, k = 0; x < r.right; x += bw, k++) {
        HBRUSH b = CreateSolidBrush(bands[k % 5]);
        RECT band{x, 0, x + bw, r.bottom};
        FillRect(dc, &band, b);
        DeleteObject(b);
    }
    EndPaint(h, &ps);
    return 0;
}

// What a shot puts behind the cards.
enum class Behind { Grey, Bands, White, Black };

// A plain window right under the cards while they are captured, so nothing behind them gets into the picture: grey,
// white or black, or the coloured bands.
HWND Backdrop(const RECT& box, Behind behind = Behind::Grey) {
    auto plain = [](PCWSTR name, COLORREF fill) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = CreateSolidBrush(fill);
        wc.lpszClassName = name;
        return RegisterClassW(&wc);
    };
    static ATOM cls = plain(L"RunCardShotBackdrop", RGB(128, 128, 128));
    static ATOM white = plain(L"RunCardShotWhite", RGB(255, 255, 255));
    static ATOM black = plain(L"RunCardShotBlack", RGB(0, 0, 0));
    static ATOM banded = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = BandsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RunCardShotBands";
        return RegisterClassW(&wc);
    }();
    if (!cls || !banded || !white || !black) return nullptr;
    PCWSTR name = behind == Behind::Bands   ? L"RunCardShotBands"
                  : behind == Behind::White ? L"RunCardShotWhite"
                  : behind == Behind::Black ? L"RunCardShotBlack"
                                            : L"RunCardShotBackdrop";
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, name, nullptr, WS_POPUP,
                             box.left, box.top, box.right - box.left, box.bottom - box.top, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!w) return nullptr;
    SetWindowPos(w, Cards(), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // DWM takes the glass's backdrop anew only when the cards' window shows a new frame (the clock's, once a
    // second): wait for one.
    Pump(1200);
    return w;
}

// The region's box with a margin, from the screen as DWM composed it.
bool Shot(const std::wstring& path, Behind behind = Behind::Grey) {
    HWND p = Cards();
    RECT box{}, w{};
    GetWindowRect(p, &w);
    if (GetWindowRgnBox(p, &box) == ERROR || IsRectEmpty(&box)) box = {0, 0, 400, 300};
    OffsetRect(&box, w.left, w.top);
    InflateRect(&box, 24, 24);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(g_monitor, &mi);
    IntersectRect(&box, &box, &mi.rcMonitor);
    // The backdrop reaches well past the picture: the glass blurs what is around the card too.
    RECT under = box;
    InflateRect(&under, 96, 96);
    IntersectRect(&under, &under, &mi.rcMonitor);
    HWND backdrop = Backdrop(under, behind);
    int cw = box.right - box.left, ch = box.bottom - box.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, cw, ch);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, cw, ch, screen, box.left, box.top, SRCCOPY);
    if (backdrop) DestroyWindow(backdrop);
    SelectObject(mem, old);
    bool ok = SavePng(path, bmp, cw, ch);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("shot %dx%d %s\n", cw, ch, ok ? "ok" : "failed");
    fflush(stdout);
    return ok;
}

// ---- the stand-in island: a window of PillTaskbar's island class in this process (the cards look for it here) ----

HWND g_island;
int g_islandW = 0, g_islandH = 0;

void IslandGone() {
    if (g_island) DestroyWindow(g_island);
    g_island = nullptr;
}

// Made as PillTaskbar builds it: hidden, 1x1 at the monitor's top left; then placed, shaped and shown (IslandShow).
void IslandMake() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"PillTaskbarIsland_test";
        registered = RegisterClassExW(&wc) != 0;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(g_monitor, &mi);
    g_island = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"PillTaskbarIsland_test", L"", WS_POPUP,
                               mi.rcMonitor.left, mi.rcMonitor.top, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void IslandShow() {
    if (!g_island) return;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(g_monitor, &mi);
    int x = (mi.rcMonitor.left + mi.rcMonitor.right - g_islandW) / 2;
    SetWindowPos(g_island, HWND_TOPMOST, x, mi.rcMonitor.top, g_islandW, g_islandH, SWP_NOACTIVATE);
    SetWindowRgn(g_island, CreateRoundRectRgn(0, 0, g_islandW + 1, g_islandH + 1, g_islandH, g_islandH), FALSE);
    ShowWindow(g_island, SW_SHOWNOACTIVATE);
}

// Whether a card overlaps the island now: the cards' region (their shapes; their paths while they glide) on the screen.
bool CardsOverIsland() {
    HWND w = Cards();
    RECT island{}, win{};
    if (!w || !g_island || !IsWindowVisible(g_island) || !GetWindowRect(g_island, &island) || !GetWindowRect(w, &win))
        return false;
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    bool over = false;
    if (GetWindowRgn(w, rgn) != ERROR) {
        OffsetRgn(rgn, win.left, win.top);
        over = RectInRegion(rgn, &island) != FALSE;
    }
    DeleteObject(rgn);
    return over;
}

// For `ms`, how long the cards overlap the island; at most `tolMs` passes.
void Clear(int ms, int tolMs) {
    ULONGLONG t0 = GetTickCount64(), last = t0;
    int overMs = 0;
    long long first = -1, end = -1;
    while (GetTickCount64() - t0 < ULONGLONG(ms)) {
        Pump(20);
        ULONGLONG now = GetTickCount64();
        if (CardsOverIsland()) {
            overMs += int(now - last);
            if (first < 0) first = (long long)(last - t0);
            end = (long long)(now - t0);
        }
        last = now;
    }
    printf("clear: over the island %d ms (first %lld, last %lld)\n", overMs, first, end);
    char what[96];
    snprintf(what, sizeof(what), "the cards keep clear of the island (over it %d ms, at most %d)", overMs, tolMs);
    Check(overMs <= tolMs, what);
}

std::string g_values;

void Values(const std::string& more) {
    std::string v = "\"$log\": true, \"card.monitor\": \"" + (g_display ? "display" + std::to_string(g_display) : std::string("primary")) +
                    "\"";
    if (!g_values.empty()) v += ", " + g_values;
    if (!more.empty()) v += ", " + more;
    size_t at = v.find("\"card.size\"");
    if (at != std::string::npos) {
        size_t digit = v.find_first_of("0123456789", at);
        g_size = digit == std::string::npos ? 100 : std::clamp(atoi(v.c_str() + digit), 80, 150);
    }
    WriteText(g_data + L"\\" + addon::kValuesFile, "{" + v + "}");
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        fwprintf(stderr, L"usage: run-host <runtime dll> <mod dll> <data folder> [options] [steps]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_data = argv[3];
    CreateDirectoryW(g_data.c_str(), nullptr);
    DeleteFileW((g_data + L"\\" + addon::kRuntimeStateFile).c_str());
    DeleteFileW((g_data + L"\\" + addon::kRuntimeLog).c_str());
    int i = 4;
    for (; i + 1 < argc && argv[i][0] == L'-'; i += 2) {
        std::wstring k = argv[i];
        if (k == L"--root") SetEnvironmentVariableW(L"RUN_CARD_ROOT", argv[i + 1]);
        else if (k == L"--values") g_values = Narrow(argv[i + 1]);
    }
    SetEnvironmentVariableW(L"RUN_CARD_TEST", L"1");
    std::wstring pipe = L"RunCardTest." + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"RUN_CARD_PIPE", pipe.c_str());
    g_pipeName = L"\\\\.\\pipe\\" + pipe;
    CloseHandle(CreateThread(nullptr, 0, Bloom, nullptr, 0, nullptr));

    // The monitor above the main one (this PC's upper monitor), else another, else the main one; by its number.
    struct Pick {
        HMONITOR primary, best;
        LONG top;
    } pick{MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), nullptr, 0};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT r, LPARAM p) -> BOOL {
            auto* k = reinterpret_cast<Pick*>(p);
            if (m != k->primary && (!k->best || r->top < k->top)) {
                k->best = m;
                k->top = r->top;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pick));
    g_monitor = pick.best ? pick.best : pick.primary;
    MONITORINFOEXW mx{};
    mx.cbSize = sizeof(mx);
    GetMonitorInfoW(g_monitor, &mx);
    const wchar_t* digits = mx.szDevice + wcslen(mx.szDevice);
    while (digits > mx.szDevice && digits[-1] >= L'0' && digits[-1] <= L'9') digits--;
    g_display = pick.best ? _wtoi(digits) : 0;
    printf("monitor %ls (display %d)\n", mx.szDevice, g_display);
    Values("");

    HANDLE configMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(addon::Config),
                                          Named(addon::kConfigMapping).c_str());
    auto* config = static_cast<addon::Config*>(MapViewOfFile(configMap, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    *config = {};
    config->size = sizeof(addon::Config);
    config->protocol = addon::kProtocol;
    config->flags = addon::kConfigLog;
    config->hostPid = GetCurrentProcessId();
    lstrcpynW(config->version, addon::kVersion, 32);
    lstrcpynW(config->dataDir, g_data.c_str(), 1024);
    GetFullPathNameW(argv[2], 1024, config->modPath, nullptr);
    g_loaded = CreateEventW(nullptr, TRUE, FALSE, Named(addon::kLoadedEvent).c_str());
    g_unload = CreateEventW(nullptr, TRUE, FALSE, Named(addon::kUnloadEvent).c_str());
    g_unloaded = CreateEventW(nullptr, TRUE, FALSE, Named(addon::kUnloadedEvent).c_str());

    HMODULE runtime = LoadLibraryW(argv[1]);
    g_proc = reinterpret_cast<HOOKPROC>(runtime ? GetProcAddress(runtime, addon::kHookProcName) : nullptr);
    if (!g_proc) {
        printf("FAIL runtime exports the hook procedure\n");
        return 1;
    }
    std::wstring mod = config->modPath;
    Check(Inject(), "runtime loaded");
    Check(WaitFor([] { return StateIs("running"); }, 10000), "runtime-state running");
    Check(WaitFor([] { return Cards() != nullptr; }, 5000), "the cards' window is up");
    Check(WaitFor([] { return Heard("\"t\":\"hello\""); }, 6000), "the cards said hello to Bloom");
    Pump(600);

    POINT cursorWas{};
    bool cursorSaved = false;
    for (; i < argc; i++) {
        std::wstring s = argv[i];
        auto arg = [&](int k) { return i + k < argc ? std::wstring(argv[i + k]) : std::wstring(); };
        if (s == L"wait") Pump(_wtoi(arg(1).c_str())), i++;
        else if (s == L"values") Values(Narrow(arg(1))), i++, Pump(500);
        else if (s == L"point") {
            RECT r;
            if (CardBox(_wtoi(arg(1).c_str()), r)) PostMouse(WM_MOUSEMOVE, 0, {r.left + 60, r.top + int(19 * Scale())});
            else printf("no card %ls\n", arg(1).c_str());
            Pump(400);
            i++;
        } else if (s == L"hover") {
            // The real cursor over card n's header (a posted move would be undone by the card's own check of the
            // cursor); put back when the steps end.
            RECT r;
            if (!cursorSaved) cursorSaved = GetCursorPos(&cursorWas);
            if (CardBox(_wtoi(arg(1).c_str()), r)) {
                POINT at{r.left + 60, r.top + int(19 * Scale())};
                ClientToScreen(Cards(), &at);
                SetCursorPos(at.x, at.y);
            } else {
                printf("no card %ls\n", arg(1).c_str());
            }
            Pump(500);
            i++;
        } else if (s == L"hoverat") {
            // The real cursor at dx/dy DIPs from card n's top left (put back when the steps end).
            RECT r;
            if (!cursorSaved) cursorSaved = GetCursorPos(&cursorWas);
            if (CardBox(_wtoi(arg(1).c_str()), r)) {
                POINT at{r.left + int(_wtoi(arg(2).c_str()) * Scale()), r.top + int(_wtoi(arg(3).c_str()) * Scale())};
                ClientToScreen(Cards(), &at);
                SetCursorPos(at.x, at.y);
            }
            Pump(500);
            i += 3;
        } else if (s == L"close-open") {
            // Right-click the first open card (a finished one closes by hand).
            RECT r;
            int k = 0;
            while (CardBox(k, r) && r.bottom - r.top < int(60 * Scale())) k++;
            if (CardBox(k, r)) {
                printf("right-click card %d\n", k);
                PostMouse(WM_MOUSEMOVE, 0, {r.left + 60, r.top + 19});
                PostMouse(WM_RBUTTONUP, 0, {r.left + 60, r.top + 19});
            } else {
                printf("no open card\n");
            }
            Pump(400);
        } else if (s == L"where") {
            // The cards' boxes in the work area (the window covers it), in pixels.
            RECT win{}, r;
            GetClientRect(Cards(), &win);
            printf("area %ldx%ld:", win.right, win.bottom);
            for (int k = 0; CardBox(k, r); k++) printf(" [%ld,%ld %ldx%ld]", r.left, r.top, r.right - r.left, r.bottom - r.top);
            printf("\n");
            fflush(stdout);
        } else if (s == L"heard") {
            Check(Heard(Narrow(arg(1))), ("Bloom heard " + Narrow(arg(1))).c_str());
            i++;
        } else if (s == L"away") {
            if (cursorSaved) SetCursorPos(cursorWas.x, cursorWas.y);
            else PostMessageW(Cards(), WM_MOUSELEAVE, 0, 0);
            Pump(700);
        } else if (s == L"click" || s == L"move") {
            RECT r;
            if (CardBox(_wtoi(arg(1).c_str()), r)) {
                float sc = Scale();
                int dy = _wtoi(arg(3).c_str());
                POINT at{r.left + int(_wtoi(arg(2).c_str()) * sc), dy < 0 ? r.bottom + int(dy * sc) : r.top + int(dy * sc)};
                PostMouse(WM_MOUSEMOVE, 0, at);
                if (s == L"click") {
                    PostMouse(WM_LBUTTONDOWN, MK_LBUTTON, at);
                    PostMouse(WM_LBUTTONUP, 0, at);
                }
            }
            Pump(400);
            i += 3;
        } else if (s == L"ask") {
            ToCards("{\"t\":\"ask\",\"id\":\"a1\",\"session\":\"" + Narrow(arg(1)) +
                    "\",\"pid\":0,\"tool\":\"Bash\",\"body\":\"command: git push origin fix/proration-rounding\\ndescription: "
                    "Push the fix\"}");
            Pump(500);
            Check(Heard("\"t\":\"shown\""), "the card showed the request");
            i++;
        } else if (s == L"askq") {
            ToCards("{\"t\":\"ask\",\"id\":\"q1\",\"session\":\"" + Narrow(arg(1)) +
                    "\",\"pid\":0,\"tool\":\"AskUserQuestion\",\"body\":\"\",\"questions\":[{\"header\":\"반올림\",\"text\":\"일할 "
                    "계산 금액은 어떻게 반올림할까요?\",\"multi\":false,\"choices\":[{\"label\":\"센트 단위 반올림 "
                    "(추천)\",\"description\":\"지금 청구서와 같습니다\"},{\"label\":\"내림\",\"description\":\"\"}]}]}");
            Pump(500);
            i++;
        } else if (s == L"askqs") {
            ToCards("{\"t\":\"ask\",\"id\":\"q3\",\"session\":\"" + Narrow(arg(1)) +
                    "\",\"pid\":0,\"tool\":\"AskUserQuestion\",\"body\":\"\",\"questions\":["
                    "{\"header\":\"반올림\",\"text\":\"일할 계산 금액은 어떻게 반올림할까요?\",\"multi\":false,\"choices\":["
                    "{\"label\":\"센트 단위 반올림 (추천)\",\"description\":\"지금 청구서와 같습니다\"},{\"label\":\"내림\","
                    "\"description\":\"\"}]},"
                    "{\"header\":\"통화\",\"text\":\"어느 통화부터 적용할까요?\",\"multi\":true,\"choices\":["
                    "{\"label\":\"원화\",\"description\":\"\"},{\"label\":\"달러\",\"description\":\"\"}]},"
                    "{\"header\":\"적용 시점\",\"text\":\"언제부터 바꿀까요?\",\"multi\":false,\"choices\":["
                    "{\"label\":\"다음 달부터\",\"description\":\"\"},{\"label\":\"바로\",\"description\":\"\"}]}]}");
            Pump(500);
            i++;
        } else if (s == L"shot" || s == L"shotc" || s == L"shotw" || s == L"shotk") {
            Shot(arg(1), s == L"shotc"   ? Behind::Bands
                         : s == L"shotw" ? Behind::White
                         : s == L"shotk" ? Behind::Black
                                         : Behind::Grey);
            i++;
        } else if (s == L"island") {
            IslandGone();
            g_islandW = _wtoi(arg(1).c_str());
            g_islandH = _wtoi(arg(2).c_str());
            IslandMake();
            IslandShow();
            i += 2;
        } else if (s == L"reisland") {
            IslandGone();
            Pump(_wtoi(arg(1).c_str()));
            IslandMake();
            Pump(_wtoi(arg(2).c_str()));
            IslandShow();
            i += 2;
        } else if (s == L"noisland") {
            IslandGone();
        } else if (s == L"clear") {
            Clear(_wtoi(arg(1).c_str()), _wtoi(arg(2).c_str()));
            i += 2;
        } else {
            fwprintf(stderr, L"unknown step %ls\n", s.c_str());
            break;
        }
    }

    if (cursorSaved) SetCursorPos(cursorWas.x, cursorWas.y);
    IslandGone();
    Check(Unload(10000), "unloaded");
    Check(Cards() == nullptr, "the cards' window is gone");
    Check(GetModuleHandleW(mod.c_str()) == nullptr, "mod DLL unmapped");
    Check(WaitFor([] { return StateIs("stopped"); }, 3000), "runtime-state stopped");
    printf("%d failure(s)\n", g_failures);
    if (g_failures) printf("---- runtime.log\n%s\n", ReadText(g_data + L"\\" + addon::kRuntimeLog).c_str());
    return g_failures;
}
