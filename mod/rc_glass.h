// SPDX-License-Identifier: GPL-3.0-only
//
// Run card: the glass's luminosity layer, as Windows' acrylic has one. The host backdrop (what is behind the window,
// blurred) gets a grey's luminosity mixed in while keeping its own hue and saturation, so a bright window behind a
// dark card comes out dark (and a dark one behind a light card light) and the card's text keeps reading on it, while
// the colours behind still show. Made of Direct2D effects that composition runs itself: Blend (luminosity) of a Flood
// over the backdrop, the Flood's alpha being how much of the luminosity goes in. The effect descriptions are written
// here instead of taken from Win2D: composition only asks each one its effect's CLSID, its properties by Direct2D's
// index, and its sources.

#pragma once

#include <windows.h>

#include <cstdint>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/base.h>

namespace rc::glass {

// windows.graphics.effects.interop.h (the Windows SDK's; llvm-mingw has none). The mapping argument is the SDK's
// GRAPHICS_EFFECT_PROPERTY_MAPPING, an int-sized enum.
#undef INTERFACE
#define INTERFACE IGraphicsEffectD2D1Interop
DECLARE_INTERFACE_IID_(IGraphicsEffectD2D1Interop, IUnknown, "2FC57384-A068-44D7-A331-30982FCF7177") {
    STDMETHOD(GetEffectId)(GUID * id) PURE;
    STDMETHOD(GetNamedPropertyMapping)(LPCWSTR name, UINT * index, int* mapping) PURE;
    STDMETHOD(GetPropertyCount)(UINT * count) PURE;
    STDMETHOD(GetProperty)(UINT index, void** value) PURE;  // ABI IPropertyValue
    STDMETHOD(GetSource)(UINT index, void** source) PURE;   // ABI IGraphicsEffectSource
    STDMETHOD(GetSourceCount)(UINT * count) PURE;
};
#undef INTERFACE

}  // namespace rc::glass

template <>
inline constexpr winrt::guid winrt::impl::guid_v<rc::glass::IGraphicsEffectD2D1Interop>{
    0x2FC57384, 0xA068, 0x44D7, {0xA3, 0x31, 0x30, 0x98, 0x2F, 0xCF, 0x71, 0x77}};

namespace rc::glass {

namespace wf = winrt::Windows::Foundation;
namespace wge = winrt::Windows::Graphics::Effects;
namespace wuc = winrt::Windows::UI::Composition;

// d2d1effects.h's CLSIDs, written out so nothing has to define them.
constexpr GUID kBlend{0x81c5b77b, 0x13f8, 0x4cdd, {0xad, 0x20, 0xc8, 0x90, 0x54, 0x7a, 0xc6, 0x5d}};
constexpr GUID kFlood{0x61c23c20, 0xae69, 0x4d8e, {0x94, 0xcf, 0x50, 0x07, 0x8d, 0xf6, 0x38, 0xf2}};
// The foreground's luminosity with the background's hue and saturation. Direct2D calls that 23
// (D2D1_BLEND_MODE_LUMINOSITY), but composition has 22 and 23 the other way round (WinUI's acrylic passes Color for
// it too): 22 is what does it here, measured (a white backdrop comes out as the Flood's grey).
constexpr uint32_t kLuminosity = 22;

// One effect: its CLSID, its properties in Direct2D's order (D2D1_BLEND_PROP_MODE, D2D1_FLOOD_PROP_COLOR, ...) and
// its inputs (Blend: 0 the background, 1 the foreground).
struct Effect : winrt::implements<Effect, wge::IGraphicsEffect, wge::IGraphicsEffectSource, IGraphicsEffectD2D1Interop> {
    Effect(const GUID& id, std::vector<wf::IPropertyValue> props, std::vector<wge::IGraphicsEffectSource> sources)
        : id_(id), props_(std::move(props)), sources_(std::move(sources)) {}

    winrt::hstring Name() const { return name_; }
    void Name(const winrt::hstring& name) { name_ = name; }

    STDMETHODIMP GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = id_;
        return S_OK;
    }
    // No property is named (nothing here is animated).
    STDMETHODIMP GetNamedPropertyMapping(LPCWSTR, UINT*, int*) noexcept override { return E_INVALIDARG; }
    STDMETHODIMP GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = UINT(props_.size());
        return S_OK;
    }
    STDMETHODIMP GetProperty(UINT index, void** value) noexcept override {
        if (!value) return E_POINTER;
        if (index >= props_.size()) return E_INVALIDARG;
        wf::IPropertyValue copy = props_[index];
        *value = winrt::detach_abi(copy);
        return S_OK;
    }
    STDMETHODIMP GetSource(UINT index, void** source) noexcept override {
        if (!source) return E_POINTER;
        if (index >= sources_.size()) return E_INVALIDARG;
        wge::IGraphicsEffectSource copy = sources_[index];
        *source = winrt::detach_abi(copy);
        return S_OK;
    }
    STDMETHODIMP GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = UINT(sources_.size());
        return S_OK;
    }

private:
    GUID id_;
    winrt::hstring name_;
    std::vector<wf::IPropertyValue> props_;
    std::vector<wge::IGraphicsEffectSource> sources_;
};

// The luminosity layer for each theme: a grey near the card's own ground, whose luminosity replaces the backdrop's
// by Amount (the Flood's alpha).
constexpr uint32_t kDarkGrey = 0x2C2C2C, kLightGrey = 0xFCFCFC;

// How much of the grey's luminosity goes in, by Bloom's 유리 투명도 (`see`, 0-100): 0.70 up to 35, then more as the
// card's own fill thins, 0.90 at 75 (the setting's top). With the fill at 65% (35), a white window behind a dark card
// comes out #353537 and a black one behind a light card #DCDCDC; at every setting the faintest text keeps 4.5:1.
inline float Amount(int see) {
    return 0.70f + 0.20f * std::clamp((see - 35) / 40.0f, 0.0f, 1.0f);
}

// The glass brush: `backdrop` (set as the source parameter "Backdrop") with the theme's luminosity layer at `amount`.
// Throws when composition can't make the effect.
inline wuc::CompositionEffectBrush Make(const wuc::Compositor& comp, const wuc::CompositionBrush& backdrop, bool light,
                                        float amount) {
    uint32_t grey = light ? kLightGrey : kDarkGrey;
    auto channel = [&](int shift) { return float((grey >> shift) & 0xFF) / 255.0f; };
    float color[4] = {channel(16), channel(8), channel(0), amount};  // straight alpha
    auto flood = winrt::make<Effect>(kFlood,
                                     std::vector<wf::IPropertyValue>{
                                         wf::PropertyValue::CreateSingleArray(color).as<wf::IPropertyValue>()},
                                     std::vector<wge::IGraphicsEffectSource>{});
    auto blend = winrt::make<Effect>(
        kBlend, std::vector<wf::IPropertyValue>{wf::PropertyValue::CreateUInt32(kLuminosity).as<wf::IPropertyValue>()},
        std::vector<wge::IGraphicsEffectSource>{wuc::CompositionEffectSourceParameter(L"Backdrop"),
                                                flood.as<wge::IGraphicsEffectSource>()});
    auto factory = comp.CreateEffectFactory(blend);
    auto status = factory.LoadStatus();
    if (status != wuc::CompositionEffectFactoryLoadStatus::Success &&
        status != wuc::CompositionEffectFactoryLoadStatus::Pending) {
        HRESULT why = factory.ExtendedError();
        winrt::throw_hresult(FAILED(why) ? why : E_FAIL);
    }
    auto brush = factory.CreateBrush();
    brush.SetSourceParameter(L"Backdrop", backdrop);
    return brush;
}

}  // namespace rc::glass
