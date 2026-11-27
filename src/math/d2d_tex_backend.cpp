#include "math/d2d_tex_backend.h"
#include "render/d2d_context.h"
#include "latex.h"
#include "core/formula.h"

#include <cmath>
#include <filesystem>
#include <mutex>

namespace {

std::unordered_map<std::string, mr::ComPtr<IDWriteFontFace>>& FontFaceCache() {
    static std::unordered_map<std::string, mr::ComPtr<IDWriteFontFace>> cache;
    return cache;
}

struct GlyphKey {
    IDWriteFontFace* face = nullptr;
    uint16_t glyph_index = 0;

    bool operator==(const GlyphKey& o) const {
        return face == o.face && glyph_index == o.glyph_index;
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const {
        return std::hash<void*>{}(k.face) ^ (static_cast<size_t>(k.glyph_index) * 2654435761u);
    }
};

std::unordered_map<GlyphKey, mr::ComPtr<ID2D1PathGeometry>, GlyphKeyHash>& GlyphGeomCache() {
    static std::unordered_map<GlyphKey, mr::ComPtr<ID2D1PathGeometry>, GlyphKeyHash> cache;
    return cache;
}

mr::ComPtr<ID2D1StrokeStyle>& MathOutlineStrokeStyle() {
    static mr::ComPtr<ID2D1StrokeStyle> style;
    return style;
}

ID2D1PathGeometry* GetOrCreateGlyphOutline(IDWriteFontFace* face, uint16_t glyph_index, float em_size) {
    auto& cache = GlyphGeomCache();
    GlyphKey key{face, glyph_index};
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second.Get();
    }
    auto* ctx = mr::D2DContext::Instance();
    if (!ctx || !ctx->D2DFactory() || !face) return nullptr;

    mr::ComPtr<ID2D1PathGeometry> geom;
    if (FAILED(ctx->D2DFactory()->CreatePathGeometry(geom.Put())) || !geom) return nullptr;

    mr::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geom->Open(sink.Put())) || !sink) return nullptr;

    float advance = 0.0f;
    DWRITE_GLYPH_OFFSET offset{0.0f, 0.0f};
    HRESULT hr = face->GetGlyphRunOutline(
        em_size, &glyph_index, &advance, &offset, 1, FALSE, FALSE, sink.Get());
    sink->Close();
    if (FAILED(hr)) return nullptr;

    if (cache.size() > 4096) cache.clear();
    ID2D1PathGeometry* raw = geom.Get();
    cache.emplace(key, std::move(geom));
    return raw;
}

D2D1_COLOR_F TexColorToD2D(tex::color c) {
    return D2D1::ColorF(
        static_cast<float>(tex::color_r(c)) / 255.0f,
        static_cast<float>(tex::color_g(c)) / 255.0f,
        static_cast<float>(tex::color_b(c)) / 255.0f,
        static_cast<float>(tex::color_a(c)) / 255.0f);
}

std::wstring MapLogicalFontFamily(const std::string& name) {
    if (name == "Serif" || name.empty()) return L"Cambria";
    if (name == "SansSerif") return L"Segoe UI";
    if (name == "Monospaced" || name == "Typewriter") return L"Cascadia Mono";
    return mr::Utf8ToWide(name);
}

}  // namespace

// ============================================================================
// tex::Font -> DirectWrite IDWriteFontFace / Family
// ============================================================================

namespace tex {

Font* Font::create(const std::string& file, float size) {
    return new mr::Font_d2d(file, size);
}

sptr<Font> Font::_create(const std::string& name, int style, float size) {
    return sptrOf<mr::Font_d2d>(name, style, size);
}

sptr<TextLayout> TextLayout::create(const std::wstring& src, const sptr<Font>& font) {
    auto f = std::static_pointer_cast<mr::Font_d2d>(font);
    return sptrOf<mr::TextLayout_d2d>(src, f);
}

}  // namespace tex

namespace mr {

Font_d2d::Font_d2d(const std::string& file, float size)
    : size_(size), is_file_font_(true) {
    auto* ctx = D2DContext::Instance();
    if (!ctx || !ctx->DWrite()) return;

    auto& cache = FontFaceCache();
    auto it = cache.find(file);
    if (it != cache.end()) {
        font_face_ = it->second;
        return;
    }

    std::wstring wfile = Utf8ToWide(file);
    ComPtr<IDWriteFontFile> font_file;
    HRESULT hr = ctx->DWrite()->CreateFontFileReference(wfile.c_str(), nullptr, font_file.Put());
    if (FAILED(hr)) return;

    BOOL is_supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_TRUETYPE;
    UINT32 num_faces = 0;
    hr = font_file->Analyze(&is_supported, &file_type, &face_type, &num_faces);
    if (FAILED(hr) || !is_supported) {
        face_type = DWRITE_FONT_FACE_TYPE_TRUETYPE;
    }

    IDWriteFontFile* files[] = {font_file.Get()};
    hr = ctx->DWrite()->CreateFontFace(
        face_type, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, font_face_.Put());
    if (SUCCEEDED(hr) && font_face_) {
        cache[file] = font_face_;
    }
}

Font_d2d::Font_d2d(const std::string& name, int style, float size)
    : family_(MapLogicalFontFamily(name)), style_(style), size_(size), is_file_font_(false) {}

float Font_d2d::getSize() const {
    return size_;
}

tex::sptr<tex::Font> Font_d2d::deriveFont(int style) const {
    auto* f = new Font_d2d();
    f->font_face_ = font_face_;
    f->family_ = family_.empty() ? L"Cambria" : family_;
    f->style_ = style;
    f->size_ = size_;
    f->is_file_font_ = is_file_font_;
    return tex::sptr<tex::Font>(f);
}

bool Font_d2d::operator==(const tex::Font& other) const {
    const auto& o = static_cast<const Font_d2d&>(other);
    return size_ == o.size_ && style_ == o.style_ &&
           font_face_.Get() == o.font_face_.Get() && family_ == o.family_;
}

bool Font_d2d::operator!=(const tex::Font& other) const {
    return !(*this == other);
}

// ============================================================================
// tex::TextLayout -> IDWriteTextLayout (for \text{...} & Unicode fallback)
// ============================================================================

TextLayout_d2d::TextLayout_d2d(const std::wstring& src, const tex::sptr<Font_d2d>& font) {
    auto* ctx = D2DContext::Instance();
    if (!ctx || !ctx->DWrite() || !font) return;

    const std::wstring family = font->Family().empty() ? L"Cambria" : font->Family();
    DWRITE_FONT_WEIGHT weight = (font->Style() & tex::BOLD) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE fstyle = (font->Style() & tex::ITALIC) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = ctx->DWrite()->CreateTextFormat(
        family.c_str(), nullptr, weight, fstyle, DWRITE_FONT_STRETCH_NORMAL,
        std::max(1.0f, font->getSize()), L"", fmt.Put());
    if (FAILED(hr) || !fmt) return;

    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    hr = ctx->DWrite()->CreateTextLayout(
        src.c_str(), static_cast<UINT32>(src.size()), fmt.Get(),
        100000.0f, 100000.0f, layout_.Put());
    if (FAILED(hr) || !layout_) return;

    DWRITE_LINE_METRICS lm{};
    UINT32 line_count = 0;
    if (SUCCEEDED(layout_->GetLineMetrics(&lm, 1, &line_count)) && line_count > 0) {
        baseline_ = lm.baseline;
    }

    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(layout_->GetMetrics(&tm))) {
        bounds_.x = tm.left;
        bounds_.y = -baseline_;
        bounds_.w = tm.widthIncludingTrailingWhitespace;
        bounds_.h = tm.height;
    }
}

void TextLayout_d2d::getBounds(tex::Rect& bounds) {
    bounds = bounds_;
}

void TextLayout_d2d::draw(tex::Graphics2D& g2, float x, float y) {
    if (!layout_) return;
    auto& gd2d = static_cast<Graphics2D_d2d&>(g2);
    gd2d.D2D()->DrawTextLayout(
        D2D1::Point2F(x, y - baseline_),
        layout_.Get(),
        gd2d.Brush(),
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

// ============================================================================
// tex::Graphics2D -> ID2D1DeviceContext5
// ============================================================================

Graphics2D_d2d::Graphics2D_d2d(ID2D1DeviceContext5* dc)
    : dc_(dc) {
    dc_->GetTransform(&base_transform_);
    local_transform_ = D2D1::Matrix3x2F::Identity();
    dc_->CreateSolidColorBrush(TexColorToD2D(color_), brush_.Put());
    SyncStrokeStyle();
}

void Graphics2D_d2d::SyncTransform() {
    D2D1_MATRIX_3X2_F total = local_transform_ * base_transform_;
    dc_->SetTransform(total);
}

void Graphics2D_d2d::SyncStrokeStyle() {
    if (!stroke_dirty_) return;
    stroke_dirty_ = false;
    auto* ctx = D2DContext::Instance();
    if (!ctx || !ctx->D2DFactory()) return;

    D2D1_CAP_STYLE cap = D2D1_CAP_STYLE_FLAT;
    switch (stroke_.cap) {
        case tex::CAP_BUTT: cap = D2D1_CAP_STYLE_FLAT; break;
        case tex::CAP_ROUND: cap = D2D1_CAP_STYLE_ROUND; break;
        case tex::CAP_SQUARE: cap = D2D1_CAP_STYLE_SQUARE; break;
    }
    D2D1_LINE_JOIN join = D2D1_LINE_JOIN_MITER;
    switch (stroke_.join) {
        case tex::JOIN_BEVEL: join = D2D1_LINE_JOIN_BEVEL; break;
        case tex::JOIN_MITER: join = D2D1_LINE_JOIN_MITER; break;
        case tex::JOIN_ROUND: join = D2D1_LINE_JOIN_ROUND; break;
    }
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        cap, cap, cap, join, stroke_.miterLimit > 0.0f ? stroke_.miterLimit : 10.0f);
    stroke_style_.Reset();
    ctx->D2DFactory()->CreateStrokeStyle(&props, nullptr, 0, stroke_style_.Put());
}

void Graphics2D_d2d::setColor(tex::color c) {
    color_ = c;
    if (brush_) {
        brush_->SetColor(TexColorToD2D(c));
    }
}

tex::color Graphics2D_d2d::getColor() const {
    return color_;
}

void Graphics2D_d2d::setStroke(const tex::Stroke& s) {
    stroke_ = s;
    stroke_dirty_ = true;
}

const tex::Stroke& Graphics2D_d2d::getStroke() const {
    return stroke_;
}

void Graphics2D_d2d::setStrokeWidth(float w) {
    stroke_.lineWidth = w;
}

const tex::Font* Graphics2D_d2d::getFont() const {
    return font_;
}

void Graphics2D_d2d::setFont(const tex::Font* font) {
    font_ = font;
}

void Graphics2D_d2d::translate(float dx, float dy) {
    local_transform_ = D2D1::Matrix3x2F::Translation(dx, dy) * local_transform_;
    SyncTransform();
}

void Graphics2D_d2d::scale(float sx, float sy) {
    sx_ *= sx;
    sy_ *= sy;
    local_transform_ = D2D1::Matrix3x2F::Scale(sx, sy) * local_transform_;
    SyncTransform();
}

void Graphics2D_d2d::rotate(float angle) {
    const float deg = angle * 180.0f / 3.14159265358979323846f;
    local_transform_ = D2D1::Matrix3x2F::Rotation(deg) * local_transform_;
    SyncTransform();
}

void Graphics2D_d2d::rotate(float angle, float px, float py) {
    const float deg = angle * 180.0f / 3.14159265358979323846f;
    local_transform_ = D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(px, py)) * local_transform_;
    SyncTransform();
}

void Graphics2D_d2d::reset() {
    local_transform_ = D2D1::Matrix3x2F::Identity();
    sx_ = 1.0f;
    sy_ = 1.0f;
    dc_->SetTransform(base_transform_);
}

float Graphics2D_d2d::sx() const {
    return sx_;
}

float Graphics2D_d2d::sy() const {
    return sy_;
}

void Graphics2D_d2d::drawChar(wchar_t c, float x, float y) {
    if (!font_ || !brush_) return;
    const auto* f = static_cast<const Font_d2d*>(font_);
    if (!f->FontFace()) {
        wchar_t str[2]{c, L'\0'};
        drawText(str, x, y);
        return;
    }

    UINT32 cp = static_cast<UINT32>(c);
    UINT16 glyph_index = 0;
    f->FontFace()->GetGlyphIndices(&cp, 1, &glyph_index);
    if (glyph_index == 0 && cp < 0x100) {
        UINT32 pua = 0xF000u + cp;
        f->FontFace()->GetGlyphIndices(&pua, 1, &glyph_index);
    }

    const float em = f->getSize();
    ID2D1PathGeometry* geom = GetOrCreateGlyphOutline(f->FontFace(), glyph_index, em);
    if (geom) {
        auto& outline_style = MathOutlineStrokeStyle();
        if (!outline_style) {
            if (auto* ctx = D2DContext::Instance(); ctx && ctx->D2DFactory()) {
                D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_LINE_JOIN_ROUND, 4.0f);
                ctx->D2DFactory()->CreateStrokeStyle(&props, nullptr, 0, outline_style.Put());
            }
        }

        D2D1_MATRIX_3X2_F saved{};
        dc_->GetTransform(&saved);
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * saved);
        dc_->FillGeometry(geom, brush_.Get());
        // Reinforce thin Computer Modern hairlines to match surrounding UI prose weight.
        dc_->DrawGeometry(geom, brush_.Get(), em * 0.025f, outline_style.Get());
        dc_->SetTransform(saved);
        return;
    }

    constexpr float kSubScale = 16.0f;
    float advance = 0.0f;
    DWRITE_GLYPH_OFFSET offset{0.0f, 0.0f};
    DWRITE_GLYPH_RUN run{};
    run.fontFace = f->FontFace();
    run.fontEmSize = em * kSubScale;
    run.glyphCount = 1;
    run.glyphIndices = &glyph_index;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    run.isSideways = FALSE;
    run.bidiLevel = 0;

    D2D1_MATRIX_3X2_F saved{};
    dc_->GetTransform(&saved);
    D2D1::Matrix3x2F glyph_scale = D2D1::Matrix3x2F::Scale(1.0f / kSubScale, 1.0f / kSubScale) *
                                   D2D1::Matrix3x2F::Translation(x, y);
    dc_->SetTransform(glyph_scale * saved);
    dc_->DrawGlyphRun(D2D1::Point2F(0.0f, 0.0f), &run, brush_.Get(), DWRITE_MEASURING_MODE_NATURAL);
    dc_->SetTransform(saved);
}

void Graphics2D_d2d::drawText(const std::wstring& c, float x, float y) {
    if (c.empty() || !brush_) return;
    const auto* f = static_cast<const Font_d2d*>(font_);
    auto sfont = f ? std::make_shared<Font_d2d>(*f) : std::make_shared<Font_d2d>("Serif", tex::PLAIN, 10.0f);
    TextLayout_d2d layout(c, sfont);
    layout.draw(*this, x, y);
}

void Graphics2D_d2d::drawLine(float x1, float y1, float x2, float y2) {
    if (!brush_) return;
    SyncStrokeStyle();
    float lw = (stroke_.lineWidth < 0.1f) ? (stroke_.lineWidth * 1.18f) : stroke_.lineWidth;
    dc_->DrawLine(
        D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
        brush_.Get(), lw, stroke_style_.Get());
}

void Graphics2D_d2d::drawRect(float x, float y, float w, float h) {
    if (!brush_) return;
    SyncStrokeStyle();
    dc_->DrawRectangle(
        D2D1::RectF(x, y, x + w, y + h),
        brush_.Get(), stroke_.lineWidth, stroke_style_.Get());
}

void Graphics2D_d2d::fillRect(float x, float y, float w, float h) {
    if (!brush_) return;
    dc_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush_.Get());
}

void Graphics2D_d2d::drawRoundRect(float x, float y, float w, float h, float rx, float ry) {
    if (!brush_) return;
    SyncStrokeStyle();
    dc_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry),
        brush_.Get(), stroke_.lineWidth, stroke_style_.Get());
}

void Graphics2D_d2d::fillRoundRect(float x, float y, float w, float h, float rx, float ry) {
    if (!brush_) return;
    dc_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry),
        brush_.Get());
}

// ============================================================================
// MathEngine (MicroTeX wrapper & cache)
// ============================================================================

MathEngine& MathEngine::Instance() {
    static MathEngine inst;
    return inst;
}

bool MathEngine::Initialize() {
    if (ready_) return true;
    std::wstring exe_dir = GetExecutableDir();
    std::filesystem::path res_path = std::filesystem::path(exe_dir) / "res";
    if (!std::filesystem::exists(res_path / ".clatexmath-res_root")) {
        // Fallback to third_party path during development runs
        std::filesystem::path dev_path = std::filesystem::path(exe_dir).parent_path() / "third_party" / "MicroTeX" / "res";
        if (std::filesystem::exists(dev_path / ".clatexmath-res_root")) {
            res_path = dev_path;
        }
    }

    try {
        tex::LaTeX::init(res_path.string());
        tex::TextRenderingBox::setFont("Serif");
        ready_ = true;
    } catch (...) {
        ready_ = false;
    }
    return ready_;
}

void MathEngine::Shutdown() {
    cache_.clear();
    GlyphGeomCache().clear();
    MathOutlineStrokeStyle().Reset();
    FontFaceCache().clear();
    if (ready_) {
        tex::LaTeX::release();
        ready_ = false;
    }
}

void MathEngine::ClearCache() {
    cache_.clear();
}

std::shared_ptr<tex::TeXRender> MathEngine::GetOrParse(
    const std::wstring& latex, bool display_style, float wrap_width_px) {
    if (!ready_ && !Initialize()) return nullptr;
    if (latex.empty()) return nullptr;

    // Normalize single-line vs multi-line or wrapped display formulas.
    int bucket = (wrap_width_px > 40.0f) ? static_cast<int>(wrap_width_px / 24.0f) * 24 : 0;
    CacheKey key{latex, display_style, bucket};
    if (auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }

    try {
        tex::Formula f(latex);
        tex::TeXRenderBuilder builder;
        builder.setStyle(display_style ? tex::TexStyle::display : tex::TexStyle::text)
               .setTextSize(16.0f)
               .setTrueValues(true)
               .setForeground(tex::black);
        if (display_style && bucket > 40) {
            builder.setWidth(tex::UnitType::pixel, static_cast<float>(bucket), tex::Alignment::left)
                   .setIsMaxWidth(true)
                   .setLineSpace(tex::UnitType::pixel, 6.0f);
        }
        std::shared_ptr<tex::TeXRender> render(builder.build(f));
        if (cache_.size() > 1024) {
            cache_.clear();
        }
        cache_.emplace(std::move(key), render);
        return render;
    } catch (...) {
        cache_.emplace(std::move(key), nullptr);
        return nullptr;
    }
}

}  // namespace mr
