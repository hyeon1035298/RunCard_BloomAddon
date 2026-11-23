// SPDX-License-Identifier: GPL-3.0-only
//
// Run card test program: draws cards outside explorer, into a PNG, from invented sessions (no transcript is read).
// It shows each state the card has, dark and light, at twice the size, so the picture can be checked against the
// design it follows; and it checks a few facts about the steps (exit code 1 when one fails).
//
//   card-host.exe <out.png> [--ko|--en] [--accent RRGGBB]

#include "../mod/rc_draw.h"

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using rc::Ms;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failed++;
}

constexpr Ms kT0 = 1795400000000;  // the invented turn's start

si::Entry E(const char* tool, const wchar_t* label, double start, double end, si::Status status = si::Status::Ok) {
    si::Entry e;
    e.tool = tool;
    e.label = label;
    e.station = si::Station::Files;
    e.startedAt = kT0 + Ms(start * 1000);
    if (end >= 0) e.endedAt = kT0 + Ms(end * 1000);
    e.status = end >= 0 ? status : si::Status::Run;
    return e;
}

si::SessionView Session(bool done) {
    si::SessionView v;
    v.id = "fixture";
    v.title = L"billing";
    v.status = done ? si::SessionState::Idle : si::SessionState::Work;
    v.map.model = "claude-sonnet-5-5";
    v.map.turnStartedAt = kT0;
    auto& es = v.map.entries;
    si::Entry you;  // the turn's own first entry: not a tool, not a step
    you.station = si::Station::You;
    you.startedAt = kT0;
    es.push_back(you);
    const wchar_t* reads[] = {L"billing/invoice.ts", L"billing/plan.ts", L"billing/tax.ts", L"lib/money.ts"};
    for (int i = 0; i < 4; i++) es.push_back(E("Read", reads[i], 0.1 + i * 0.1, 1.9));
    for (int i = 0; i < 4; i++) es.push_back(E("Grep", L"Grep", 2.0 + i * 0.2, 3.4));
    es.push_back(E("Edit", L"billing/invoice.ts", 3.5, 6.0));
    const wchar_t* tests[] = {L"tests/invoice.test.ts", L"tests/plan.test.ts", L"tests/tax.test.ts"};
    for (int i = 0; i < 3; i++) es.push_back(E("Edit", tests[i], 6.1 + i * 0.3, 7.9));
    if (done) {
        es.push_back(E("Bash", L"Run unit tests", 8.0, 11.3));
        es.push_back(E("Bash", L"Push fix/proration-rounding", 11.5, 12.9));
        v.map.turnEndedAt = kT0 + 13200;
        v.map.filesChanged = 4;
    } else {
        es.push_back(E("Bash", L"Run unit tests", 8.0, -1));
    }
    return v;
}

void Fixtures(bool ko) {
    rc::Card work = rc::CardOf(Session(false), ko);
    Check(work.steps.size() == 4, "work: runs of one kind folded into four steps");
    Check(work.phase == rc::Phase::Work, "work: phase");
    Check(rc::Current(work) && rc::Current(work)->icon == rc::Icon::Shell, "work: the running step is the shell");
    Check(work.model == L"Claude Sonnet 5.5", "model name");
    rc::Card done = rc::CardOf(Session(true), ko);
    Check(done.phase == rc::Phase::Done && !rc::Current(done), "done: phase, nothing running");
    Check(rc::ModelName("claude-haiku-4-5-20251001") == L"Claude Haiku 4.5", "model name: date left out");
    Check(rc::ModelName("claude-opus-5-5[1m]") == L"Claude Opus 5.5", "model name: [1m] left out");
    Check(rc::Clock(13200) == L"0:13" && rc::Clock(3723000) == L"1:02:03", "clock");
    Check(rc::Duration(false, 1900, false) == L"1.9s" && rc::Duration(false, 65000, false) == L"1m 5s", "durations");
}

struct Shot {
    rc::Card card;
    std::optional<rc::Ask> ask;
    rc::View view;
    Ms now;
    rc::Pointer ptr;
};

bool SavePng(IWICImagingFactory* wic, IWICBitmap* bitmap, const wchar_t* path) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bitmap, nullptr)) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "card-host.exe <out.png> [--ko|--en] [--accent RRGGBB]\n");
        return 2;
    }
    bool ko = true;
    uint32_t accentDark = 0xFFE4E4E4, accentLight = 0xFF2E2E2E;  // Bloom's gray, the accent this PC uses
    for (int i = 2; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--en") ko = false;
        if (a == L"--ko") ko = true;
        if (a == L"--accent" && i + 1 < argc) accentDark = accentLight = 0xFF000000u | wcstoul(argv[++i], nullptr, 16);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Fixtures(ko);

    ComPtr<ID2D1Factory1> d2d;
    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                      reinterpret_cast<void**>(d2d.GetAddressOf()));
    ComPtr<IDWriteFactory> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!d2d || !dwrite || !wic) return 3;

    rc::Painter painter(d2d.Get(), dwrite.Get());
    rc::Card work = rc::CardOf(Session(false), ko);
    rc::Card done = rc::CardOf(Session(true), ko);
    rc::Card stopped = done;
    stopped.phase = rc::Phase::Stopped;
    rc::Ask approve;
    approve.id = "a1";
    approve.tool = L"Bash";
    // One long line, as a chained command comes: shown wrapped, whole.
    approve.body = L"command: git fetch origin && git rebase origin/main && npm test -- --runInBand --coverage=false && "
                   L"git push --force-with-lease origin fix/proration-rounding\ndescription: Rebase on main, test, push the fix";
    rc::Ask question;
    question.id = "q1";
    question.tool = L"AskUserQuestion";
    rc::Question q;
    q.header = ko ? L"반올림" : L"Rounding";
    q.text = ko ? L"일할 계산 금액은 어떻게 반올림할까요?" : L"How should prorated amounts round?";
    q.choices = {{ko ? L"센트 단위 반올림 (추천)" : L"Round to the cent (Recommended)",
                  ko ? L"지금 청구서와 같습니다" : L"Matches today's invoices"},
                 {ko ? L"내림" : L"Round down",
                  ko ? L"고객에게 조금 덜 청구합니다. 회계 쪽에서 월말에 차액을 한 번에 맞춰야 합니다"
                     : L"Bills customers slightly less; accounting squares the difference at the end of the month"},
                 {ko ? L"은행가 반올림" : L"Banker's rounding", L""}};
    question.questions = {q};
    question.picks = {{0}};

    // What the card shows whole, and what goes back to Bloom's card.
    Check(painter.Fits(approve, ko), "fits: a long one-line command, wrapped");
    rc::Ask huge = approve;
    huge.body.clear();
    for (int i = 0; i < 40; i++) huge.body += L"echo line " + std::to_wstring(i) + L" && ";
    Check(!painter.Fits(huge, ko), "fits: a command past the card's lines goes to Bloom");
    Check(painter.Fits(question, ko), "fits: the question");
    rc::Ask wordy = question;
    for (int i = 0; i < 6; i++) wordy.questions[0].text += L" " + wordy.questions[0].text;
    Check(!painter.Fits(wordy, ko), "fits: a question past three lines goes to Bloom");

    Ms now = kT0 + 10500;
    std::vector<std::vector<Shot>> rows = {
        {{work, std::nullopt, rc::View::Pill, now, {}}, {done, std::nullopt, rc::View::Pill, kT0 + 20000, {}}},
        {{work, std::nullopt, rc::View::Card, now, {}},
         {work, approve, rc::View::Card, now, {rc::Act::Allow}},
         {work, question, rc::View::Card, now, {}},
         {done, std::nullopt, rc::View::Card, kT0 + 20000, {rc::Act::Open}},
         {stopped, std::nullopt, rc::View::Card, kT0 + 20000, {}}},
    };
    constexpr float kScale = 2, kGap = 24, kMargin = 28;
    // Each theme a band: measure the band's height from its cards.
    float bandW = 0, bandH = 0;
    std::vector<float> rowH;
    rc::Palette measure = rc::MakePalette(false, accentDark);
    for (auto& row : rows) {
        float w = kMargin, h = 0;
        for (auto& s : row) {
            rc::Laid L = painter.Run(nullptr, s.card, s.ask ? &*s.ask : nullptr, s.view, measure, s.now, s.ptr, ko);
            w += L.w + kGap;
            h = std::max(h, L.h);
        }
        bandW = std::max(bandW, w - kGap + kMargin);
        rowH.push_back(h);
        bandH += h + kGap;
    }
    bandH += 2 * kMargin - kGap;
    UINT W = UINT(bandW * kScale), H = UINT(2 * bandH * kScale);

    ComPtr<IWICBitmap> bitmap;
    wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    ComPtr<ID2D1RenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    if (!bitmap || FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), rp, &rt))) return 4;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt.As(&dc))) return 5;
    dc->BeginDraw();
    for (int theme = 0; theme < 2; theme++) {
        bool light = theme == 1;
        rc::Palette p = rc::MakePalette(light, light ? accentLight : accentDark);
        float top = theme * bandH;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(kScale, kScale));
        ComPtr<ID2D1SolidColorBrush> ground;
        dc->CreateSolidColorBrush(si::draw_impl::Color(light ? 0xFFE6E6E9 : 0xFF141416), &ground);
        dc->FillRectangle({0, top, bandW, top + bandH}, ground.Get());
        float y = top + kMargin;
        for (size_t r = 0; r < rows.size(); r++) {
            float x = kMargin;
            for (auto& s : rows[r]) {
                dc->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * D2D1::Matrix3x2F::Scale(kScale, kScale));
                rc::Laid L = painter.Run(dc.Get(), s.card, s.ask ? &*s.ask : nullptr, s.view, p, s.now, s.ptr, ko, true);
                x += L.w + kGap;
            }
            y += rowH[r] + kGap;
        }
    }
    HRESULT hr = dc->EndDraw();
    if (FAILED(hr)) {
        std::fprintf(stderr, "draw failed: 0x%08lx\n", hr);
        return 6;
    }
    if (!SavePng(wic.Get(), bitmap.Get(), argv[1])) return 7;
    std::printf("wrote %ls (%ux%u)\n", argv[1], W, H);
    return g_failed ? 1 : 0;
}
