#pragma once

#include "core/types.h"
#include "math/d2d_tex_backend.h"
#include "render/image_loader.h"

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <atomic>
#include <memory>

namespace mr {

// COM drawing effect attached to DirectWrite text ranges for per-span ink colors.
class SpanColorEffect : public IUnknown {
public:
    explicit SpanColorEffect(D2D1_COLOR_F color) : color_(color) {}

    D2D1_COLOR_F Color() const { return color_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    virtual ~SpanColorEffect() = default;
    std::atomic<ULONG> ref_count_{1};
    D2D1_COLOR_F color_;
};

// Inline LaTeX formula embedded inside an IDWriteTextLayout paragraph/heading/cell.
class InlineMathObject : public IDWriteInlineObject {
public:
    InlineMathObject(std::shared_ptr<tex::TeXRender> render, float text_size, tex::color fg);

    void UpdateStyle(float text_size, tex::color fg);

    HRESULT STDMETHODCALLTYPE Draw(
        void* clientDrawingContext,
        IDWriteTextRenderer* renderer,
        FLOAT originX,
        FLOAT originY,
        BOOL isSideways,
        BOOL isRightToLeft,
        IUnknown* clientDrawingEffect) override;

    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) override;
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) override;
    HRESULT STDMETHODCALLTYPE GetBreakConditions(
        DWRITE_BREAK_CONDITION* breakConditionBefore,
        DWRITE_BREAK_CONDITION* breakConditionAfter) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    virtual ~InlineMathObject() = default;
    std::atomic<ULONG> ref_count_{1};
    std::shared_ptr<tex::TeXRender> render_;
    float text_size_ = 16.0f;
    tex::color fg_ = tex::black;
};

// Inline image/badge embedded inside an IDWriteTextLayout paragraph or table cell.
class InlineImageObject : public IDWriteInlineObject {
public:
    InlineImageObject(std::shared_ptr<LoadedImage> image, float max_height, D2D1_COLOR_F fallback_color);

    void UpdateMaxHeight(float max_height, D2D1_COLOR_F fallback_color);

    HRESULT STDMETHODCALLTYPE Draw(
        void* clientDrawingContext,
        IDWriteTextRenderer* renderer,
        FLOAT originX,
        FLOAT originY,
        BOOL isSideways,
        BOOL isRightToLeft,
        IUnknown* clientDrawingEffect) override;

    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) override;
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) override;
    HRESULT STDMETHODCALLTYPE GetBreakConditions(
        DWRITE_BREAK_CONDITION* breakConditionBefore,
        DWRITE_BREAK_CONDITION* breakConditionAfter) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    virtual ~InlineImageObject() = default;
    std::atomic<ULONG> ref_count_{1};
    std::shared_ptr<LoadedImage> image_;
    float max_height_ = 22.0f;
    D2D1_COLOR_F fallback_color_;
};

// Custom DirectWrite text renderer that paints glyphs, underlines, strikethroughs, and inline math/images.
class D2DTextRenderer : public IDWriteTextRenderer {
public:
    D2DTextRenderer(ID2D1DeviceContext5* dc, D2D1_COLOR_F default_color);

    void SetDefaultColor(D2D1_COLOR_F color) { default_color_ = color; }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(
        void* clientDrawingContext, BOOL* isDisabled) override;
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(
        void* clientDrawingContext, DWRITE_MATRIX* transform) override;
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(
        void* clientDrawingContext, FLOAT* pixelsPerDip) override;

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void* clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE measuringMode,
        const DWRITE_GLYPH_RUN* glyphRun,
        const DWRITE_GLYPH_RUN_DESCRIPTION* glyphRunDescription,
        IUnknown* clientDrawingEffect) override;

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void* clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        const DWRITE_UNDERLINE* underline,
        IUnknown* clientDrawingEffect) override;

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void* clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        const DWRITE_STRIKETHROUGH* strikethrough,
        IUnknown* clientDrawingEffect) override;

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void* clientDrawingContext,
        FLOAT originX,
        FLOAT originY,
        IDWriteInlineObject* inlineObject,
        BOOL isSideways,
        BOOL isRightToLeft,
        IUnknown* clientDrawingEffect) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    virtual ~D2DTextRenderer() = default;
    D2D1_COLOR_F ResolveEffectColor(IUnknown* effect) const;

    std::atomic<ULONG> ref_count_{1};
    ID2D1DeviceContext5* dc_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    D2D1_COLOR_F default_color_;
};

}  // namespace mr
