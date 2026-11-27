#include "render/text_renderer.h"

#include <algorithm>
#include <cstring>

namespace mr {

// ============================================================================
// SpanColorEffect
// ============================================================================

HRESULT STDMETHODCALLTYPE SpanColorEffect::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE SpanColorEffect::AddRef() {
    return ++ref_count_;
}

ULONG STDMETHODCALLTYPE SpanColorEffect::Release() {
    ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// ============================================================================
// InlineMathObject
// ============================================================================

InlineMathObject::InlineMathObject(std::shared_ptr<tex::TeXRender> render, float text_size, tex::color fg)
    : render_(std::move(render)), text_size_(text_size), fg_(fg) {}

void InlineMathObject::UpdateStyle(float text_size, tex::color fg) {
    text_size_ = text_size;
    fg_ = fg;
}

HRESULT STDMETHODCALLTYPE InlineMathObject::Draw(
    void* clientDrawingContext,
    IDWriteTextRenderer* /*renderer*/,
    FLOAT originX,
    FLOAT originY,
    BOOL /*isSideways*/,
    BOOL /*isRightToLeft*/,
    IUnknown* clientDrawingEffect) {
    if (!render_) return S_OK;
    auto* dc = static_cast<ID2D1DeviceContext5*>(clientDrawingContext);
    if (!dc) return E_INVALIDARG;

    tex::color draw_color = fg_;
    if (clientDrawingEffect) {
        // Check if inside a link/colored span
        auto* eff = static_cast<SpanColorEffect*>(clientDrawingEffect);
        D2D1_COLOR_F c = eff->Color();
        draw_color = tex::argb(c.a, c.r, c.g, c.b);
    }

    render_->setTextSize(text_size_);
    render_->setForeground(draw_color);

    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    D2D1::Matrix3x2F shift = D2D1::Matrix3x2F::Translation(originX + 2.0f, originY);
    dc->SetTransform(shift * saved);

    Graphics2D_d2d g2(dc);
    render_->draw(g2, 0, 0);

    dc->SetTransform(saved);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineMathObject::GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) {
    if (!metrics) return E_POINTER;
    if (!render_) {
        *metrics = {4.0f, text_size_, text_size_ * 0.8f, FALSE};
        return S_OK;
    }
    render_->setTextSize(text_size_);
    const float w = std::max(2.0f, static_cast<float>(render_->getWidth())) + 4.0f;
    const float h = std::max(4.0f, static_cast<float>(render_->getHeight()));
    const float baseline = std::clamp(h * render_->getBaseline(), 1.0f, h);

    metrics->width = w;
    metrics->height = h;
    metrics->baseline = baseline;
    metrics->supportsSideways = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineMathObject::GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) {
    if (!overhangs) return E_POINTER;
    *overhangs = {2.0f, 2.0f, 2.0f, 2.0f};
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineMathObject::GetBreakConditions(
    DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) {
    if (before) *before = DWRITE_BREAK_CONDITION_CAN_BREAK;
    if (after) *after = DWRITE_BREAK_CONDITION_CAN_BREAK;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineMathObject::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
        *ppv = static_cast<IDWriteInlineObject*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE InlineMathObject::AddRef() {
    return ++ref_count_;
}

ULONG STDMETHODCALLTYPE InlineMathObject::Release() {
    ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// ============================================================================
// InlineImageObject
// ============================================================================

InlineImageObject::InlineImageObject(
    std::shared_ptr<LoadedImage> image, float max_height, D2D1_COLOR_F fallback_color)
    : image_(std::move(image)), max_height_(max_height), fallback_color_(fallback_color) {}

void InlineImageObject::UpdateMaxHeight(float max_height, D2D1_COLOR_F fallback_color) {
    max_height_ = max_height;
    fallback_color_ = fallback_color;
}

HRESULT STDMETHODCALLTYPE InlineImageObject::Draw(
    void* clientDrawingContext,
    IDWriteTextRenderer* /*renderer*/,
    FLOAT originX,
    FLOAT originY,
    BOOL /*isSideways*/,
    BOOL /*isRightToLeft*/,
    IUnknown* /*clientDrawingEffect*/) {
    auto* dc = static_cast<ID2D1DeviceContext5*>(clientDrawingContext);
    if (!dc) return E_INVALIDARG;

    DWRITE_INLINE_OBJECT_METRICS m{};
    GetMetrics(&m);
    D2D1_RECT_F dest = D2D1::RectF(originX, originY, originX + m.width, originY + m.height);

    if (image_ && image_->status == LoadedImage::Status::Ready) {
        DrawLoadedImage(dc, *image_, dest);
        return S_OK;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(fallback_color_, brush.Put()))) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(dest, 3.0f, 3.0f), brush.Get(), 1.0f);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineImageObject::GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) {
    if (!metrics) return E_POINTER;
    float w = max_height_;
    float h = max_height_;
    if (image_ && image_->status == LoadedImage::Status::Ready && image_->width > 0.0f && image_->height > 0.0f) {
        if (image_->height <= max_height_) {
            w = image_->width;
            h = image_->height;
        } else {
            float scale = max_height_ / image_->height;
            w = image_->width * scale;
            h = max_height_;
        }
    }
    metrics->width = std::max(4.0f, w);
    metrics->height = std::max(4.0f, h);
    metrics->baseline = metrics->height * 0.85f;
    metrics->supportsSideways = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineImageObject::GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) {
    if (!overhangs) return E_POINTER;
    *overhangs = {0, 0, 0, 0};
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineImageObject::GetBreakConditions(
    DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) {
    if (before) *before = DWRITE_BREAK_CONDITION_CAN_BREAK;
    if (after) *after = DWRITE_BREAK_CONDITION_CAN_BREAK;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE InlineImageObject::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
        *ppv = static_cast<IDWriteInlineObject*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE InlineImageObject::AddRef() {
    return ++ref_count_;
}

ULONG STDMETHODCALLTYPE InlineImageObject::Release() {
    ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// ============================================================================
// D2DTextRenderer
// ============================================================================

D2DTextRenderer::D2DTextRenderer(ID2D1DeviceContext5* dc, D2D1_COLOR_F default_color)
    : dc_(dc), default_color_(default_color) {
    if (dc_) {
        dc_->CreateSolidColorBrush(default_color_, brush_.Put());
    }
}

D2D1_COLOR_F D2DTextRenderer::ResolveEffectColor(IUnknown* effect) const {
    if (!effect) return default_color_;
    return static_cast<SpanColorEffect*>(effect)->Color();
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::IsPixelSnappingDisabled(
    void* /*clientDrawingContext*/, BOOL* isDisabled) {
    if (!isDisabled) return E_POINTER;
    *isDisabled = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::GetCurrentTransform(
    void* /*clientDrawingContext*/, DWRITE_MATRIX* transform) {
    if (!transform) return E_POINTER;
    D2D1_MATRIX_3X2_F m{};
    dc_->GetTransform(&m);
    std::memcpy(transform, &m, sizeof(DWRITE_MATRIX));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::GetPixelsPerDip(
    void* /*clientDrawingContext*/, FLOAT* pixelsPerDip) {
    if (!pixelsPerDip) return E_POINTER;
    float dx = 96.0f, dy = 96.0f;
    dc_->GetDpi(&dx, &dy);
    *pixelsPerDip = dx / 96.0f;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::DrawGlyphRun(
    void* /*clientDrawingContext*/,
    FLOAT baselineOriginX,
    FLOAT baselineOriginY,
    DWRITE_MEASURING_MODE measuringMode,
    const DWRITE_GLYPH_RUN* glyphRun,
    const DWRITE_GLYPH_RUN_DESCRIPTION* /*glyphRunDescription*/,
    IUnknown* clientDrawingEffect) {
    if (!dc_ || !brush_ || !glyphRun) return S_OK;
    brush_->SetColor(ResolveEffectColor(clientDrawingEffect));
    dc_->DrawGlyphRun(
        D2D1::Point2F(baselineOriginX, baselineOriginY),
        glyphRun,
        brush_.Get(),
        measuringMode);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::DrawUnderline(
    void* /*clientDrawingContext*/,
    FLOAT baselineOriginX,
    FLOAT baselineOriginY,
    const DWRITE_UNDERLINE* underline,
    IUnknown* clientDrawingEffect) {
    if (!dc_ || !brush_ || !underline) return S_OK;
    brush_->SetColor(ResolveEffectColor(clientDrawingEffect));
    D2D1_RECT_F rect = D2D1::RectF(
        baselineOriginX,
        baselineOriginY + underline->offset,
        baselineOriginX + underline->width,
        baselineOriginY + underline->offset + std::max(1.0f, underline->thickness));
    dc_->FillRectangle(rect, brush_.Get());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::DrawStrikethrough(
    void* /*clientDrawingContext*/,
    FLOAT baselineOriginX,
    FLOAT baselineOriginY,
    const DWRITE_STRIKETHROUGH* strikethrough,
    IUnknown* clientDrawingEffect) {
    if (!dc_ || !brush_ || !strikethrough) return S_OK;
    brush_->SetColor(ResolveEffectColor(clientDrawingEffect));
    D2D1_RECT_F rect = D2D1::RectF(
        baselineOriginX,
        baselineOriginY + strikethrough->offset,
        baselineOriginX + strikethrough->width,
        baselineOriginY + strikethrough->offset + std::max(1.0f, strikethrough->thickness));
    dc_->FillRectangle(rect, brush_.Get());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::DrawInlineObject(
    void* clientDrawingContext,
    FLOAT originX,
    FLOAT originY,
    IDWriteInlineObject* inlineObject,
    BOOL isSideways,
    BOOL isRightToLeft,
    IUnknown* clientDrawingEffect) {
    if (!inlineObject) return S_OK;
    return inlineObject->Draw(
        clientDrawingContext ? clientDrawingContext : dc_,
        this,
        originX,
        originY,
        isSideways,
        isRightToLeft,
        clientDrawingEffect);
}

HRESULT STDMETHODCALLTYPE D2DTextRenderer::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
        riid == __uuidof(IDWriteTextRenderer)) {
        *ppv = static_cast<IDWriteTextRenderer*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D2DTextRenderer::AddRef() {
    return ++ref_count_;
}

ULONG STDMETHODCALLTYPE D2DTextRenderer::Release() {
    ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

const wchar_t* PickMonoFontFamily(IDWriteFactory5* dw) {
    static std::wstring cached;
    if (!cached.empty()) return cached.c_str();
    cached = L"Consolas";
    if (!dw) return cached.c_str();
    ComPtr<IDWriteFontCollection1> sys;
    if (SUCCEEDED(dw->GetSystemFontCollection(FALSE, sys.Put(), FALSE)) && sys) {
        for (const wchar_t* cand : {L"Cascadia Code", L"Cascadia Mono", L"JetBrains Mono", L"Consolas"}) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(cand, &idx, &exists)) && exists) {
                cached = cand;
                break;
            }
        }
    }
    return cached.c_str();
}

const wchar_t* PickUiFontFamily(IDWriteFactory5* dw) {
    static std::wstring cached;
    if (!cached.empty()) return cached.c_str();
    cached = L"Segoe UI";
    if (!dw) return cached.c_str();
    ComPtr<IDWriteFontCollection1> sys;
    if (SUCCEEDED(dw->GetSystemFontCollection(FALSE, sys.Put(), FALSE)) && sys) {
        for (const wchar_t* cand : {L"Segoe UI Variable Text", L"Segoe UI", L"Microsoft YaHei UI"}) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(cand, &idx, &exists)) && exists) {
                cached = cand;
                break;
            }
        }
    }
    return cached.c_str();
}

}  // namespace mr
