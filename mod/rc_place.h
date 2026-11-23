// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: where the cards go. A monitor by its number (Windows' display number, the n of \\.\DISPLAYn; 0 for
// the main monitor), one of eight places on it, and the cards stacked from that place in the order they came:
// the oldest nearest the place, each next one further in. Top places stack down, bottom places up, the middle
// ones spread evenly above and below the middle. Across, a card keeps to its place's side (the middle places
// centre). At the top and bottom places they may go in a row instead (Row). Pure arithmetic apart from finding the
// monitor, so the test program checks it.

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

// The room the stack may take along its column: the work area less the margins. For a row, the height a card of it
// may take.
inline float Room(Box area, float margin) {
    return std::max(0.0f, area.h - 2 * margin);
}

// Whether the cards go in a row: asked for (card.arrange = row) and at a top or bottom place; the middle places stack.
inline bool Across(Anchor a, bool row) {
    return row && (AtTop(a) || AtBottom(a));
}

// The cards' boxes in one row across `area`, in the order given: the oldest at the place's corner, each next one
// further in (a right place's row grows leftwards, a left place's rightwards). A top place's cards align their tops,
// a bottom place's their bottoms. At the centre places the first card is in the middle and the next ones go to its
// right and left in turn (2 right, 3 left, 4 right…), the row as a whole centred. A middle place stacks (Stack).
inline std::vector<Box> Row(const std::vector<Size>& sizes, Anchor a, Box area, float margin, float gap) {
    if (!AtTop(a) && !AtBottom(a)) return Stack(sizes, a, area, margin, gap);
    std::vector<Box> out(sizes.size());
    for (size_t i = 0; i < sizes.size(); i++) {
        out[i].w = sizes[i].w;
        out[i].h = sizes[i].h;
        out[i].y = AtTop(a) ? area.y + margin : area.Bottom() - margin - sizes[i].h;
    }
    if (AtLeft(a) || AtRight(a)) {
        float x = AtLeft(a) ? area.x + margin : area.Right() - margin;
        for (Box& b : out) {
            if (AtLeft(a)) {
                b.x = x;
                x = b.Right() + gap;
            } else {
                b.x = x - b.w;
                x = b.x - gap;
            }
        }
        return out;
    }
    // Left to right: the even ones past the first, furthest first, then the first, then the odd ones.
    if (sizes.empty()) return out;
    std::vector<size_t> order;
    for (size_t i = 2; i < sizes.size(); i += 2) order.insert(order.begin(), i);
    order.push_back(0);
    for (size_t i = 1; i < sizes.size(); i += 2) order.push_back(i);
    float total = 0;
    for (const Size& s : sizes) total += s.w;
    total += gap * float(sizes.size() - 1);
    float x = area.x + (area.w - total) / 2;
    for (size_t i : order) {
        out[i].x = x;
        x = out[i].Right() + gap;
    }
    return out;
}

// The room a row may take across: the work area less the margins.
inline float RoomAcross(Box area, float margin) {
    return std::max(0.0f, area.w - 2 * margin);
}

// ---- keeping clear of what sits on the monitor's edges --------------------------------------------------------
// Session Island's shape (at rest, open, or between), a taskbar slid out over the work area, PillTaskbar's islands:
// each lies along an edge of the monitor. A card of the stack that would touch one moves that side of the area past
// it, so the stack starts beside it; the stack follows as the shape opens and closes.

enum class Edge : uint8_t { None, Top, Bottom, Left, Right };

// How close to the monitor's edge a shape lies to count as along it (DIPs): a pill taskbar floats a few DIPs in.
constexpr float kAlongEdge = 32;

// The edge of `mon` the box lies along (the nearest within kAlongEdge), or None.
inline Edge EdgeOf(Box o, Box mon) {
    float d[4] = {o.y - mon.y, mon.Bottom() - o.Bottom(), o.x - mon.x, mon.Right() - o.Right()};
    Edge e[4] = {Edge::Top, Edge::Bottom, Edge::Left, Edge::Right};
    int best = -1;
    for (int i = 0; i < 4; i++)
        if (d[i] <= kAlongEdge && (best < 0 || d[i] < d[best])) best = i;
    return best < 0 ? Edge::None : e[best];
}

// The boxes overlap, or come within `gap` of each other.
inline bool Near(Box a, Box b, float gap) {
    return a.x < b.Right() + gap && b.x < a.Right() + gap && a.y < b.Bottom() + gap && b.y < a.Bottom() + gap;
}

// The area `lay` (area → the cards' boxes) keeps to, clear of `obstacles` along the edges of `mon` (all in the same
// coordinates). A few rounds: a stack moved past one shape may meet another.
template <class Lay>
inline Box Clear(Lay lay, Box area, Box mon, float margin, float gap, const std::vector<Box>& obstacles) {
    for (int round = 0; round < 3 && !obstacles.empty(); round++) {
        std::vector<Box> boxes = lay(area);
        bool changed = false;
        for (const Box& o : obstacles) {
            Edge e = EdgeOf(o, mon);
            if (e == Edge::None) continue;
            if (std::none_of(boxes.begin(), boxes.end(), [&](const Box& b) { return Near(b, o, gap); })) continue;
            // the side of the area moved so the cards' margin starts a gap past the shape
            if (e == Edge::Top && o.Bottom() + gap - margin > area.y) {
                float top = o.Bottom() + gap - margin;
                area.h -= top - area.y;
                area.y = top;
                changed = true;
            } else if (e == Edge::Bottom && o.y - gap + margin < area.Bottom()) {
                area.h = o.y - gap + margin - area.y;
                changed = true;
            } else if (e == Edge::Left && o.Right() + gap - margin > area.x) {
                float left = o.Right() + gap - margin;
                area.w -= left - area.x;
                area.x = left;
                changed = true;
            } else if (e == Edge::Right && o.x - gap + margin < area.Right()) {
                area.w = o.x - gap + margin - area.x;
                changed = true;
            }
        }
        area.w = std::max(0.0f, area.w);
        area.h = std::max(0.0f, area.h);
        if (!changed) break;
    }
    return area;
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
