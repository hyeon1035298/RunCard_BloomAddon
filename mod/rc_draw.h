// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: a card drawn with Direct2D and DirectWrite, in Bloom's look (its Palette tokens for dark and light, its
// accent, Segoe UI then Malgun Gothic, Segoe Fluent Icons). The layout follows the run-status card it was designed
// from: a header (the mark, what is under way, the turn's clock), the model and the step, a bar of steps, the last
// steps, a row of buttons. Folded, the card is its header alone, a pill.
//
// One routine lays the card out and draws it: given no device context it only measures, so the size and the places
// the buttons answer clicks at always match the picture. The spinning ring round the mark is not drawn here (the
// window turns a Composition shape for it, so a turning ring draws nothing again) unless `ring` asks for a still one.

#pragma once

#include "rc_model.h"
#include "si/si_draw.h"

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace rc {

using Microsoft::WRL::ComPtr;

// ---- Bloom's tokens -------------------------------------------------------------------------------------------

struct Palette {
    bool light = false;
    uint32_t surface, line, text, dim, faint, row, accent, accentInk, accentSoft, ok, danger, track, btn, btnHover,
        btnEdge, well, hover, mark;
};

inline double Luminance(uint32_t c) {
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(BYTE(c >> 16) / 255.0) + 0.7152 * lin(BYTE(c >> 8) / 255.0) + 0.0722 * lin(BYTE(c) / 255.0);
}

inline double Contrast(uint32_t a, uint32_t b) {
    double x = Luminance(a), y = Luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// Bloom Palette.cs: Popup, PopupLine, Text, TextDim, TextFaint, TileFg, Track, Btn, BtnHover, BtnEdge, Well, Hover,
// Ok, Danger; AccentInk as Bloom picks it (its own ink when that reads on the accent, else the other).
inline Palette MakePalette(bool light, uint32_t accent) {
    Palette p{};
    p.light = light;
    p.accent = accent;
    p.accentSoft = (accent & 0x00FFFFFF) | 0x1F000000;
    uint32_t ink = light ? 0xFFFAFAFA : 0xFF18181B, other = light ? 0xFF18181B : 0xFFFAFAFA;
    double own = Contrast(ink, accent);
    p.accentInk = own >= 3 || own >= Contrast(other, accent) ? ink : other;
    if (light) {
        p.surface = 0xFAFAFAFA, p.line = 0x1F111111, p.text = 0xFF11181C, p.dim = 0xFF3F3F46, p.faint = 0xFF71717A;
        p.row = 0xFF27272A, p.ok = 0xFF0E793C, p.danger = 0xFFB3363A, p.track = 0x1A111111, p.btn = 0xFFFFFFFF;
        p.btnHover = 0xFFF4F4F5, p.btnEdge = 0x26111111, p.well = 0x0D111111, p.hover = 0x14111111, p.mark = 0x14111111;
    } else {
        p.surface = 0xF527272A, p.line = 0x1FFFFFFF, p.text = 0xFFECEDEE, p.dim = 0xFFA1A1AA, p.faint = 0xFF97989A;
        p.row = 0xFFD4D4D8, p.ok = 0xFF17C964, p.danger = 0xFFE77578, p.track = 0x1AFFFFFF, p.btn = 0xFF3F3F46;
        p.btnHover = 0xFF52525B, p.btnEdge = 0x14FFFFFF, p.well = 0x660E0E10, p.hover = 0x14FFFFFF, p.mark = 0x14FFFFFF;
    }
    return p;
}

// ---- measures, in DIPs ------------------------------------------------------------------------------------------

constexpr float kCardW = 340;
constexpr float kPad = 14;
constexpr float kRadius = 12;  // Bloom's popup surfaces
constexpr float kPillH = 38;
constexpr float kPillMinW = 196;
constexpr float kMark = 22;       // the mark's circle
constexpr float kMarkLeft = 9;    // from the left edge, pill and card alike
constexpr float kHeaderH = 24;
constexpr float kRowH = 23;
constexpr int kRows = 5;          // steps shown
constexpr int kSegments = 10;     // the bar's most segments
constexpr float kBtnH = 28;
constexpr int kRequestLines = 14;  // a permission request's lines on the card; a longer one goes to Bloom's card
constexpr int kQuestionLines = 3, kLabelLines = 2, kAboutLines = 3;  // the same for a question and its choices
constexpr float kRingR = 10.5f, kRingW = 1.6f;  // the spinning ring round the mark
// A running step's segment fills toward the end without reaching it: half after this long.
constexpr double kHalfFillMs = 2500;

enum class View : uint8_t { Pill, Card };

// What a click at a place does.
enum class Act : uint8_t { None, Open, Allow, Deny, Send, Pick, Header };
struct Hit {
    D2D1_RECT_F r{};
    Act act = Act::None;
    int q = -1, choice = -1;
};

struct Laid {
    float w = 0, h = 0;
    D2D1_POINT_2F markAt{};  // the mark's centre (the window puts the ring there)
    std::vector<Hit> hits;
};

// The interaction state the picture shows.
struct Pointer {
    Act hover = Act::None;
    int q = -1, choice = -1;
    bool pressed = false;
};

class Painter {
public:
    Painter(ID2D1Factory* d2d, IDWriteFactory* dwrite) : d2d_(d2d), dwrite_(dwrite) {
        fallback_ = si::draw_impl::MakeFallback(dwrite);
        tabular_ = si::draw_impl::Tabular(dwrite);
        ComPtr<IDWriteFontCollection> fonts;
        dwrite->GetSystemFontCollection(&fonts);
        auto has = [&](const wchar_t* family) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            return fonts && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
        };
        icons_ = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        mono_ = has(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    }

    // Whether the card can show Bloom's request whole: every line of a permission request, every question and
    // choice in full. One that can't goes back to Bloom (`none`), whose own card shows it: nothing the user allows or
    // picks stays out of sight.
    bool Fits(const Ask& a, bool ko) {
        ko_ = ko;
        float tw = kCardW - 2 * kPad;
        if (a.questions.empty())
            return Lines(a.body, RequestW(kCardW), kRequestSize, DWRITE_FONT_WEIGHT_NORMAL, mono_) <= kRequestLines;
        for (const Question& q : a.questions) {
            if (Lines(q.text, tw, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) > kQuestionLines) return false;
            if (TextW(QuestionTop(q), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD) > tw) return false;
            for (const Choice& ch : q.choices) {
                if (Lines(ch.label, tw - 20, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) > kLabelLines) return false;
                if (Lines(ch.description, tw - 20, 11, DWRITE_FONT_WEIGHT_NORMAL) > kAboutLines) return false;
            }
        }
        return true;
    }

    // Lays the card out and, with a dc, draws it at the dc's transform (DIPs). `ask` is Bloom's request for this
    // session (Phase::Approve), `now` the clock the times are read at.
    Laid Run(ID2D1DeviceContext* dc, const Card& c, const Ask* ask, View view, const Palette& p, Ms now,
             const Pointer& ptr, bool ko, bool ring = false) {
        // The card's body goes under everything and its height is known only once laid out: measure first.
        float bodyH = view == View::Card && dc ? Run(nullptr, c, ask, view, p, now, ptr, ko).h : kPillH;
        dc_ = dc;
        p_ = &p;
        ptr_ = ptr;
        ko_ = ko;
        Laid L;
        if (dc_) {
            dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0), brush_.ReleaseAndGetAddressOf());
            dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        Phase phase = ask ? Phase::Approve : c.phase;
        std::wstring headline = Headline(c, ask, phase);
        std::wstring clock = Clock((c.turnEnd ? *c.turnEnd : now) - c.turnStart);

        // The pill, and the card's header: [mark] [icon] what is under way ............ 0:13
        // The card's header sits where the pill's does, so a pill opens into its card without a jump.
        float headerH = view == View::Pill ? kPillH : kHeaderH;
        float top = view == View::Pill ? 0 : (kPillH - kHeaderH) / 2;
        float clockW = TextW(clock, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
        bool headIcon = HeadIcon(c, ask, phase).has_value();
        float textX = kMarkLeft + kMark + 9 + (headIcon ? 16 + 7 : 0);
        if (view == View::Pill) {
            float want = textX + TextW(headline, 13, DWRITE_FONT_WEIGHT_NORMAL) + 14 + clockW + kPad;
            L.w = std::clamp(std::ceil(want), kPillMinW, kCardW);
            L.h = kPillH;
        } else {
            L.w = kCardW;
        }
        float markX = kMarkLeft + kMark / 2;
        float cy = top + headerH / 2;
        L.markAt = {markX, cy};
        float clockX = L.w - kPad - clockW;
        if (dc_) {
            Body(L.w, view == View::Pill ? kPillH : bodyH, view == View::Pill ? kPillH / 2 : kRadius);
            Mark(L.markAt, phase, c, ring);
            if (auto icon = HeadIcon(c, ask, phase)) Glyph(IconGlyph(*icon), markX + kMark / 2 + 9, cy - 8, 16, 16, p.dim, 14);
            Text(headline, textX, cy - 10, clockX - 10 - textX, 20, 13, view == View::Pill ? DWRITE_FONT_WEIGHT_NORMAL
                                                                                        : DWRITE_FONT_WEIGHT_NORMAL,
                 phase == Phase::Done || phase == Phase::Stopped ? p.text : view == View::Pill ? p.text : p.text);
            Text(clock, clockX, cy - 9, clockW + 1, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.dim, true);
        }
        L.hits.push_back({{0, top, L.w, top + headerH}, Act::Header});
        if (view == View::Pill) return L;

        // The model and the step.
        float y = top + headerH + 6;
        int n = int(c.steps.size());
        std::wstring step = c.phase == Phase::Done || c.phase == Phase::Stopped
                                ? (ko ? Num(n) + L"단계" : Num(n) + (n == 1 ? L" step" : L" steps"))
                                : (ko ? L"단계 " + Num(std::max(n, 1)) : L"Step " + Num(std::max(n, 1)));
        if (dc_) {
            float stepW = TextW(step, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
            Text(c.model, kPad, y, L.w - 2 * kPad - stepW - 8, 16, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint);
            Text(step, L.w - kPad - stepW, y, stepW + 1, 16, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint, true);
        }
        y += 16 + 7;
        if (dc_) Bar(c, kPad, y, L.w - 2 * kPad, now);
        y += 3 + 10;

        if (phase == Phase::Approve && ask && !ask->questions.empty()) {
            y = Questions(*ask, y, L);
        } else if (phase == Phase::Approve && ask) {
            y = Request(*ask, y, L.w);
        } else {
            y = Rows(c, y, L.w, now);
        }

        // The buttons.
        y += 6;
        float bx = kPad - 2;
        if (phase == Phase::Approve && ask && ask->questions.empty()) {
            bx = Button(L, bx, y, Tr(ko, L"허용", L"Allow"), L'\xE73E', Act::Allow, true, true);
            bx = Button(L, bx + 6, y, Tr(ko, L"거절", L"Deny"), L'\xE711', Act::Deny, false, true);
        } else if (phase == Phase::Approve && ask) {
            Button(L, bx, y, Tr(ko, L"보내기", L"Send"), L'\xE724', Act::Send, true, ask->Answered());
        }
        std::wstring open = phase == Phase::Approve ? Tr(ko, L"터미널에서", L"In terminal") : Tr(ko, L"열기", L"Open");
        float openW = TextW(open, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) + 6 + 12 + 20;
        D2D1_RECT_F o{L.w - kPad + 6 - openW, y, L.w - kPad + 6, y + kBtnH};
        L.hits.push_back({o, Act::Open});
        if (dc_) {
            if (ptr_.hover == Act::Open) Fill(o, kBtnH / 2, p.hover);
            Text(open, o.left + 10, y + 5, openW, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.text);
            Arrow(o.right - 10 - 12 + 2, y + kBtnH / 2, p.text);
        }
        L.h = y + kBtnH + 12;
        return L;
    }

private:
    ID2D1Factory* d2d_;
    IDWriteFactory* dwrite_;
    ComPtr<IDWriteFontFallback> fallback_;
    ComPtr<IDWriteTypography> tabular_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1DeviceContext* dc_ = nullptr;
    const Palette* p_ = nullptr;
    Pointer ptr_;
    bool ko_ = true;
    const wchar_t* icons_;
    const wchar_t* mono_;

    // ---- words ----

    std::wstring Headline(const Card& c, const Ask* ask, Phase phase) const {
        bool ko = ko_;
        if (phase == Phase::Approve && ask) {
            if (!ask->questions.empty())
                return ask->questions.size() == 1 ? Tr(ko, L"Claude의 질문", L"Claude has a question")
                                                  : Tr(ko, L"Claude의 질문 " + Num(ask->questions.size()) + L"개",
                                                       L"Claude has " + Num(ask->questions.size()) + L" questions");
            return Tr(ko, L"승인 요청 · ", L"Needs your approval · ") + ask->tool;
        }
        if (phase == Phase::Done) {
            Ms took = c.turnEnd.value_or(c.turnStart) - c.turnStart;
            std::wstring done = Tr(ko, Duration(true, took, true) + L" 만에 완료", L"Done in " + Duration(false, took, true));
            if (c.filesChanged > 0)
                done += Tr(ko, L" · 파일 " + Num(c.filesChanged) + L"개 바뀜",
                           L" · " + Num(c.filesChanged) + (c.filesChanged == 1 ? L" file changed" : L" files changed"));
            return done;
        }
        if (phase == Phase::Stopped) return Tr(ko, L"중단됨", L"Stopped");
        const Step* s = Current(c);
        // Claude Code waits for the person in its own window (a permission Bloom did not pass on).
        if (phase == Phase::Ask && !(s && (s->icon == Icon::Question || s->icon == Icon::Plan)))
            return Tr(ko, L"승인을 기다리는 중", L"Waiting for approval");
        if (s) return s->running;
        return Tr(ko, L"생각하는 중…", L"Thinking…");
    }

    // The icon after the mark: what is under way, or the tool asking. None once the turn is over (the mark says it).
    static std::optional<Icon> HeadIcon(const Card& c, const Ask* ask, Phase phase) {
        if (phase == Phase::Approve && ask) {
            std::string tool(ask->tool.begin(), ask->tool.end());
            return ask->questions.empty() ? KindOf(tool) : Icon::Question;
        }
        if (phase == Phase::Done || phase == Phase::Stopped) return std::nullopt;
        if (const Step* s = Current(c)) return s->icon;
        return Icon::Think;
    }

    // Segoe Fluent Icons (the same code points in Segoe MDL2 Assets).
    static wchar_t IconGlyph(Icon i) {
        switch (i) {
            case Icon::Read: return L'\xE8A5';      // Document
            case Icon::Search: return L'\xE721';    // Search
            case Icon::Edit: return L'\xE70F';      // Edit
            case Icon::Shell: return L'\xE756';     // CommandPrompt
            case Icon::Web: return L'\xE774';       // Globe
            case Icon::Agent: return L'\xE99A';     // Robot
            case Icon::Skill: return L'\xE82F';     // Lightbulb
            case Icon::Question: return L'\xE897';  // Help
            case Icon::Plan: return L'\xE8FD';      // BulletedList
            case Icon::Mcp: return L'\xE943';       // Code
            case Icon::Todo: return L'\xE9D5';      // CheckList
            case Icon::Think: return L'\xE945';     // LightningBolt... a spark
            case Icon::Wait: return L'\xE72E';      // Lock
            default: return L'\xE9CE';              // Unknown... a dot
        }
    }

    // ---- drawing helpers ----

    void Color(uint32_t argb, float opacity = 1) {
        brush_->SetColor(si::draw_impl::Color(argb, opacity));
    }

    void Fill(D2D1_RECT_F r, float radius, uint32_t argb) {
        Color(argb);
        dc_->FillRoundedRectangle({r, radius, radius}, brush_.Get());
    }

    // The body: the surface and its hairline edge.
    void Body(float w, float h, float radius) {
        D2D1_ROUNDED_RECT rr{{0.5f, 0.5f, w - 0.5f, h - 0.5f}, radius - 0.5f, radius - 0.5f};
        Color(p_->surface);
        dc_->FillRoundedRectangle(rr, brush_.Get());
        Color(p_->line);
        dc_->DrawRoundedRectangle(rr, brush_.Get(), 1);
    }

    ComPtr<IDWriteTextFormat> Format(float size, DWRITE_FONT_WEIGHT weight, const wchar_t* family = L"Segoe UI") {
        ComPtr<IDWriteTextFormat> f;
        dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                  L"", &f);
        if (!f) return f;
        si::draw_impl::SetFallback(f.Get(), fallback_.Get());
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return f;
    }

    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, float w, float h, float size, DWRITE_FONT_WEIGHT weight,
                                     bool tabular, bool wrap = false, const wchar_t* family = L"Segoe UI") {
        auto f = Format(size, weight, family);
        ComPtr<IDWriteTextLayout> l;
        if (!f) return l;
        if (wrap) {
            if (FAILED(f->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK))) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), f.Get(), std::max(w, 1.0f), std::max(h, 1.0f), &l);
        if (!l) return l;
        DWRITE_TEXT_RANGE all{0, UINT32(s.size())};
        if (tabular && tabular_) l->SetTypography(tabular_.Get(), all);
        if (!wrap) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> dots;
            if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(f.Get(), &dots))) l->SetTrimming(&trim, dots.Get());
        }
        return l;
    }

    float TextW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, bool tabular = false) {
        auto l = Layout(s, 4000, 40, size, weight, tabular);
        DWRITE_TEXT_METRICS m{};
        if (!l || FAILED(l->GetMetrics(&m))) return 0;
        return std::ceil(m.widthIncludingTrailingWhitespace);
    }

    // The lines a text takes wrapped at `w`.
    int Lines(const std::wstring& s, float w, float size, DWRITE_FONT_WEIGHT weight, const wchar_t* family = L"Segoe UI") {
        if (s.empty()) return 0;
        auto l = Layout(s, w, 4000, size, weight, false, true, family);
        DWRITE_TEXT_METRICS m{};
        if (!l || FAILED(l->GetMetrics(&m))) return 1 << 20;
        return int(m.lineCount);
    }

    // The height a wrapped text takes, at most `lines` lines.
    float TextH(const std::wstring& s, float w, float size, DWRITE_FONT_WEIGHT weight, int lines, float lineH) {
        auto l = Layout(s, w, 4000, size, weight, false, true);
        DWRITE_TEXT_METRICS m{};
        if (!l || FAILED(l->GetMetrics(&m))) return lineH;
        return std::min<float>(float(std::max<UINT32>(m.lineCount, 1)), float(lines)) * lineH;
    }

    void Text(const std::wstring& s, float x, float y, float w, float h, float size, DWRITE_FONT_WEIGHT weight,
              uint32_t argb, bool tabular = false, const wchar_t* family = L"Segoe UI") {
        if (!dc_ || s.empty() || w <= 0) return;
        auto l = Layout(s, w, h, size, weight, tabular, false, family);
        if (!l) return;
        Color(argb);
        dc_->DrawTextLayout({x, y}, l.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Wrapped text in at most `lines` lines (cut by a clip past that; Fits keeps a request from needing more).
    void Wrapped(const std::wstring& s, float x, float y, float w, float size, DWRITE_FONT_WEIGHT weight, int lines,
                 float lineH, uint32_t argb, const wchar_t* family = L"Segoe UI") {
        if (!dc_ || s.empty()) return;
        auto l = Layout(s, w, lines * lineH, size, weight, false, true, family);
        if (!l) return;
        DWRITE_LINE_SPACING_METHOD method = DWRITE_LINE_SPACING_METHOD_UNIFORM;
        l->SetLineSpacing(method, lineH, lineH * 0.8f);
        Color(argb);
        dc_->PushAxisAlignedClip({x, y, x + w, y + lines * lineH}, D2D1_ANTIALIAS_MODE_ALIASED);
        dc_->DrawTextLayout({x, y}, l.Get(), brush_.Get());
        dc_->PopAxisAlignedClip();
    }

    void Glyph(wchar_t g, float x, float y, float w, float h, uint32_t argb, float size) {
        if (!dc_) return;
        auto f = Format(size, DWRITE_FONT_WEIGHT_NORMAL, icons_);
        if (!f) return;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        Color(argb);
        wchar_t s[2] = {g, 0};
        dc_->DrawText(s, 1, f.Get(), {x, y, x + w, y + h}, brush_.Get());
    }

    // The mark: a small round tile with a spark in the accent; a check when done, a square when stopped; a dot
    // when the session waits for the person.
    void Mark(D2D1_POINT_2F at, Phase phase, const Card& c, bool ring) {
        const Palette& p = *p_;
        D2D1_ELLIPSE disc{at, kMark / 2, kMark / 2};
        if (phase == Phase::Done) {
            Color(p.ok);
            dc_->DrawEllipse({at, kMark / 2 - 1.5f, kMark / 2 - 1.5f}, brush_.Get(), 1.5f);
            ComPtr<ID2D1PathGeometry> tick;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d_->CreatePathGeometry(&tick)) && SUCCEEDED(tick->Open(&sink))) {
                sink->BeginFigure({at.x - 4.2f, at.y + 0.2f}, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine({at.x - 1.2f, at.y + 3.1f});
                sink->AddLine({at.x + 4.4f, at.y - 3.0f});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                ComPtr<ID2D1StrokeStyle> round;
                d2d_->CreateStrokeStyle({D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                         D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0},
                                        nullptr, 0, &round);
                dc_->DrawGeometry(tick.Get(), brush_.Get(), 1.6f, round.Get());
            }
            return;
        }
        Color(p.mark);
        dc_->FillEllipse(disc, brush_.Get());
        if (phase == Phase::Stopped) {
            Fill({at.x - 3.5f, at.y - 3.5f, at.x + 3.5f, at.y + 3.5f}, 1.5f, p.faint);
            return;
        }
        // The spark: eight rays, the long ones on the axes.
        ComPtr<ID2D1StrokeStyle> round;
        d2d_->CreateStrokeStyle({D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                 10, D2D1_DASH_STYLE_SOLID, 0},
                                nullptr, 0, &round);
        Color(p.accent);
        for (int i = 0; i < 8; i++) {
            double a = i * 3.14159265358979 / 4;
            float r0 = 1.3f, r1 = i % 2 ? 4.2f : 5.4f;
            dc_->DrawLine({at.x + float(std::cos(a)) * r0, at.y + float(std::sin(a)) * r0},
                          {at.x + float(std::cos(a)) * r1, at.y + float(std::sin(a)) * r1}, brush_.Get(), 1.6f,
                          round.Get());
        }
        if (ring && phase == Phase::Work) {
            // A still ring, as the window's turning one looks at one moment (a quarter and a bit of the circle).
            ComPtr<ID2D1PathGeometry> arc;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d_->CreatePathGeometry(&arc)) && SUCCEEDED(arc->Open(&sink))) {
                double a0 = -2.4, a1 = -0.6;
                sink->BeginFigure({at.x + float(std::cos(a0)) * kRingR, at.y + float(std::sin(a0)) * kRingR},
                                  D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc({{at.x + float(std::cos(a1)) * kRingR, at.y + float(std::sin(a1)) * kRingR},
                              {kRingR, kRingR},
                              0,
                              D2D1_SWEEP_DIRECTION_CLOCKWISE,
                              D2D1_ARC_SIZE_SMALL});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                Color(p.text);
                dc_->DrawGeometry(arc.Get(), brush_.Get(), kRingW, round.Get());
            }
        }
        if (phase == Phase::Approve || phase == Phase::Ask || c.asking) {
            D2D1_POINT_2F d{at.x + 7.5f, at.y - 7.5f};
            Color(p.light ? 0xFFFAFAFA : 0xFF27272A);
            dc_->FillEllipse({d, 4.6f, 4.6f}, brush_.Get());
            Color(p.accent);
            dc_->FillEllipse({d, 3.2f, 3.2f}, brush_.Get());
        }
    }

    // The bar of steps: a segment each (the oldest folded into the first when there are more than kSegments); the
    // finished ones in the accent, the running one filling. No segments for steps not yet taken: their number is
    // not known.
    void Bar(const Card& c, float x, float y, float w, Ms now) {
        const Palette& p = *p_;
        int n = int(c.steps.size());
        int segs = std::clamp(n, 1, kSegments);
        float gap = 4, sw = (w - gap * (segs - 1)) / segs;
        bool over = c.phase == Phase::Done || c.phase == Phase::Stopped;
        const Step* cur = Current(c);
        for (int i = 0; i < segs; i++) {
            D2D1_RECT_F r{x + i * (sw + gap), y, x + i * (sw + gap) + sw, y + 3};
            Fill(r, 1.5f, p.track);
            float part = 1;
            bool last = i == segs - 1;
            if (n == 0) part = over ? 1.0f : 0.0f;
            else if (last && cur && !over) {
                double t = double(std::max<Ms>(now - cur->startedAt, 0));
                part = float(t / (t + kHalfFillMs));
            }
            if (part <= 0) continue;
            uint32_t color = c.phase == Phase::Stopped ? p.faint : p.accent;
            Fill({r.left, r.top, r.left + std::max(3.0f, sw * part), r.bottom}, 1.5f, color);
        }
    }

    // The last steps: icon, words, time.
    float Rows(const Card& c, float y, float w, Ms now) {
        const Palette& p = *p_;
        int n = int(c.steps.size());
        int from = std::max(0, n - kRows);
        if (n == 0) {
            if (dc_)
                Text(c.phase == Phase::Work ? Tr(ko_, L"첫 단계를 준비하는 중", L"Getting ready")
                                            : Tr(ko_, L"도구를 쓰지 않은 답", L"Answered without tools"),
                     kPad + 24, y, w - 2 * kPad - 24, kRowH, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint);
            return y + kRowH;
        }
        for (int i = from; i < n; i++) {
            const Step& s = c.steps[i];
            bool running = !s.endedAt && c.phase != Phase::Done && c.phase != Phase::Stopped;
            std::wstring time = Duration(ko_, (s.endedAt ? *s.endedAt : now) - s.startedAt, running);
            if (!s.endedAt && !running) time.clear();
            if (dc_) {
                float tw = TextW(time, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
                uint32_t ic = s.failed ? p.danger : running ? p.faint : p.dim;
                Glyph(IconGlyph(s.icon), kPad - 1, y + 4, 16, 16, ic, 13);
                Text(running ? s.running : s.done, kPad + 24, y, w - 2 * kPad - 24 - tw - 10, kRowH, 12.5f,
                     DWRITE_FONT_WEIGHT_NORMAL, running ? p.faint : s.failed ? p.danger : p.row);
                Text(time, w - kPad - tw, y, tw + 1, kRowH, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint, true);
            }
            y += kRowH;
        }
        return y;
    }

    // A permission request: what the tool would do, as Bloom passed it on, in a well, whole (wrapped; Fits sends a
    // longer one to Bloom's card).
    static constexpr float kRequestSize = 11.5f, kRequestLineH = 17;
    static float RequestW(float w) { return w - 2 * kPad - 12; }

    float Request(const Ask& a, float y, float w) {
        const Palette& p = *p_;
        int n = std::clamp(Lines(a.body, RequestW(w), kRequestSize, DWRITE_FONT_WEIGHT_NORMAL, mono_), 1, kRequestLines);
        float h = 10 + n * kRequestLineH + 8;
        if (dc_) {
            Fill({kPad - 2, y, w - kPad + 2, y + h}, 8, p.well);
            Wrapped(a.body, kPad + 6, y + 9, RequestW(w), kRequestSize, DWRITE_FONT_WEIGHT_NORMAL, n, kRequestLineH, p.row,
                    mono_);
        }
        return y + h + 4;
    }

    // The small line over a question: its header, and that any number may be picked.
    std::wstring QuestionTop(const Question& q) const {
        std::wstring top = q.header;
        if (q.multi) top += (top.empty() ? L"" : L" · ") + Tr(ko_, L"여러 개 선택", L"Pick any");
        return top;
    }

    // AskUserQuestion: each question, its choices to pick (a ring for one, a box for any).
    float Questions(const Ask& a, float y, Laid& L) {
        const Palette& p = *p_;
        float w = L.w, tw = w - 2 * kPad;
        for (size_t qi = 0; qi < a.questions.size(); qi++) {
            const Question& q = a.questions[qi];
            if (qi > 0) y += 6;
            std::wstring top = QuestionTop(q);
            if (!top.empty()) {
                if (dc_) Text(top, kPad, y, tw, 16, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, p.faint);
                y += 17;
            }
            float th = TextH(q.text, tw, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kQuestionLines, 18);
            if (dc_) Wrapped(q.text, kPad, y, tw, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kQuestionLines, 18, p.text);
            y += th + 4;
            const std::vector<int>* picked = qi < a.picks.size() ? &a.picks[qi] : nullptr;
            for (size_t ci = 0; ci < q.choices.size(); ci++) {
                const Choice& ch = q.choices[ci];
                bool on = picked && std::find(picked->begin(), picked->end(), int(ci)) != picked->end();
                bool about = !ch.description.empty() && ch.description != ch.label;
                float labelH = TextH(ch.label, tw - 20, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, kLabelLines, 18);
                float aboutH = about ? TextH(ch.description, tw - 20, 11, DWRITE_FONT_WEIGHT_NORMAL, kAboutLines, 15) : 0;
                float rh = about ? 4 + labelH - 1 + aboutH + 2 : 4 + labelH + 4;
                D2D1_RECT_F r{kPad - 6, y, w - kPad + 6, y + rh};
                L.hits.push_back({r, Act::Pick, int(qi), int(ci)});
                if (dc_) {
                    bool hot = ptr_.hover == Act::Pick && ptr_.q == int(qi) && ptr_.choice == int(ci);
                    if (on) Fill(r, 8, p.accentSoft);
                    else if (hot) Fill(r, 8, p.hover);
                    D2D1_POINT_2F mc{kPad + 6, y + 13};
                    Color(on ? p.accent : p.faint);
                    if (q.multi) {
                        D2D1_ROUNDED_RECT box{{mc.x - 6, mc.y - 6, mc.x + 6, mc.y + 6}, 3, 3};
                        if (on) dc_->FillRoundedRectangle(box, brush_.Get());
                        else dc_->DrawRoundedRectangle(box, brush_.Get(), 1.3f);
                        if (on) Glyph(L'\xE73E', mc.x - 6, mc.y - 6, 12, 12, p.accentInk, 9);
                    } else {
                        dc_->DrawEllipse({mc, 6, 6}, brush_.Get(), 1.3f);
                        if (on) dc_->FillEllipse({mc, 3.2f, 3.2f}, brush_.Get());
                    }
                    Wrapped(ch.label, kPad + 20, y + 4, tw - 20, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, kLabelLines, 18, p.text);
                    if (about)
                        Wrapped(ch.description, kPad + 20, y + 4 + labelH - 1, tw - 20, 11, DWRITE_FONT_WEIGHT_NORMAL,
                                kAboutLines, 15, p.faint);
                }
                y += rh + 2;
            }
        }
        return y + 2;
    }

    // The design's open arrow: a north-east arrow, 8 DIPs, its centre left at (x, cy).
    void Arrow(float x, float cy, uint32_t argb) {
        ComPtr<ID2D1StrokeStyle> round;
        d2d_->CreateStrokeStyle({D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                 10, D2D1_DASH_STYLE_SOLID, 0},
                                nullptr, 0, &round);
        Color(argb);
        float a = x, b = cy + 4, e = x + 8, f = cy - 4;
        dc_->DrawLine({a, b}, {e, f}, brush_.Get(), 1.3f, round.Get());
        dc_->DrawLine({e - 5.5f, f}, {e, f}, brush_.Get(), 1.3f, round.Get());
        dc_->DrawLine({e, f}, {e, f + 5.5f}, brush_.Get(), 1.3f, round.Get());
    }

    // A pill button; returns its right edge.
    float Button(Laid& L, float x, float y, const std::wstring& label, wchar_t glyph, Act act, bool primary,
                 bool enabled) {
        const Palette& p = *p_;
        float w = 12 + 14 + 6 + TextW(label, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) + 14;
        D2D1_RECT_F r{x, y, x + w, y + kBtnH};
        if (enabled) L.hits.push_back({r, act});
        if (dc_) {
            bool hot = enabled && ptr_.hover == act;
            uint32_t fill = primary ? (enabled ? p.accent : p.track) : hot ? p.btnHover : p.btn;
            uint32_t ink = primary ? (enabled ? p.accentInk : p.faint) : p.text;
            Fill(r, kBtnH / 2, fill);
            if (primary && hot) Fill(r, kBtnH / 2, p.light ? 0x1AFFFFFF : 0x1A000000);
            if (!primary) {
                Color(p.btnEdge);
                dc_->DrawRoundedRectangle({{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, kBtnH / 2 - 0.5f,
                                           kBtnH / 2 - 0.5f},
                                          brush_.Get(), 1);
            }
            Glyph(glyph, x + 11, y + 7, 14, 14, ink, 11.5f);
            Text(label, x + 11 + 14 + 6, y + 5, w - 31, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, ink);
        }
        return r.right;
    }
};

}  // namespace rc
