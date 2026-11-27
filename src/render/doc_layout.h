#pragma once

#include "core/markdown_doc.h"
#include "math/d2d_tex_backend.h"
#include "render/image_loader.h"
#include "render/text_renderer.h"
#include "ui/theme.h"

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mr {

struct TextRangeSpan {
    uint32_t start = 0;
    uint32_t length = 0;
};

struct LinkSpan {
    uint32_t start = 0;
    uint32_t length = 0;
    std::wstring url;
};

struct InlineCopyReplacement {
    uint32_t pos = 0;
    std::wstring text;
};

struct RichTextItem {
    D2D1_RECT_F rect{};
    D2D1_RECT_F cell_clip{};
    std::wstring text;  // UTF-16 string passed to IDWriteTextLayout (with U+FFFC for inline objects)
    ComPtr<IDWriteTextLayout> layout;
    std::vector<TextRangeSpan> code_pills;
    std::vector<TextRangeSpan> mark_highlights;
    std::vector<LinkSpan> links;
    std::vector<InlineCopyReplacement> copy_replacements;
    int selectable_id = -1;
    int table_idx = -1;
};

struct TableRowItem {
    D2D1_RECT_F rect{};
    bool is_header = false;
    std::vector<int> cell_rich_indices;
};

struct TableItem {
    D2D1_RECT_F card_rect{};
    D2D1_RECT_F body_rect{};
    D2D1_RECT_F hscroll_track_rect{};
    D2D1_RECT_F hscroll_thumb_rect{};

    float content_width = 0.0f;
    float scroll_x = 0.0f;
    float max_scroll_x = 0.0f;
    std::vector<float> col_widths;
    std::vector<TableRowItem> rows;
};

struct CodeBlockItem {
    D2D1_RECT_F card_rect{};
    D2D1_RECT_F header_rect{};
    D2D1_RECT_F copy_btn_rect{};
    D2D1_RECT_F body_rect{};
    D2D1_RECT_F hscroll_track_rect{};
    D2D1_RECT_F hscroll_thumb_rect{};

    std::string lang;
    std::wstring code;
    ComPtr<IDWriteTextLayout> layout;
    float content_width = 0.0f;
    float scroll_x = 0.0f;
    float max_scroll_x = 0.0f;
    int selectable_id = -1;
};

struct DisplayMathItem {
    D2D1_RECT_F card_rect{};
    D2D1_RECT_F copy_btn_rect{};
    std::wstring tex;
    std::shared_ptr<tex::TeXRender> render;
    float text_size = 18.0f;
    float draw_scale = 1.0f;
    float formula_w = 0.0f;
    float formula_h = 0.0f;
    float scroll_x = 0.0f;
    float max_scroll_x = 0.0f;
};

struct ImageItem {
    D2D1_RECT_F card_rect{};
    D2D1_RECT_F img_rect{};
    std::wstring resolved_src;
    std::wstring alt;
    std::wstring title;
    std::wstring link_href;
    std::shared_ptr<LoadedImage> image;
    ComPtr<IDWriteTextLayout> caption_layout;
    D2D1_RECT_F caption_rect{};
};

struct DecorBox {
    enum class Kind : uint8_t {
        RectFill = 0,
        RoundedCard,
        LeftBar,
        RuleLine,
        Checkbox
    };
    Kind kind = Kind::RectFill;
    D2D1_RECT_F rect{};
    D2D1_COLOR_F fill{};
    D2D1_COLOR_F stroke{};
    float radius = 0.0f;
    float stroke_width = 1.0f;
    bool checked = false;
};

struct HeadingPosition {
    int heading_id = 0;
    std::string slug;
    float y = 0.0f;
};

struct TextSelection {
    int start_sel = -1;
    uint32_t start_pos = 0;
    int end_sel = -1;
    uint32_t end_pos = 0;

    bool HasSelection() const {
        return start_sel >= 0 && end_sel >= 0 && !(start_sel == end_sel && start_pos == end_pos);
    }

    void Clear() {
        start_sel = end_sel = -1;
        start_pos = end_pos = 0;
    }
};

struct SearchMatch {
    int selectable_id = -1;
    uint32_t start = 0;
    uint32_t length = 0;
    float y = 0.0f;
};

struct HitTestResult {
    enum class Target : uint8_t {
        None = 0,
        Text,
        Link,
        CopyCodeBtn,
        CopyMathBtn,
        Image,
        CodeHScroll,
        TableHScroll
    };

    Target target = Target::None;
    int item_index = -1;
    int selectable_id = -1;
    uint32_t text_pos = 0;
    std::wstring url;
    std::wstring tooltip;
};

struct ScrollAnchor {
    enum class Kind : uint8_t { None = 0, RichText, Code, Math, Image };
    Kind kind = Kind::None;
    int index = -1;
    uint32_t text_pos = 0;
    float offset_y = 0.0f;
};

// Lays out an entire MarkdownDocument into GPU-ready DirectWrite / Direct2D primitives.
class DocumentLayout {
public:
    void Build(
        const MarkdownDocument& doc,
        float viewport_width,
        int zoom_percent,
        bool sidebar_visible,
        const ThemePalette& theme,
        ImageCache& images);

    void Render(
        ID2D1DeviceContext5* dc,
        const ThemePalette& theme,
        float scroll_y,
        float viewport_height,
        const TextSelection& selection,
        const std::vector<SearchMatch>& search_matches,
        int active_search_idx,
        int hovered_copy_code_idx,
        int copied_code_idx,
        int hovered_copy_math_idx,
        int copied_math_idx,
        int active_hscroll_code_idx = -1,
        int active_hscroll_table_idx = -1);

    HitTestResult HitTest(float doc_x, float doc_y) const;
    bool HitTestSelectableNearest(float doc_x, float doc_y, int& out_sel_id, uint32_t& out_pos) const;
    std::wstring ExtractSelectedText(const TextSelection& sel) const;
    std::wstring ExtractAllText() const;
    void SelectWordAt(int sel_id, uint32_t pos, uint32_t& out_start, uint32_t& out_end) const;

    std::vector<SearchMatch> FindAllMatches(const std::wstring& query) const;
    bool GetHeadingY(int heading_id, float& out_y) const;
    bool GetAnchorY(const std::string& slug, float& out_y) const;
    int FindActiveHeadingId(float scroll_y) const;
    ScrollAnchor CaptureScrollAnchor(float scroll_y) const;
    float ResolveScrollAnchor(const ScrollAnchor& anchor) const;

    float TotalHeight() const { return total_height_; }
    float ContentLeft() const { return content_left_; }
    float ContentWidth() const { return content_width_; }

    bool ScrollCodeBlockBy(int code_idx, float dx);
    bool ScrollMathBlockBy(int math_idx, float dx);
    bool ScrollTableBy(int table_idx, float dx);
    const std::vector<ImageItem>& Images() const { return image_items_; }
    const std::vector<CodeBlockItem>& CodeBlocks() const { return code_blocks_; }
    const std::vector<DisplayMathItem>& MathBlocks() const { return math_blocks_; }
    const std::vector<TableItem>& Tables() const { return table_items_; }

private:
    struct SelectableSlot {
        bool is_code = false;
        size_t index = 0;  // Index into rich_items_ or code_blocks_
    };

    struct LayoutContext {
        const MarkdownDocument* doc = nullptr;
        const ThemePalette* theme = nullptr;
        ImageCache* images = nullptr;
        IDWriteFactory5* dw = nullptr;
        float scale = 1.0f;
        float body_size = 16.0f;
    };

    float LayoutBlockList(const std::vector<Block>& blocks, float x, float y, float width, const LayoutContext& ctx, int list_depth, bool compact);
    float LayoutBlock(const Block& b, float x, float y, float width, const LayoutContext& ctx, int list_depth, bool in_quote);
    float LayoutParagraphItem(const ParagraphData& p, float x, float y, float width, float font_size, bool is_heading, int heading_level, D2D1_COLOR_F base_color, const LayoutContext& ctx, TextAlign align = TextAlign::Default);
    bool BuildParagraphItem(const ParagraphData& p, float width, float font_size, bool is_heading, int heading_level, D2D1_COLOR_F base_color, const LayoutContext& ctx, TextAlign align, RichTextItem& out_item, float& out_height);
    float LayoutCodeBlock(const Block& b, float x, float y, float width, const LayoutContext& ctx);
    float LayoutDisplayMath(const Block& b, float x, float y, float width, const LayoutContext& ctx);
    float LayoutImageBlock(const Block& b, float x, float y, float width, const LayoutContext& ctx);
    float LayoutTable(const Block& b, float x, float y, float width, const LayoutContext& ctx);

    std::vector<DecorBox> decor_behind_;
    std::vector<DecorBox> decor_front_;
    std::vector<RichTextItem> rich_items_;
    std::vector<CodeBlockItem> code_blocks_;
    std::vector<DisplayMathItem> math_blocks_;
    std::vector<ImageItem> image_items_;
    std::vector<TableItem> table_items_;
    std::vector<SelectableSlot> selectables_;
    std::vector<HeadingPosition> headings_;
    std::unordered_map<std::string, float> anchor_map_;

    float total_height_ = 0.0f;
    float content_left_ = 0.0f;
    float content_width_ = 760.0f;
    float scale_ = 1.0f;
};

}  // namespace mr
