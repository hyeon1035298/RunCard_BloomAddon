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
    uint32_t shine;  // a working headline's resting tone, under the light swept over it
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
// The card's own: TextDim and TextFaint are lighter in dark and TextFaint darker in light, and AccentInk keeps its own
// ink only from 4.5:1, so all the card's words read at 4.5:1 or more on the body, a hovered or picked choice and the
// row to type in (Bloom's windows keep theirs).
// `glass` (Bloom's 유리 투명도, 0-100; -1 without glass): the body is Bloom's glass fill over the blur behind it
// (Palette.Glass: Solid, see-through by the setting) instead of the Popup colour.
inline Palette MakePalette(bool light, uint32_t accent, int glass = -1) {
    Palette p{};
    p.light = light;
    p.accent = accent;
    p.accentSoft = (accent & 0x00FFFFFF) | 0x1F000000;
    uint32_t ink = light ? 0xFFFAFAFA : 0xFF18181B, other = light ? 0xFF18181B : 0xFFFAFAFA;
    double own = Contrast(ink, accent);
    p.accentInk = own >= 4.5 || own >= Contrast(other, accent) ? ink : other;
    if (light) {
        p.surface = 0xFAFAFAFA, p.line = 0x1F111111, p.text = 0xFF11181C, p.dim = 0xFF3F3F46, p.faint = 0xFF5E5E67;
        p.row = 0xFF27272A, p.ok = 0xFF0E793C, p.danger = 0xFFB3363A, p.track = 0x1A111111, p.btn = 0xFFFFFFFF;
        p.btnHover = 0xFFF4F4F5, p.btnEdge = 0x26111111, p.well = 0x0D111111, p.hover = 0x14111111, p.mark = 0x14111111;
        p.shine = p.faint;
    } else {
        p.surface = 0xF527272A, p.line = 0x1FFFFFFF, p.text = 0xFFECEDEE, p.dim = 0xFFC4C4CB, p.faint = 0xFFACACB4;
        p.shine = 0xFFA1A1AA;
        p.row = 0xFFD4D4D8, p.ok = 0xFF17C964, p.danger = 0xFFE77578, p.track = 0x1AFFFFFF, p.btn = 0xFF3F3F46;
        p.btnHover = 0xFF52525B, p.btnEdge = 0x14FFFFFF, p.well = 0x660E0E10, p.hover = 0x14FFFFFF, p.mark = 0x14FFFFFF;
    }
    if (glass >= 0) {
        uint32_t alpha = uint32_t(std::lround(2.55 * (100 - std::clamp(glass, 0, 100))));
        p.surface = (light ? 0x00F4F4F5u : 0x0018181Bu) | alpha << 24;
    }
    return p;
}

// ---- measures, in DIPs ------------------------------------------------------------------------------------------

constexpr float kCardW = 340;
constexpr float kPad = 14;
constexpr float kRadius = 12;  // Bloom's popup surfaces
constexpr float kPillH = 38;
constexpr float kPillMinW = 196;
// A pill of a row (card.arrange = row) with pill.width = same: shorter than a card, so folding a card narrows the row.
constexpr float kRowPillW = 220;
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
// Claude's mark (the spark Claude's apps show; a trademark of Anthropic, see THIRD_PARTY.md), its path in a 24 × 24 box
// as Simple Icons draws it.
constexpr const char* kClaudeMark =
    "m4.7144 15.9555 4.7174-2.6471.079-.2307-.079-.1275h-.2307l-.7893-.0486-2.6956-.0729-2.3375-.0971-2.2646-.1214-.5707-"
    ".1215-.5343-.7042.0546-.3522.4797-.3218.686.0608 1.5179.1032 2.2767.1578 1.6514.0972 2.4468.255h.3886l.0546-.1579-."
    "1336-.0971-.1032-.0972L6.973 9.8356l-2.55-1.6879-1.3356-.9714-.7225-.4918-.3643-.4614-.1578-1.0078.6557-.7225.8803.0"
    "607.2246.0607.8925.686 1.9064 1.4754 2.4893 1.8336.3643.3035.1457-.1032.0182-.0728-.164-.2733-1.3539-2.4467-1.445-2.4"
    "893-.6435-1.032-.17-.6194c-.0607-.255-.1032-.4674-.1032-.7285L6.287.1335 6.6997 0l.9957.1336.419.3642.6192 1.4147 1."
    "0018 2.2282 1.5543 3.0296.4553.8985.2429.8318.091.255h.1579v-.1457l.1275-1.706.2368-2.0947.2307-2.6957.0789-.7589.37"
    "64-.9107.7468-.4918.5828.2793.4797.686-.0668.4433-.2853 1.8517-.5586 2.9021-.3643 1.9429h.2125l.2429-.2429.9835-1.30"
    "53 1.6514-2.0643.7286-.8196.85-.9046.5464-.4311h1.0321l.759 1.1293-.34 1.1657-1.0625 1.3478-.8804 1.1414-1.2628 1.7-"
    ".7893 1.36.0729.1093.1882-.0183 2.8535-.607 1.5421-.2794 1.8396-.3157.8318.3886.091.3946-.3278.8075-1.967.4857-2.3072"
    ".4614-3.4364.8136-.0425.0304.0486.0607 1.5482.1457.6618.0364h1.621l3.0175.2247.7892.522.4736.6376-.079.4857-1.2142.6"
    "193-1.6393-.3886-3.825-.9107-1.3113-.3279h-.1822v.1093l1.0929 1.0686 2.0035 1.8092 2.5075 2.3314.1275.5768-.3218.455"
    "4-.34-.0486-2.2039-1.6575-.85-.7468-1.9246-1.621h-.1275v.17l.4432.6496 2.3436 3.5214.1214 1.0807-.17.3521-.6071.2125"
    "-.6679-.1214-1.3721-1.9246L14.38 17.959l-1.1414-1.9428-.1397.079-.674 7.2552-.3156.3703-.7286.2793-.6071-.4614-.3218"
    "-.7468.3218-1.4753.3886-1.9246.3157-1.53.2853-1.9004.17-.6314-.0121-.0425-.1397.0182-1.4328 1.9672-2.1796 2.9446-1.72"
    "43 1.8456-.4128.164-.7164-.3704.0667-.6618.4008-.5889 2.386-3.0357 1.4389-1.882.929-1.0868-.0062-.1579h-.0546l-6.3385"
    " 4.1164-1.1293.1457-.4857-.4554.0608-.7467.2307-.2429 1.9064-1.3114Z";
constexpr float kClaudeSize = 14;  // the mark's width and height in the disc
// A running step's segment fills toward the end without reaching it: half after this long.
constexpr double kHalfFillMs = 2500;

enum class View : uint8_t { Pill, Card };

// What a click at a place does.
// Reply opens the row to type in; Field is that box (the window's edit box goes over it), Say its send button.
// Page turns to another question (Hit::q) when there are several.
enum class Act : uint8_t { None, Open, Allow, Deny, Send, Pick, Header, Stop, Close, Reply, Field, Say, Page };
struct Hit {
    D2D1_RECT_F r{};
    Act act = Act::None;
    int q = -1, choice = -1;
};

struct Laid {
    float w = 0, h = 0;
    D2D1_POINT_2F markAt{};  // the mark's centre (the window puts the ring there)
    std::vector<Hit> hits;
    D2D1_RECT_F title{};     // the headline's words
    std::wstring titleText;
    bool shine = false;      // drawn for the window's light (Motion::shine, a working headline at rest)
    D2D1_RECT_F field{};     // where the typed words go in the row to type in (the window's edit box), when composing
};

// The interaction state the picture shows.
struct Pointer {
    Act hover = Act::None;
    int q = -1, choice = -1;
    bool pressed = false;
};

// ---- motion: as the run-status card the design came from moves ------------------------------------------------

// Fluent decelerate (PillTaskbar's kIslandEase): a card opening out of its pill or folding back, the stack moving.
constexpr float kEase[4] = {0.1f, 0.9f, 0.2f, 1.0f};
// Out-cubic as a cubic Bézier: the parts that change in place (headline, buttons).
constexpr float kSettle[4] = {0.33f, 1.0f, 0.68f, 1.0f};
// A card opening, folding, changing size (rows, a pill's words): with kEase over the stack's own move, so a card's far
// edge stays put as the window moves it and its clip.
constexpr double kOpenMs = 320, kFoldMs = 320, kRowsMs = 320;
constexpr double kTitleMs = 220;  // the headline changing
constexpr double kButtonsMs = 200;              // the buttons changing
constexpr double kDotMs = 220, kCheckMs = 260;  // the asking dot popping in, the done check growing in
constexpr double kShineMs = 1820;               // one sweep of the light over a working headline, and its pause
constexpr float kTitleRise = 8, kRowRise = 6, kButtonRise = 4;  // how far the parts rise as they change (DIPs)
constexpr float kOpenBlur = 6, kButtonBlur = 4;                  // their blur, a standard deviation (DIPs)
// 보내기 opening the row to type in, and the row folding back into it unsent:
// the button's shape growing into the box, and the other buttons going (and coming back) with a slight shrink.
constexpr float kSayEase[4] = {0.2f, 0.9f, 0.25f, 1.0f};
constexpr float kSayFade[4] = {0.2f, 0.8f, 0.2f, 1.0f};
constexpr double kSayOpenMs = 260, kSayFoldMs = 240;
constexpr float kSayShrink = 0.94f, kSayBlur = 2;

// A CSS cubic-bezier(b) at x (0-1), as Composition's cubic Bézier easing gives it.
inline float Bezier(const float (&b)[4], float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    auto at = [](float p1, float p2, float t) {
        float u = 1 - t;
        return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t;
    };
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 30; i++) {
        float v = at(b[0], b[2], t);
        if (std::fabs(v - x) < 1e-5f) break;
        (v < x ? lo : hi) = t;
        t = (lo + hi) / 2;
    }
    return at(b[1], b[3], t);
}
// The x (0-1) at which Bezier(b, x) is y: where on its way an eased change is when it has gone y of it.
inline float BezierInv(const float (&b)[4], float y) {
    if (y <= 0) return 0;
    if (y >= 1) return 1;
    float lo = 0, hi = 1;
    for (int i = 0; i < 30; i++) ((Bezier(b, (lo + hi) / 2) < y) ? lo : hi) = (lo + hi) / 2;
    return (lo + hi) / 2;
}

// 보내기's fades (SayMorph): the other buttons' (1 shown) and its word's, at `k` of the opening or (`back`) the folding.
// The others go in the first part of the opening and come back once the box has started shrinking; the word fades
// where it is, early in the opening, late in the folding.
struct SayFade {
    float others = 1, word = 1;
};
inline SayFade SayFades(bool back, float k) {
    auto part = [](float x, float from, float to) { return std::clamp((x - from) / (to - from), 0.0f, 1.0f); };
    if (back) return {Bezier(kSayFade, part(k, 0.2f, 1)), part(k, 0.55f, 1)};
    return {1 - Bezier(kSayFade, part(k, 0, 0.625f)), 1 - part(k, 0, 0.375f)};
}
// ... on a way that began turned back halfway at `turn` (its k then; < 0: not turned) from the fades `was`: they go on
// from those and settle onto the way's own over kSayTurn of it (by its end at the latest), so nothing jumps.
constexpr float kSayTurn = 0.4f;
inline SayFade SayFades(bool back, float k, float turn, SayFade was) {
    SayFade f = SayFades(back, k);
    if (turn < 0 || turn >= 1) return f;
    SayFade at = SayFades(back, turn);
    float left = 1 - Bezier(kSayFade, std::clamp((k - turn) / (std::min(1.0f, turn + kSayTurn) - turn), 0.0f, 1.0f));
    f.others = std::clamp(f.others + (was.others - at.others) * left, 0.0f, 1.0f);
    f.word = std::clamp(f.word + (was.word - at.word) * left, 0.0f, 1.0f);
    return f;
}
// 보내기's change turned back at `k` of its way (`back`: it was folding; `turn`, `was`: how that way began): the new
// way's k with the shape where it is, and the fades there for it to go on from.
struct SayTurned {
    float k;
    SayFade was;
};
inline SayTurned SayTurn(bool back, float k, float turn, SayFade was) {
    return {BezierInv(kSayEase, 1 - Bezier(kSayEase, k)), SayFades(back, k, turn, was)};
}
inline float Settle(float x) { return Bezier(kSettle, x); }
inline float Ease(float x) { return Bezier(kEase, x); }
// Past 1 a little and back: a dot popping in.
inline float Pop(float x) {
    float u = std::clamp(x, 0.0f, 1.0f) - 1;
    return 1 + 2.4f * u * u * u + 1.4f * u * u;
}

// One moment of the card's changes. Each progress runs from 0 to 1 over its time from when that change began, and is
// 1 at rest; the cards and requests a change began from are the window's, kept while it lasts.
struct Motion {
    float open = 1;  // how far the card is open, eased (kEase): its body under the header shows out of a blur
    float title = 1;  // the headline: the new one rises in as the old one (titleWas's) rises out
    const Card* titleWas = nullptr;
    const Ask* titleAsk = nullptr;
    float rows = 1;  // rows coming in (and the oldest shown going) and the row before turning done, from rowsWas's
    const Card* rowsWas = nullptr;
    float buttons = 1;  // the buttons: the new ones come up out of a blur as buttonsWas's go up into one
    const Card* buttonsWas = nullptr;
    const Ask* buttonsAsk = nullptr;
    // 보내기 pressed: the buttons' row (sayWas's) turning into the row to type in; sayBack: that row (sayWas's) folding
    // back into the buttons unsent. Linear in time; the parts ease on their own (SayMorph).
    float say = 1;
    bool sayBack = false;
    const Card* sayWas = nullptr;
    float sayTurn = -1;  // ... the way began turned back halfway, at this `say` (< 0: it did not; SayFades)
    SayFade sayTurnWas;  // ... from these fades
    float dot = 1, check = 1;  // the asking dot popping in; the done check growing in over the fading mark
    float size = 1;            // a pill going from wasW wide to its own width
    float wasW = 0;
    float pillW = 0;  // the pill a card opens from or folds into: the clock keeps to the clip's right edge
    bool shine = false;  // a working headline is drawn in its resting tone: the window sweeps the light over it
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
        float pager = a.questions.size() > 1 ? PagerW(a, int(a.questions.size()) - 1) + kPagerGap : 0;
        for (const Question& q : a.questions) {
            if (Lines(q.text, tw, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) > kQuestionLines) return false;
            if (TextW(QuestionTop(q), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD) > tw - pager) return false;
            for (const Choice& ch : q.choices) {
                if (Lines(ch.label, tw - 20, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) > kLabelLines) return false;
                if (Lines(ch.description, tw - 20, 11, DWRITE_FONT_WEIGHT_NORMAL) > kAboutLines) return false;
            }
        }
        return true;
    }

    // The card's height with Bloom's request on it: with several questions, shown one at a time, its tallest page.
    float AskH(const Card& c, const Ask& a, const Palette& p, Ms now, bool ko) {
        Ask page = a;
        float h = 0;
        for (size_t i = 0; i < std::max<size_t>(a.questions.size(), 1); i++) {
            page.page = int(i);
            h = std::max(h, Run(nullptr, c, &page, View::Card, p, now, {}, ko).h);
        }
        return h;
    }

    // Lays the card out and, with a dc, draws it at the dc's transform (DIPs). `ask` is Bloom's request for this
    // session (Phase::Approve), `now` the clock the times are read at, `motion` the changes under way (none: at rest).
    Laid Run(ID2D1DeviceContext* dc, const Card& c, const Ask* ask, View view, const Palette& p, Ms now,
             const Pointer& ptr, bool ko, bool ring = false, const Motion* motion = nullptr) {
        static const Motion kRest;
        const Motion& m = motion ? *motion : kRest;
        // The card's body goes under everything and its height is known only once laid out: measure first.
        float bodyH = view == View::Card && dc ? Run(nullptr, c, ask, view, p, now, ptr, ko, false, motion).h : kPillH;
        dc_ = dc;
        p_ = &p;
        ptr_ = ptr;
        ko_ = ko;
        alpha_ = 1;
        Laid L;
        if (dc_) {
            dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0), brush_.ReleaseAndGetAddressOf());
            dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        Phase phase = ask ? Phase::Approve : c.phase;
        std::wstring headline = Headline(c, ask, phase);
        // The clock runs while the turn or its subs work; then it says how long it all took.
        std::wstring clock = Clock((!c.turnEnd || SubsAtWork(c) > 0 ? now : std::max(*c.turnEnd, c.subsEnd)) - c.turnStart);

        // The pill, and the card's header: [mark] [icon] what is under way ............ 0:13
        // The card's header sits where the pill's does, so a pill opens into its card without a jump.
        float headerH = view == View::Pill ? kPillH : kHeaderH;
        float top = view == View::Pill ? 0 : (kPillH - kHeaderH) / 2;
        float clockW = TextW(clock, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
        float textX = TitleX(c, ask, phase);
        if (view == View::Pill) {
            float want = textX + TextW(headline, 13, DWRITE_FONT_WEIGHT_NORMAL) + 14 + clockW + kPad;
            L.w = samePill_ > 0 ? samePill_ : std::clamp(std::ceil(want), kPillMinW, kCardW);
            // A pill whose words changed width goes to its new width with the window's clip.
            if (m.size < 1 && m.wasW > 0) L.w = m.wasW + (L.w - m.wasW) * Ease(m.size);
            L.h = kPillH;
        } else {
            L.w = kCardW;
        }
        float markX = kMarkLeft + kMark / 2;
        float cy = top + headerH / 2;
        L.markAt = {markX, cy};
        // Opening or folding, the card shows as wide as its clip: the clock stays at the clip's right edge.
        float edge = view == View::Card && m.open < 1 && m.pillW > 0 ? m.pillW + (L.w - m.pillW) * m.open : L.w;
        float clockX = edge - kPad - clockW;
        L.title = {textX, cy - 10, clockX - 10, cy + 10};
        L.titleText = headline;
        if (dc_) {
            Body(L.w, view == View::Pill ? kPillH : bodyH, view == View::Pill ? kPillH / 2 : kRadius);
            // The mark pointed at is the card's close button (an X), on the pill too.
            if (ptr_.hover == Act::Close) CloseMark(L.markAt);
            else Mark(L.markAt, phase, c, ring, m);
            // The headline: a new one rises in as the old one rises out, inside the header's band. At rest a working
            // one is drawn in its resting tone for the light the window sweeps over it.
            dc_->PushAxisAlignedClip({markX + kMark / 2 + 7, 0, clockX - 6, kPillH}, D2D1_ANTIALIAS_MODE_ALIASED);
            if (m.title < 1 && m.titleWas) {
                float k = Settle(m.title);
                Phase was = m.titleAsk ? Phase::Approve : m.titleWas->phase;
                Faded(1 - k, -kTitleRise * k, 0, [&] { Title(*m.titleWas, m.titleAsk, was, cy, clockX, p.text); });
                Faded(k, kTitleRise * (1 - k), 0, [&] { Title(c, ask, phase, cy, clockX, p.text); });
            } else {
                // The light waits while the header's width moves (a pill resizing, a card opening or folding). A pill
                // folded back from a card (Motion::open left at 0) is at rest, so it has the light too.
                bool moving = m.size < 1 || (view == View::Card && m.open < 1);
                L.shine = m.shine && !moving && phase == Phase::Work && !c.asking && !headline.empty();
                Title(c, ask, phase, cy, clockX, L.shine ? p.shine : p.text);
            }
            dc_->PopAxisAlignedClip();
            Text(clock, clockX, cy - 9, clockW + 1, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.dim, true);
        }
        L.hits.push_back({{kMarkLeft - 3, cy - kMark / 2 - 3, kMarkLeft + kMark + 3, cy + kMark / 2 + 3}, Act::Close});
        L.hits.push_back({{0, top, L.w, top + headerH}, Act::Header});
        if (view == View::Pill) return L;

        // The body under the header comes out of a blur as the card opens, and goes back into it as it folds.
        float y = top + headerH + 6;
        float shown = std::clamp((m.open - 0.35f) / 0.65f, 0.0f, 1.0f);
        Faded(shown, 0, kOpenBlur * (1 - shown), [&] {
            // The model and the step.
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
            if (dc_) Bar(c, kPad, y, L.w - 2 * kPad, now, m);
            y += 3 + 10;

            // A sub's request or question: a line saying whose it is, over it.
            std::wstring whose = SubLine(c, ask);
            if (!whose.empty()) {
                if (dc_) {
                    Glyph(IconGlyph(Icon::Agent), kPad - 1, y + 1, 16, 16, p.faint, 12);
                    Text(whose, kPad + 20, y, L.w - 2 * kPad - 20, 18, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint);
                }
                y += 18 + 4;
            }
            if (phase == Phase::Approve && ask && !ask->questions.empty()) {
                y = Questions(*ask, y, L);
            } else if (phase == Phase::Approve && ask) {
                y = Request(*ask, y, L.w);
            } else {
                y = Rows(c, y, L.w, now, m);
            }

            // The buttons: changing, the new ones come up out of a blur as the old ones go up into one.
            y += 6;
            if (m.say < 1 && m.sayWas && SayMorph(L, y, c, ask, m)) {
                // 보내기's own change (SayMorph drew it).
            } else if (m.buttons < 1 && m.buttonsWas) {
                float k = Settle(m.buttons);
                Phase was = m.buttonsAsk ? Phase::Approve : m.buttonsWas->phase;
                Laid gone;  // the buttons going answer no clicks
                gone.w = L.w;
                Faded(1 - k, -kButtonRise * k, kButtonBlur * k, [&] { Buttons(gone, y, was, *m.buttonsWas, m.buttonsAsk); });
                Faded(k, kButtonRise * (1 - k), kButtonBlur * (1 - k), [&] { Buttons(L, y, phase, c, ask); });
            } else {
                Buttons(L, y, phase, c, ask);
            }
        });
        L.h = y + kBtnH + 12;
        return L;
    }

    // The headline as the card shows it (the window starts its change when it differs).
    std::wstring TitleOf(const Card& c, const Ask* ask, bool ko) {
        ko_ = ko;
        return Headline(c, ask, ask ? Phase::Approve : c.phase);
    }

    // Which buttons the card shows (the window animates a change between these).
    static int ButtonsOf(const Card& c, const Ask* ask) {
        Phase phase = ask ? Phase::Approve : c.phase;
        if (phase == Phase::Approve && ask && !ask->inRow) return ask->questions.empty() ? 1 : 2;
        if (c.composing) return 4;
        if (!c.note.empty()) return 5;
        // Stop stops the turn: none once it is over, though its subs work on.
        return (phase == Phase::Work && c.stoppable && !c.turnEnd ? 3 : 0) + (c.canSay ? 8 : 0);
    }

    // Whether the mark carries the dot of a session waiting for the person.
    static bool Dotted(const Card& c, const Ask* ask) {
        Phase phase = ask ? Phase::Approve : c.phase;
        return phase == Phase::Approve || phase == Phase::Ask || c.asking || !c.subAsk.empty();
    }

    // The line over a sub's request or question saying whose it is (the row open for a sub's question Bloom passed no
    // choices for says it too); empty for the card's own.
    static std::wstring SubLine(const Card& c, const Ask* ask) {
        if (ask) return ask->sub;
        return c.composing ? c.subAsk : std::wstring();
    }

    // The headline's words alone at (0, 0), as Run draws them in Laid::title (the window's light, drawn bright).
    void Words(ID2D1DeviceContext* dc, const Palette& p, const std::wstring& s, float w, float h, uint32_t argb) {
        dc_ = dc;
        p_ = &p;
        alpha_ = 1;
        dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0), brush_.ReleaseAndGetAddressOf());
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        Text(s, 0, 0, w, h, 13, DWRITE_FONT_WEIGHT_NORMAL, argb);
        dc_ = nullptr;
    }

    // Every pill as wide as `w` (pill.width = same: an open card's width, so opening one changes only its height; a
    // row's kRowPillW); 0: a pill is as wide as its words (kPillMinW to kCardW).
    void SamePill(float w) { samePill_ = w; }

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
    float samePill_ = kCardW;
    float alpha_ = 1;  // every colour's opacity, for a part fading (Faded)
    // The buttons' row drawn round 보내기 growing into the box (SayMorph): without the button it grows from, each of
    // the others shrunk about its middle by `shrink_`.
    Act skip_ = Act::None;
    float shrink_ = 1;
    const wchar_t* icons_;
    const wchar_t* mono_;
    ComPtr<ID2D1PathGeometry> claude_;  // kClaudeMark, made at the first draw

    // ---- words ----

    std::wstring Headline(const Card& c, const Ask* ask, Phase phase) const {
        bool ko = ko_;
        if (phase == Phase::Approve && ask) {
            if (!ask->questions.empty() && !ask->sub.empty())
                return ask->questions.size() == 1 ? Tr(ko, L"하위의 질문", L"A subtask has a question")
                                                  : Tr(ko, L"하위의 질문 " + Num(ask->questions.size()) + L"개",
                                                       L"A subtask has " + Num(ask->questions.size()) + L" questions");
            if (!ask->questions.empty())
                return ask->questions.size() == 1 ? Tr(ko, L"Claude의 질문", L"Claude has a question")
                                                  : Tr(ko, L"Claude의 질문 " + Num(ask->questions.size()) + L"개",
                                                       L"Claude has " + Num(ask->questions.size()) + L" questions");
            return Tr(ko, L"승인 요청 · ", L"Needs your approval · ") + ask->tool;
        }
        // The turn is over but subs it started still work: the card works on (WorkOn), or stays stopped, saying so.
        if (int subs = SubsAtWork(c); c.turnEnd && subs > 0 && phase != Phase::Ask) {
            std::wstring n = Num(subs);
            std::wstring en = n + (subs == 1 ? L" subtask running" : L" subtasks running");
            return phase == Phase::Stopped ? Tr(ko, L"중단됨 · 하위 " + n + L"개 진행 중", L"Stopped \x00B7 " + en)
                                           : Tr(ko, L"하위 " + n + L"개 진행 중", en);
        }
        if (phase == Phase::Done) {
            // How long it all took: the turn, and its subs' work after it.
            Ms took = std::max(c.turnEnd.value_or(c.turnStart), c.subsEnd) - c.turnStart;
            std::wstring done = Tr(ko, Duration(true, took, true) + L" 만에 완료", L"Done in " + Duration(false, took, true));
            if (c.filesChanged > 0)
                done += Tr(ko, L" · 파일 " + Num(c.filesChanged) + L"개 바뀜",
                           L" · " + Num(c.filesChanged) + (c.filesChanged == 1 ? L" file changed" : L" files changed"));
            return done;
        }
        if (phase == Phase::Stopped) return Tr(ko, L"중단됨", L"Stopped");
        const Step* s = Under(c);
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
        if (c.turnEnd && SubsAtWork(c) > 0) return Icon::Agent;  // "하위 2개 진행 중"
        if (const Step* s = Under(c)) return s->icon;
        return Icon::Think;
    }

    // Where the headline's words start: after the mark, and after the icon when there is one.
    static float TitleX(const Card& c, const Ask* ask, Phase phase) {
        return kMarkLeft + kMark + 9 + (HeadIcon(c, ask, phase) ? 16 + 7 : 0);
    }

    // The headline and the icon before it, its middle at `cy`, the words up to the clock at `clockX`.
    void Title(const Card& c, const Ask* ask, Phase phase, float cy, float clockX, uint32_t ink) {
        if (auto icon = HeadIcon(c, ask, phase)) Glyph(IconGlyph(*icon), kMarkLeft + kMark + 9, cy - 8, 16, 16, p_->dim, 14);
        float x = TitleX(c, ask, phase);
        Text(Headline(c, ask, phase), x, cy - 10, clockX - 10 - x, 20, 13, DWRITE_FONT_WEIGHT_NORMAL, ink);
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
        brush_->SetColor(si::draw_impl::Color(argb, opacity * alpha_));
    }

    // Direct2D's Gaussian blur (CLSID_D2D1GaussianBlur; its first property is the standard deviation, in DIPs).
    static constexpr GUID kBlurEffect{0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

    // Whatever `draw` draws, at `alpha`, `dy` lower and blurred by `blur` (a part changing). Blurred, it is recorded
    // in the card's own space and drawn through the blur at the dc's transform. Without a dc it only lays out.
    template <class F>
    void Faded(float alpha, float dy, float blur, F&& draw) {
        if (!dc_) {
            draw();
            return;
        }
        float was = alpha_;
        alpha_ = was * std::clamp(alpha, 0.0f, 1.0f);
        D2D1_MATRIX_3X2_F t;
        dc_->GetTransform(&t);
        ComPtr<ID2D1Image> target;
        ComPtr<ID2D1CommandList> list;
        ComPtr<ID2D1Effect> fx;
        if (blur >= 0.05f && SUCCEEDED(dc_->CreateEffect(kBlurEffect, &fx)) && SUCCEEDED(dc_->CreateCommandList(&list))) {
            dc_->GetTarget(&target);
            dc_->SetTarget(list.Get());
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, dy));
            draw();
            dc_->SetTarget(target.Get());
            dc_->SetTransform(t);
            if (SUCCEEDED(list->Close())) {
                fx->SetInput(0, list.Get());
                fx->SetValue(0u, blur);
                dc_->DrawImage(fx.Get());
            }
        } else {
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * t);
            draw();
            dc_->SetTransform(t);
        }
        alpha_ = was;
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
    // The mark as a close button: its disc a shade stronger, an X in it.
    void CloseMark(D2D1_POINT_2F at) {
        const Palette& p = *p_;
        Color(p.btnHover);
        dc_->FillEllipse({at, kMark / 2, kMark / 2}, brush_.Get());
        ComPtr<ID2D1StrokeStyle> round;
        d2d_->CreateStrokeStyle({D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                 10, D2D1_DASH_STYLE_SOLID, 0},
                                nullptr, 0, &round);
        Color(p.text);
        float r = 3.6f;
        dc_->DrawLine({at.x - r, at.y - r}, {at.x + r, at.y + r}, brush_.Get(), 1.6f, round.Get());
        dc_->DrawLine({at.x - r, at.y + r}, {at.x + r, at.y - r}, brush_.Get(), 1.6f, round.Get());
    }

    void Mark(D2D1_POINT_2F at, Phase phase, const Card& c, bool ring, const Motion& m) {
        const Palette& p = *p_;
        if (phase == Phase::Done) {
            // The check grows in (a little past its size and back) over the working mark fading.
            float k = std::clamp(m.check, 0.0f, 1.0f);
            if (k < 1) Faded(1 - std::min(1.0f, 2 * k), 0, 0, [&] {
                Color(p.mark);
                dc_->FillEllipse({at, kMark / 2, kMark / 2}, brush_.Get());
                Spark(at);
            });
            D2D1_MATRIX_3X2_F was;
            dc_->GetTransform(&was);
            float s = 0.6f + 0.4f * Pop(k);
            dc_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, at) * was);
            Faded(std::min(1.0f, 2 * k), 0, 0, [&] { Check(at); });
            dc_->SetTransform(was);
            return;
        }
        Color(p.mark);
        dc_->FillEllipse({at, kMark / 2, kMark / 2}, brush_.Get());
        if (phase == Phase::Stopped) {
            Fill({at.x - 3.5f, at.y - 3.5f, at.x + 3.5f, at.y + 3.5f}, 1.5f, p.faint);
            return;
        }
        Spark(at);
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
                dc_->DrawGeometry(arc.Get(), brush_.Get(), kRingW, Round().Get());
            }
        }
        if (phase == Phase::Approve || phase == Phase::Ask || c.asking || !c.subAsk.empty()) {
            // The dot pops in: a little past its size and back.
            float k = std::clamp(m.dot, 0.0f, 1.0f), s = std::max(0.0f, Pop(k));
            D2D1_POINT_2F d{at.x + 7.5f, at.y - 7.5f};
            Faded(std::min(1.0f, 3 * k), 0, 0, [&] {
                Color(p.light ? 0xFFFAFAFA : 0xFF27272A);
                dc_->FillEllipse({d, 4.6f * s, 4.6f * s}, brush_.Get());
                Color(p.accent);
                dc_->FillEllipse({d, 3.2f * s, 3.2f * s}, brush_.Get());
            });
        }
    }

    ComPtr<ID2D1StrokeStyle> Round() {
        ComPtr<ID2D1StrokeStyle> round;
        d2d_->CreateStrokeStyle({D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                 10, D2D1_DASH_STYLE_SOLID, 0},
                                nullptr, 0, &round);
        return round;
    }

    // Claude's mark, in the accent as the spark was (Bloom's, its dark or light tone).
    void Spark(D2D1_POINT_2F at) {
        if (!claude_) claude_ = si::draw_impl::ParsePath(d2d_, kClaudeMark);
        if (!claude_) return;
        constexpr float k = kClaudeSize / 24;
        D2D1_MATRIX_3X2_F was;
        dc_->GetTransform(&was);
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(at.x - 12 * k, at.y - 12 * k) * was);
        Color(p_->accent);
        dc_->FillGeometry(claude_.Get(), brush_.Get());
        dc_->SetTransform(was);
    }

    // The done mark: a ring and a tick in the ok colour.
    void Check(D2D1_POINT_2F at) {
        Color(p_->ok);
        dc_->DrawEllipse({at, kMark / 2 - 1.5f, kMark / 2 - 1.5f}, brush_.Get(), 1.5f);
        ComPtr<ID2D1PathGeometry> tick;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2d_->CreatePathGeometry(&tick)) && SUCCEEDED(tick->Open(&sink))) {
            sink->BeginFigure({at.x - 4.2f, at.y + 0.2f}, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine({at.x - 1.2f, at.y + 3.1f});
            sink->AddLine({at.x + 4.4f, at.y - 3.0f});
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            dc_->DrawGeometry(tick.Get(), brush_.Get(), 1.6f, Round().Get());
        }
    }

    // The bar of steps: a segment each (the oldest folded into the first when there are more than kSegments); the
    // finished ones in the accent, the running one filling. No segments for steps not yet taken: their number is
    // not known. A step coming in grows its segment from nothing as the one before fills up.
    void Bar(const Card& c, float x, float y, float w, Ms now, const Motion& m) {
        const Palette& p = *p_;
        int n = int(c.steps.size());
        int segs = std::clamp(n, 1, kSegments);
        const Card* was = RowsWas(c, m);
        float k = Ease(m.rows);
        int had = was ? std::clamp(int(was->steps.size()), 1, kSegments) : segs;
        const Step* wasCur = was && int(was->steps.size()) <= kSegments && !Over(*was) ? Current(*was) : nullptr;
        float weight[kSegments], total = 0, gaps = 0, gap = 4;
        for (int i = 0; i < segs; i++) {
            weight[i] = i >= had ? k : 1;
            total += weight[i];
            if (i > 0) gaps += gap * weight[i];
        }
        float unit = (w - gaps) / std::max(total, 0.001f);
        bool over = Over(c);
        const Step* cur = Current(c);
        float at = x;
        for (int i = 0; i < segs; i++) {
            if (i > 0) at += gap * weight[i];
            float sw = unit * weight[i];
            D2D1_RECT_F r{at, y, at + sw, y + 3};
            at += sw;
            if (sw < 0.5f) continue;
            Fill(r, 1.5f, p.track);
            float part = 1;
            if (n == 0) part = over ? 1.0f : 0.0f;
            else if (i == segs - 1 && cur && !over) part = Filling(now - cur->startedAt);
            else if (wasCur && i == had - 1) part = Filling(now - wasCur->startedAt) * (1 - k) + k;
            if (part <= 0) continue;
            uint32_t color = c.phase == Phase::Stopped ? p.faint : p.accent;
            Fill({r.left, r.top, r.left + std::min(sw, std::max(3.0f, sw * part)), r.bottom}, 1.5f, color);
        }
    }

    // How far a running step's segment is filled after `t`.
    static float Filling(Ms t) {
        double d = double(std::max<Ms>(t, 0));
        return float(d / (d + kHalfFillMs));
    }

    static bool Over(const Card& c) { return c.phase == Phase::Done || c.phase == Phase::Stopped; }

    // Whether a step's row shows it running: under way in a turn that runs, or a sub's while subs work on past the
    // card's turn.
    static bool Running(const Card& c, const Step& s) { return !s.endedAt && (!Over(c) || (s.sub && SubsAtWork(c) > 0)); }

    // The card the rows' change began from, while it lasts (none for another turn's, with more steps).
    static const Card* RowsWas(const Card& c, const Motion& m) {
        return m.rows < 1 && m.rowsWas && m.rowsWas->steps.size() <= c.steps.size() ? m.rowsWas : nullptr;
    }

    // The last steps: icon, words, time. Rows that came in grow from nothing as they rise in, the oldest shown
    // shrinks away above them, and a row that finished turns from its running words to its done ones.
    float Rows(const Card& c, float y, float w, Ms now, const Motion& m) {
        const Palette& p = *p_;
        int n = int(c.steps.size());
        if (n == 0) {
            if (dc_)
                Text(!Over(c) ? Tr(ko_, L"첫 단계를 준비하는 중", L"Getting ready") : Tr(ko_, L"도구를 쓰지 않은 답", L"Answered without tools"),
                     kPad + 24, y, w - 2 * kPad - 24, kRowH, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint);
            return y + kRowH;
        }
        const Card* was = RowsWas(c, m);
        float k = Ease(m.rows);
        int had = was ? int(was->steps.size()) : n;
        int from = std::max(0, n - kRows), wasFrom = std::max(0, had - kRows);
        for (int i = std::min(from, wasFrom); i < n; i++) {
            const Step& s = c.steps[i];
            bool running = Running(c, s);
            bool leaving = i < from, coming = i >= had;
            float h = leaving ? kRowH * (1 - k) : coming ? kRowH * k : kRowH;
            if (dc_ && h > 0.05f) {
                if (h < kRowH) dc_->PushAxisAlignedClip({0, y, w, y + h}, D2D1_ANTIALIAS_MODE_ALIASED);
                const Step* before = was && i < had ? &was->steps[i] : nullptr;
                bool ran = before && Running(*was, *before);
                if (leaving) {
                    Faded(1 - k, h - kRowH, 0, [&] { Row(s, running, y, w, now); });
                } else if (coming) {
                    Faded(k, kRowRise * (1 - k), 0, [&] { Row(s, running, y, w, now); });
                } else if (ran && !running) {
                    Faded(1 - k, 0, 0, [&] { Row(*before, true, y, w, now); });
                    Faded(k, 0, 0, [&] { Row(s, false, y, w, now); });
                } else {
                    Row(s, running, y, w, now);
                }
                if (h < kRowH) dc_->PopAxisAlignedClip();
            }
            y += h;
        }
        return y;
    }

    // One step's row at `y`: its icon, its words (running or done), its time. A sub's step is set in, its words led by
    // the sub's tag ("[S1]", faint).
    static constexpr float kSubIndent = 11;
    void Row(const Step& s, bool running, float y, float w, Ms now) {
        const Palette& p = *p_;
        std::wstring time = Duration(ko_, (s.endedAt ? *s.endedAt : now) - s.startedAt, running);
        if (!s.endedAt && !running) time.clear();
        float tw = TextW(time, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
        float in = s.sub ? kSubIndent : 0;
        uint32_t ic = s.failed ? p.danger : running ? p.faint : p.dim;
        Glyph(IconGlyph(s.icon), kPad - 1 + in, y + 4, 16, 16, ic, 13);
        float tx = kPad + 24 + in, tr = w - kPad - tw - 10;
        if (s.sub && s.subNo > 0) {
            std::wstring tag = L"[S" + Num(s.subNo) + L"]";
            float gw = TextW(tag, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
            Text(tag, tx, y, gw + 1, kRowH, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint, true);
            tx += gw + 5;
        }
        Text(running ? s.running : s.done, tx, y, tr - tx, kRowH, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
             running ? p.faint : s.failed ? p.danger : p.row);
        Text(time, w - kPad - tw, y, tw + 1, kRowH, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint, true);
    }

    // The buttons at `y`: the turn's own on the left (Stop while it works, Allow and Deny or Send while it asks, and
    // 보내기 on a session Bloom can send words to), Open on the right. Typing, the row is the box to type in alone.
    void Buttons(Laid& L, float y, Phase phase, const Card& c, const Ask* ask) {
        const Palette& p = *p_;
        bool ko = ko_;
        float bx = kPad - 2;
        bool request = phase == Phase::Approve && ask && !ask->inRow;
        if (!request && c.composing) {
            Field(L, y, phase, c, ask);
            return;
        }
        if (request && ask->questions.empty()) {
            bx = Button(L, bx, y, Tr(ko, L"허용", L"Allow"), L'\xE73E', Act::Allow, true, true);
            bx = Button(L, bx + 6, y, Tr(ko, L"거절", L"Deny"), L'\xE711', Act::Deny, false, true);
        } else if (request) {
            Button(L, bx, y, Tr(ko, L"보내기", L"Send"), L'\xE724', Act::Send, true, ask->Answered());
        } else if (!c.note.empty()) {
            // What came of a send, where its button was.
            Text(c.note, kPad, y + 5, L.w - 2 * kPad - 80, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, c.noteBad ? p.danger : p.dim);
        } else {
            if (phase == Phase::Work && c.stoppable) {
                // Where the design has Pause: Claude Code has no pause, Stop is its Esc.
                bx = Button(L, bx, y, Tr(ko, L"정지", L"Stop"), L'\xEE95', Act::Stop, false, !c.stopping) + 6;
            }
            if (c.canSay) Button(L, bx, y, ReplyWord(), kReplyGlyph, Act::Reply, false, true);
        }
        std::wstring open = request ? Tr(ko, L"터미널에서", L"In terminal") : Tr(ko, L"열기", L"Open");
        float openW = TextW(open, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) + 6 + 12 + 20;
        D2D1_RECT_F o{L.w - kPad + 6 - openW, y, L.w - kPad + 6, y + kBtnH};
        L.hits.push_back({o, Act::Open});
        if (dc_) Shrunk(o, [&] {
            if (ptr_.hover == Act::Open) Fill(o, kBtnH / 2, p.hover);
            Text(open, o.left + 10, y + 5, openW, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.text);
            Arrow(o.right - 10 - 12 + 2, y + kBtnH / 2, p.text);
        });
    }

    static constexpr wchar_t kReplyGlyph = L'\xE8BD', kSendGlyph = L'\xE724';  // 보내기 (Message), send (Send)
    std::wstring ReplyWord() const { return Tr(ko_, L"보내기", L"Send"); }

    // The row to type in's box at `y`, and its round send button at the end of a box `r`.
    static D2D1_RECT_F FieldBox(float w, float y) { return {kPad - 2, y, w - kPad + 2, y + kBtnH}; }
    static D2D1_RECT_F SendBox(D2D1_RECT_F r) {
        float go = kBtnH - 6;
        return {r.right - 3 - go, r.top + 3, r.right - 3, r.top + 3 + go};
    }
    // Whether the send button is lit: words typed, or a choice picked for every question of the T3 question over it.
    static bool Lit(const Card& c, const Ask* ask) { return c.typed || (ask && ask->inRow && ask->Answered()); }
    uint32_t FieldEdge(const Card& c) const { return c.focused ? p_->faint : p_->btnEdge; }

    // The round send button in `send`.
    void SendButton(D2D1_RECT_F send, bool lit) {
        const Palette& p = *p_;
        float go = send.right - send.left;
        Fill(send, go / 2, lit ? p.accent : p.track);
        if (lit && ptr_.hover == Act::Say) Fill(send, go / 2, p.light ? 0x1AFFFFFF : 0x1A000000);
        Glyph(kSendGlyph, send.left + (go - 12) / 2, send.top + (go - 12) / 2, 12, 12, lit ? p.accentInk : p.faint, 10);
    }

    // The row to type in: a box the width of the row, the window's edit box over its words (the words drawn here
    // while that box is elsewhere), a round send button at its end, lit once there are words, or a choice picked for
    // every question of the T3 question above it (`ask`: words typed answer it as Other).
    void Field(Laid& L, float y, Phase phase, const Card& c, const Ask* ask) {
        const Palette& p = *p_;
        bool ko = ko_;
        bool choices = ask && ask->inRow;
        bool lit = Lit(c, ask);
        D2D1_RECT_F r = FieldBox(L.w, y);
        D2D1_RECT_F send = SendBox(r);
        L.field = {r.left + 12, y + 3, send.left - 6, y + kBtnH - 3};
        L.hits.push_back({{r.left, r.top, send.left - 1, r.bottom}, Act::Field});
        if (lit) L.hits.push_back({send, Act::Say});
        if (!dc_) return;
        Fill(r, kBtnH / 2, p.btn);
        Color(FieldEdge(c));
        dc_->DrawRoundedRectangle({{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, kBtnH / 2 - 0.5f, kBtnH / 2 - 0.5f},
                                  brush_.Get(), 1);
        SendButton(send, lit);
        if (c.focused) return;  // the edit box shows the words
        std::wstring words = c.draft;
        std::replace(words.begin(), words.end(), L'\n', L' ');
        if (words.empty())
            words = choices                ? Tr(ko, L"기타 · 직접 답하기…", L"Other · type your answer…")
                    : !c.subAsk.empty()    ? Tr(ko, L"하위에 직접 답하기…", L"Type your answer to the subtask…")
                    : phase == Phase::Ask  ? Tr(ko, L"직접 답하기…", L"Type your answer…")
                    : phase == Phase::Work ? Tr(ko, L"지금 턴에 끼워 넣기…", L"Add to this turn…")
                                           : Tr(ko, L"이어서 보내기…", L"Send a follow-up…");
        Text(words, L.field.left, y + 5, L.field.right - L.field.left, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
             c.draft.empty() ? p.faint : p.text);
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

    // Several questions are shown one at a time: on the shown one's top line, at its right, ‹ 2 / 4 ›. The arrows are
    // round buttons standing out past the text's edge by kPagerOut; the count between them.
    static constexpr float kPageBtn = 22, kPagerOut = 5, kPagerGap = 8;
    std::wstring PageWords(const Ask& a, int page) const {
        return Num(page + 1) + L" / " + Num(static_cast<long long>(a.questions.size()));
    }
    float PageCountW(const Ask& a, int page) {
        return std::max(34.0f, TextW(PageWords(a, page), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, true) + 10);
    }
    // Its width from the text's right edge leftward.
    float PagerW(const Ask& a, int page) { return 2 * kPageBtn + PageCountW(a, page) - kPagerOut; }

    // The pager at the top line starting at `y` (text 16 high), its right edge at `right`.
    void Pager(const Ask& a, float right, float y, Laid& L) {
        const Palette& p = *p_;
        int n = int(a.questions.size()), page = a.page;
        float cw = PageCountW(a, page), top = y + 8 - kPageBtn / 2;
        D2D1_RECT_F next{right + kPagerOut - kPageBtn, top, right + kPagerOut, top + kPageBtn};
        D2D1_RECT_F prev{next.left - cw - kPageBtn, top, next.left - cw, top + kPageBtn};
        auto arrow = [&](D2D1_RECT_F r, int to, wchar_t glyph) {
            bool can = to >= 0 && to < n;
            if (can) L.hits.push_back({r, Act::Page, to});
            if (!dc_) return;
            bool hot = can && ptr_.hover == Act::Page && ptr_.q == to;
            if (hot) Fill(r, kPageBtn / 2, p.hover);
            uint32_t ink = !can ? (p.faint & 0x00FFFFFF) | 0x66000000 : hot ? p.text : p.dim;
            Glyph(glyph, r.left + (kPageBtn - 12) / 2, r.top + (kPageBtn - 12) / 2, 12, 12, ink, 10);
        };
        arrow(prev, page - 1, L'\xE76B');  // ChevronLeft
        arrow(next, page + 1, L'\xE76C');  // ChevronRight
        if (dc_) {
            std::wstring words = PageWords(a, page);
            float ww = TextW(words, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
            Text(words, prev.right + (cw - ww) / 2, y, ww + 1, 16, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, p.faint, true);
        }
    }

    // AskUserQuestion: the question shown (each, when there is one), its choices to pick (a ring for one, a box for
    // any).
    float Questions(const Ask& a, float y, Laid& L) {
        const Palette& p = *p_;
        float w = L.w, tw = w - 2 * kPad;
        bool paged = a.questions.size() > 1;
        size_t from = paged ? size_t(std::clamp(a.page, 0, int(a.questions.size()) - 1)) : 0;
        size_t to = paged ? from + 1 : a.questions.size();
        for (size_t qi = from; qi < to; qi++) {
            const Question& q = a.questions[qi];
            if (qi > from) y += 6;
            std::wstring top = QuestionTop(q);
            if (paged) {
                float pagerW = PagerW(a, a.page);
                if (dc_ && !top.empty()) Text(top, kPad, y, tw - pagerW - kPagerGap, 16, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, p.faint);
                Pager(a, w - kPad, y, L);
                y += 17;
            } else if (!top.empty()) {
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
                    // A picked choice's note a step stronger: the faint tone falls under 4.5:1 on the accent's tint.
                    if (about)
                        Wrapped(ch.description, kPad + 20, y + 4 + labelH - 1, tw - 20, 11, DWRITE_FONT_WEIGHT_NORMAL,
                                kAboutLines, 15, on ? p.dim : p.faint);
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
        float w = ButtonW(label);
        D2D1_RECT_F r{x, y, x + w, y + kBtnH};
        if (act == skip_) return r.right;
        if (enabled) L.hits.push_back({r, act});
        if (dc_) Shrunk(r, [&] {
            bool hot = enabled && ptr_.hover == act;
            uint32_t fill = primary ? (enabled ? p.accent : p.track) : hot ? p.btnHover : p.btn;
            uint32_t ink = primary ? (enabled ? p.accentInk : p.faint) : enabled ? p.text : p.faint;
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
        });
        return r.right;
    }

    float ButtonW(const std::wstring& label) { return 12 + 14 + 6 + TextW(label, 12.5f, DWRITE_FONT_WEIGHT_NORMAL) + 14; }

    // Whatever `draw` draws, shrunk by shrink_ about the middle of `r` (a button going, SayMorph).
    template <class F>
    void Shrunk(D2D1_RECT_F r, F&& draw) {
        if (shrink_ >= 1) {
            draw();
            return;
        }
        D2D1_MATRIX_3X2_F was;
        dc_->GetTransform(&was);
        D2D1_POINT_2F mid{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(shrink_, shrink_, mid) * was);
        draw();
        dc_->SetTransform(was);
    }

    // `a` to `b` by `t` (0-1), each channel.
    static uint32_t Mix(uint32_t a, uint32_t b, float t) {
        if (t <= 0) return a;
        if (t >= 1) return b;
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) {
            float x = float((a >> s) & 0xFF), z = float((b >> s) & 0xFF);
            out |= uint32_t(std::lround(x + (z - x) * t)) << s;
        }
        return out;
    }

    // 보내기 pressed: the button's shape grows left and right into the row to type in, its word fading
    // where it is, its glyph going along to the row's end, where it turns into the round send button; the other
    // buttons go with a slight shrink and blur. m.sayBack: the row folded unsent, the same played back (the box
    // shrinking into the button, the others coming back). No words in the box while it moves: the window's edit box
    // comes once the box has arrived and goes before it folds. The row keeps its height throughout; it answers clicks
    // as the end it is nearer does. False (nothing drawn) when the card is not between those two rows.
    bool SayMorph(Laid& L, float y, const Card& c, const Ask* ask, const Motion& m) {
        const Card& row = m.sayBack ? c : *m.sayWas;  // the buttons' row
        const Card& box = m.sayBack ? *m.sayWas : c;  // the row to type in
        if (row.composing || !box.composing) return false;
        Phase rowPhase = ask ? Phase::Approve : row.phase, boxPhase = ask ? Phase::Approve : box.phase;
        // Both ends laid out (no picture), and where 보내기 is among the buttons.
        Laid rowL, boxL;
        rowL.w = boxL.w = L.w;
        ID2D1DeviceContext* dc = dc_;
        dc_ = nullptr;
        Buttons(rowL, y, rowPhase, row, ask);
        Field(boxL, y, boxPhase, box, ask);
        dc_ = dc;
        auto reply = std::find_if(rowL.hits.begin(), rowL.hits.end(), [](const Hit& h) { return h.act == Act::Reply; });
        if (reply == rowL.hits.end()) return false;
        float k = std::clamp(m.say, 0.0f, 1.0f);
        float g = m.sayBack ? 1 - Bezier(kSayEase, k) : Bezier(kSayEase, k);  // 0 the button, 1 the box
        const Laid& nearer = g >= 0.5f ? boxL : rowL;
        L.hits.insert(L.hits.end(), nearer.hits.begin(), nearer.hits.end());
        L.field = nearer.field;
        if (!dc_) return true;

        const Palette& p = *p_;
        auto lerp = [g](float a, float b) { return a + (b - a) * g; };
        auto part = [](float x, float from, float to) { return std::clamp((x - from) / (to - from), 0.0f, 1.0f); };
        D2D1_RECT_F from = reply->r, to = FieldBox(L.w, y);
        // The fades (SayFades), from where they were when the change was turned back halfway.
        SayFade fades = SayFades(m.sayBack, k, m.sayTurn, m.sayTurnWas);
        float others = fades.others;
        if (others > 0.002f) {
            Laid gone;  // their clicks are rowL's
            gone.w = L.w;
            skip_ = Act::Reply;
            shrink_ = kSayShrink + (1 - kSayShrink) * others;
            Faded(others, 0, kSayBlur * (1 - others), [&] { Buttons(gone, y, rowPhase, row, ask); });
            skip_ = Act::None;
            shrink_ = 1;
        }
        // The shape: the button's rect, fill and edge to the box's.
        D2D1_RECT_F r{lerp(from.left, to.left), y, lerp(from.right, to.right), y + kBtnH};
        Fill(r, kBtnH / 2, Mix(ptr_.hover == Act::Reply ? p.btnHover : p.btn, p.btn, g));
        Color(Mix(p.btnEdge, FieldEdge(box), g));
        dc_->DrawRoundedRectangle({{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, kBtnH / 2 - 0.5f, kBtnH / 2 - 0.5f},
                                  brush_.Get(), 1);
        float word = fades.word;
        if (word > 0.002f)
            Faded(word, 0, 0, [&] {
                Text(ReplyWord(), from.left + 31, y + 5, from.right - from.left - 31, 18, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, p.text);
            });
        // The send button grows in at the box's end as the glyph lands in it.
        D2D1_RECT_F send = SendBox(r);
        float lands = part(g, 0.8f, 1);
        if (lands > 0.002f) {
            D2D1_MATRIX_3X2_F was;
            dc_->GetTransform(&was);
            float s = 0.8f + 0.2f * lands;
            if (s < 1)
                dc_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {(send.left + send.right) / 2, (send.top + send.bottom) / 2}) * was);
            Faded(lands, 0, 0, [&] { SendButton(send, Lit(box, ask)); });
            dc_->SetTransform(was);
        }
        // The glyph slides from the button to the send button's middle, fading as it lands.
        float glyph = 1 - part(g, 0.5f, 1);
        if (glyph > 0.002f) {
            D2D1_RECT_F end = SendBox(to);
            float cx = lerp(from.left + 11 + 7, (end.left + end.right) / 2), cy = y + kBtnH / 2;
            float box12 = lerp(14, 12);
            Faded(glyph, 0, 0, [&] { Glyph(kReplyGlyph, cx - box12 / 2, cy - box12 / 2, box12, box12, p.text, lerp(11.5f, 10)); });
        }
        return true;
    }
};

}  // namespace rc
