#pragma once

#include "core/types.h"
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif
#include "graphic/graphic.h"
#include "render.h"

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace mr {

class Font_d2d : public tex::Font {
public:
    Font_d2d(const std::string& file, float size);
    Font_d2d(const std::string& name, int style, float size);

    float getSize() const override;
    tex::sptr<tex::Font> deriveFont(int style) const override;
    bool operator==(const tex::Font& f) const override;
    bool operator!=(const tex::Font& f) const override;

    IDWriteFontFace* FontFace() const { return font_face_.Get(); }
    const std::wstring& Family() const { return family_; }
    int Style() const { return style_; }
    bool IsFileFont() const { return is_file_font_; }

private:
    Font_d2d() = default;

    ComPtr<IDWriteFontFace> font_face_;
    std::wstring family_;
    int style_ = tex::PLAIN;
    float size_ = 10.0f;
    bool is_file_font_ = false;
};

class TextLayout_d2d : public tex::TextLayout {
public:
    TextLayout_d2d(const std::wstring& src, const tex::sptr<Font_d2d>& font);

    void getBounds(tex::Rect& bounds) override;
    void draw(tex::Graphics2D& g2, float x, float y) override;

private:
    ComPtr<IDWriteTextLayout> layout_;
    float baseline_ = 0.0f;
    tex::Rect bounds_{};
};

class Graphics2D_d2d : public tex::Graphics2D {
public:
    explicit Graphics2D_d2d(ID2D1DeviceContext5* dc);

    void setColor(tex::color c) override;
    tex::color getColor() const override;
    void setStroke(const tex::Stroke& s) override;
    const tex::Stroke& getStroke() const override;
    void setStrokeWidth(float w) override;
    const tex::Font* getFont() const override;
    void setFont(const tex::Font* font) override;

    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float angle) override;
    void rotate(float angle, float px, float py) override;
    void reset() override;
    float sx() const override;
    float sy() const override;

    void drawChar(wchar_t c, float x, float y) override;
    void drawText(const std::wstring& c, float x, float y) override;
    void drawLine(float x1, float y1, float x2, float y2) override;
    void drawRect(float x, float y, float w, float h) override;
    void fillRect(float x, float y, float w, float h) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry) override;

    ID2D1DeviceContext5* D2D() const { return dc_; }
    ID2D1SolidColorBrush* Brush() const { return brush_.Get(); }

private:
    void SyncTransform();
    void SyncStrokeStyle();

    ID2D1DeviceContext5* dc_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> stroke_style_;
    D2D1_MATRIX_3X2_F base_transform_{};
    D2D1_MATRIX_3X2_F local_transform_{};

    tex::color color_ = tex::black;
    tex::Stroke stroke_;
    bool stroke_dirty_ = true;
    const tex::Font* font_ = nullptr;
    float sx_ = 1.0f;
    float sy_ = 1.0f;
};

// Initializes MicroTeX once using <exe_dir>/res and caches compiled formulas.
class MathEngine {
public:
    static MathEngine& Instance();

    bool Initialize();
    void Shutdown();
    bool IsReady() const { return ready_; }

    // Compiles or retrieves a cached TeXRender (stored with trueValues=true for scale invariance).
    std::shared_ptr<tex::TeXRender> GetOrParse(
        const std::wstring& latex,
        bool display_style,
        float wrap_width_px = 0.0f);

    void ClearCache();

private:
    MathEngine() = default;

    struct CacheKey {
        std::wstring latex;
        bool display_style = false;
        int wrap_bucket = 0;

        bool operator==(const CacheKey& o) const {
            return display_style == o.display_style && wrap_bucket == o.wrap_bucket && latex == o.latex;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t h = std::hash<std::wstring>{}(k.latex);
            h ^= (static_cast<size_t>(k.display_style) + 0x9e3779b9 + (h << 6) + (h >> 2));
            h ^= (static_cast<size_t>(k.wrap_bucket) + 0x9e3779b9 + (h << 6) + (h >> 2));
            return h;
        }
    };

    bool ready_ = false;
    std::unordered_map<CacheKey, std::shared_ptr<tex::TeXRender>, CacheKeyHash> cache_;
};

}  // namespace mr
