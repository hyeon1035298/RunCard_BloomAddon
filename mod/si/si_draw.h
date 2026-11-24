// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: the drawing helpers the cards use with Direct2D and DirectWrite. A colour from
// ARGB, SVG path data to a Direct2D path, Bloom's type ("Segoe UI", then "Malgun Gothic", then the system's
// fallback) and tabular figures.

#pragma once

#include <d2d1_1.h>
#include <d2d1_2.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace si {

namespace draw_impl {

using Microsoft::WRL::ComPtr;

inline D2D1_COLOR_F Color(uint32_t argb, double opacity = 1) {
    return {float((argb >> 16) & 255) / 255.f, float((argb >> 8) & 255) / 255.f, float(argb & 255) / 255.f,
            float(((argb >> 24) & 255) / 255.0 * opacity)};
}

// ---- SVG path data to a Direct2D path -------------------------------------------------------------
// The commands M L H V C S Q A Z, absolute and relative.

class PathParser {
public:
    explicit PathParser(const char* s) : s_(s) {}

    bool Run(ID2D1GeometrySink* sink) {
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        D2D1_POINT_2F cur{}, start{}, ctrl{};
        bool open = false, cubic = false;
        char cmd = 0;
        auto begin = [&] {
            if (!open) sink->BeginFigure(cur, D2D1_FIGURE_BEGIN_FILLED);
            open = true;
        };
        auto end = [&](bool closed) {
            if (open) sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            open = false;
        };
        for (;;) {
            Ws();
            if (*s_ == 0) break;
            if ((*s_ >= 'A' && *s_ <= 'Z') || (*s_ >= 'a' && *s_ <= 'z')) {
                cmd = *s_++;
            } else if (cmd == 0) {
                return false;
            }
            bool rel = cmd >= 'a';
            float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
            bool wasCubic = cubic;
            cubic = false;
            switch (cmd | 0x20) {
                case 'm': {
                    float x, y;
                    if (!Num(x) || !Num(y)) return false;
                    end(false);
                    cur = start = {ox + x, oy + y};
                    begin();
                    cmd = rel ? 'l' : 'L';  // pairs after a move are lines
                    break;
                }
                case 'z':
                    end(true);
                    cur = start;
                    cmd = 0;
                    break;
                case 'l': {
                    float x, y;
                    if (!Num(x) || !Num(y)) return false;
                    begin();
                    cur = {ox + x, oy + y};
                    sink->AddLine(cur);
                    break;
                }
                case 'h': {
                    float x;
                    if (!Num(x)) return false;
                    begin();
                    cur.x = ox + x;
                    sink->AddLine(cur);
                    break;
                }
                case 'v': {
                    float y;
                    if (!Num(y)) return false;
                    begin();
                    cur.y = oy + y;
                    sink->AddLine(cur);
                    break;
                }
                case 'c':
                case 's': {
                    float x1 = 0, y1 = 0, x2, y2, x, y;
                    bool smooth = (cmd | 0x20) == 's';
                    if (!smooth && (!Num(x1) || !Num(y1))) return false;
                    if (!Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return false;
                    begin();
                    D2D1_POINT_2F c1 = smooth ? (wasCubic ? D2D1_POINT_2F{2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur)
                                              : D2D1_POINT_2F{ox + x1, oy + y1};
                    D2D1_POINT_2F c2{ox + x2, oy + y2};
                    cur = {ox + x, oy + y};
                    sink->AddBezier({c1, c2, cur});
                    ctrl = c2;
                    cubic = true;
                    break;
                }
                case 'q': {
                    float x1, y1, x, y;
                    if (!Num(x1) || !Num(y1) || !Num(x) || !Num(y)) return false;
                    begin();
                    D2D1_POINT_2F c1{ox + x1, oy + y1};
                    cur = {ox + x, oy + y};
                    sink->AddQuadraticBezier({c1, cur});
                    break;
                }
                case 'a': {
                    float rx, ry, rot, x, y;
                    int large, sweep;
                    if (!Num(rx) || !Num(ry) || !Num(rot) || !Flag(large) || !Flag(sweep) || !Num(x) || !Num(y))
                        return false;
                    begin();
                    cur = {ox + x, oy + y};
                    sink->AddArc({cur, {std::fabs(rx), std::fabs(ry)}, rot,
                                  sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
                    break;
                }
                default:
                    return false;
            }
        }
        end(false);
        return true;
    }

private:
    void Ws() {
        while (*s_ == ' ' || *s_ == ',' || *s_ == '\t' || *s_ == '\n' || *s_ == '\r') s_++;
    }
    // An SVG number: sign, digits, a point and digits, an exponent. Locale-free.
    bool Num(float& out) {
        Ws();
        const char* p = s_;
        double sign = 1;
        if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
        double v = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'), any = true;
        if (*p == '.') {
            p++;
            double f = 0.1;
            while (*p >= '0' && *p <= '9') v += (*p++ - '0') * f, f /= 10, any = true;
        }
        if (!any) return false;
        if ((*p == 'e' || *p == 'E') && ((p[1] >= '0' && p[1] <= '9') || p[1] == '-' || p[1] == '+')) {
            p++;
            int es = 1, e = 0;
            if (*p == '+' || *p == '-') es = *p++ == '-' ? -1 : 1;
            while (*p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
            v *= std::pow(10.0, es * e);
        }
        out = float(sign * v);
        s_ = p;
        return true;
    }
    bool Flag(int& out) {
        Ws();
        if (*s_ != '0' && *s_ != '1') return false;
        out = *s_++ - '0';
        return true;
    }

    const char* s_;
};

inline ComPtr<ID2D1PathGeometry> ParsePath(ID2D1Factory* f, const char* d) {
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return nullptr;
    bool ok = PathParser(d).Run(sink.Get());
    if (FAILED(sink->Close()) || !ok) return nullptr;
    return g;
}


// Bloom's font list, "Segoe UI","Malgun Gothic",sans-serif: what Segoe UI lacks comes from Malgun Gothic first
// (Hangul, and shapes such as U+25C6), then from the system's fallback (U+2715 from Segoe UI Symbol).
inline ComPtr<IDWriteFontFallback> MakeFallback(IDWriteFactory* dwrite) {
    ComPtr<IDWriteFactory2> f2;
    ComPtr<IDWriteFontFallbackBuilder> builder;
    ComPtr<IDWriteFontFallback> system, out;
    if (FAILED(dwrite->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void**>(f2.GetAddressOf()))) ||
        FAILED(f2->CreateFontFallbackBuilder(&builder)) || FAILED(f2->GetSystemFontFallback(&system)))
        return nullptr;
    // Malgun Gothic only where it has the characters: a mapping that names a font without them would still
    // be taken and draw boxes, so the ranges come from the font itself.
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    ComPtr<IDWriteFontFace1> face1;
    if (SUCCEEDED(dwrite->GetSystemFontCollection(&fonts)) &&
        SUCCEEDED(fonts->FindFamilyName(L"Malgun Gothic", &index, &exists)) && exists &&
        SUCCEEDED(fonts->GetFontFamily(index, &family)) &&
        SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, &font)) &&
        SUCCEEDED(font->CreateFontFace(&face)) &&
        SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace1), reinterpret_cast<void**>(face1.GetAddressOf())))) {
        UINT32 count = 0;
        face1->GetUnicodeRanges(0, nullptr, &count);
        std::vector<DWRITE_UNICODE_RANGE> ranges(count);
        if (count > 0 && SUCCEEDED(face1->GetUnicodeRanges(count, ranges.data(), &count))) {
            const WCHAR* names[] = {L"Malgun Gothic"};
            builder->AddMapping(ranges.data(), count, names, 1);
        }
    }
    builder->AddMappings(system.Get());
    if (FAILED(builder->CreateFontFallback(&out))) return nullptr;
    return out;
}

inline void SetFallback(IDWriteTextFormat* format, IDWriteFontFallback* fallback) {
    ComPtr<IDWriteTextFormat1> f1;
    if (fallback != nullptr &&
        SUCCEEDED(format->QueryInterface(__uuidof(IDWriteTextFormat1), reinterpret_cast<void**>(f1.GetAddressOf()))))
        f1->SetFontFallback(fallback);
}

inline ComPtr<IDWriteTypography> Tabular(IDWriteFactory* dwrite) {
    ComPtr<IDWriteTypography> t;
    if (SUCCEEDED(dwrite->CreateTypography(&t))) t->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    return t;
}

}  // namespace draw_impl
}  // namespace si
