// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: where the cards go. A monitor by its number (Windows' display number, the n of \\.\DISPLAYn; 0 for
// the main monitor), one of eight places on it, and the cards stacked from that place in the order they came:
// the oldest nearest the place, each next one further in. Top places stack down, bottom places up, the middle
// ones spread evenly above and below the middle. Across, a card keeps to its place's side (the middle places
// centre). Pure arithmetic apart from finding the monitor, so the test program checks it.

#pragma once

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string_view>
#include <vector>

namespace rc {

enum class Anchor : uint8_t { TopLeft, TopCenter, TopRight, MiddleLeft, MiddleRight, BottomLeft, BottomCenter, BottomRight };

inline Anchor AnchorOf(std::wstring_view s) {
    if (s == L"topLeft") return Anchor::TopLeft;
    if (s == L"topCenter") return Anchor::TopCenter;
    if (s == L"middleLeft") return Anchor::MiddleLeft;
    if (s == L"middleRight") return Anchor::MiddleRight;
    if (s == L"bottomLeft") return Anchor::BottomLeft;
    if (s == L"bottomCenter") return Anchor::BottomCenter;
    if (s == L"bottomRight") return Anchor::BottomRight;
    return Anchor::TopRight;
}

inline bool AtTop(Anchor a) {
    return a == Anchor::TopLeft || a == Anchor::TopCenter || a == Anchor::TopRight;
}
inline bool AtBottom(Anchor a) {
    return a == Anchor::BottomLeft || a == Anchor::BottomCenter || a == Anchor::BottomRight;
}
inline bool AtLeft(Anchor a) {
    return a == Anchor::TopLeft || a == Anchor::MiddleLeft || a == Anchor::BottomLeft;
}
inline bool AtRight(Anchor a) {
    return a == Anchor::TopRight || a == Anchor::MiddleRight || a == Anchor::BottomRight;
}

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
};

struct Size {
    float w = 0, h = 0;
};

// The cards' boxes in `area` (the monitor's work area, DIPs), in the order given.
inline std::vector<Box> Stack(const std::vector<Size>& sizes, Anchor a, Box area, float margin, float gap) {
    std::vector<Box> out;
    out.reserve(sizes.size());
    float total = 0;
    for (const Size& s : sizes) total += s.h;
    if (!sizes.empty()) total += gap * float(sizes.size() - 1);
    float y = AtTop(a)      ? area.y + margin
              : AtBottom(a) ? area.Bottom() - margin
                            : area.y + std::max(margin, (area.h - total) / 2);
    for (const Size& s : sizes) {
        Box b{0, 0, s.w, s.h};
        b.x = AtLeft(a) ? area.x + margin : AtRight(a) ? area.Right() - margin - s.w : area.x + (area.w - s.w) / 2;
        if (AtBottom(a)) {
            b.y = y - s.h;
            y = b.y - gap;
        } else {
            b.y = y;
            y = b.Bottom() + gap;
        }
        out.push_back(b);
    }
    return out;
}

// The room the stack may take along its column: the work area less the margins.
inline float Room(Box area, float margin) {
    return std::max(0.0f, area.h - 2 * margin);
}

// The monitor with Windows' display number n (\\.\DISPLAYn), or the main monitor for 0 or a number not connected.
inline HMONITOR MonitorByNumber(int n) {
    struct Find {
        int n;
        HMONITOR found;
    } f{n, nullptr};
    if (n > 0) {
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
                auto* f = reinterpret_cast<Find*>(p);
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (!GetMonitorInfoW(m, &info)) return TRUE;
                const wchar_t* d = info.szDevice;
                const wchar_t* digits = d + wcslen(d);
                while (digits > d && digits[-1] >= L'0' && digits[-1] <= L'9') digits--;
                if (*digits && _wtoi(digits) == f->n) {
                    f->found = m;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&f));
    }
    return f.found ? f.found : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

}  // namespace rc
