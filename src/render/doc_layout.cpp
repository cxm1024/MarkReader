#include "render/doc_layout.h"
#include "core/types.h"
#include "render/d2d_context.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace mr {

namespace {

ComPtr<IDWriteTextLayout> MakeSimpleLabel(
    IDWriteFactory5* dw,
    const std::wstring& text,
    float size,
    bool bold,
    bool mono,
    float max_width,
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    ComPtr<IDWriteTextLayout> layout;
    if (!dw) return layout;
    ComPtr<IDWriteTextFormat> fmt;
    const wchar_t* fam = mono ? PickMonoFontFamily(dw) : PickUiFontFamily(dw);
    if (FAILED(dw->CreateTextFormat(
            fam, nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            std::max(8.0f, size), L"", fmt.Put()))) {
        return layout;
    }
    fmt->SetTextAlignment(align);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(),
                         std::max(20.0f, max_width), 10000.0f, layout.Put());
    return layout;
}

std::vector<DWRITE_HIT_TEST_METRICS> HitTestRangeBoxes(
    IDWriteTextLayout* layout, uint32_t start, uint32_t length, float ox, float oy) {
    std::vector<DWRITE_HIT_TEST_METRICS> boxes;
    if (!layout || length == 0) return boxes;
    UINT32 count = 0;
    layout->HitTestTextRange(start, length, ox, oy, nullptr, 0, &count);
    if (count == 0) return boxes;
    boxes.resize(count);
    if (FAILED(layout->HitTestTextRange(start, length, ox, oy, boxes.data(), count, &count))) {
        boxes.clear();
    } else {
        boxes.resize(count);
    }
    return boxes;
}

bool PointInRect(float x, float y, const D2D1_RECT_F& r) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void DrawSmallPillButton(
    ID2D1DeviceContext5* dc,
    ID2D1SolidColorBrush* brush,
    IDWriteFactory5* dw,
    const D2D1_RECT_F& r,
    const std::wstring& label,
    D2D1_COLOR_F bg,
    D2D1_COLOR_F border,
    D2D1_COLOR_F fg,
    float font_size) {
    brush->SetColor(bg);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5.0f, 5.0f), brush);
    brush->SetColor(border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 5.0f, 5.0f), brush, 1.0f);
    if (auto tl = MakeSimpleLabel(dw, label, font_size, false, false, r.right - r.left, DWRITE_TEXT_ALIGNMENT_CENTER)) {
        DWRITE_TEXT_METRICS tm{};
        tl->GetMetrics(&tm);
        float ty = r.top + ((r.bottom - r.top) - tm.height) * 0.5f;
        brush->SetColor(fg);
        dc->DrawTextLayout(D2D1::Point2F(r.left, ty), tl.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
}

void AdmonitionStyle(AdmonitionKind kind, const ThemePalette& t, D2D1_COLOR_F& accent, D2D1_COLOR_F& bg, std::wstring& title) {
    switch (kind) {
        case AdmonitionKind::Tip:
            accent = t.adm_tip_accent; bg = t.adm_tip_bg; title = L"TIP"; break;
        case AdmonitionKind::Important:
            accent = t.adm_important_accent; bg = t.adm_important_bg; title = L"IMPORTANT"; break;
        case AdmonitionKind::Warning:
            accent = t.adm_warning_accent; bg = t.adm_warning_bg; title = L"WARNING"; break;
        case AdmonitionKind::Caution:
            accent = t.adm_caution_accent; bg = t.adm_caution_bg; title = L"CAUTION"; break;
        default:
            accent = t.adm_note_accent; bg = t.adm_note_bg; title = L"NOTE"; break;
    }
}

struct InlineMathChunk {
    std::wstring raw;
    std::wstring compiled;
};

bool IsBreakableTeXRelCmd(const std::wstring& cmd) {
    static const wchar_t* const kCmds[] = {
        L"le", L"leq", L"ge", L"geq", L"neq", L"ne", L"approx", L"equiv", L"propto",
        L"sim", L"simeq", L"cong", L"implies", L"iff", L"to", L"rightarrow", L"leftarrow",
        L"gets", L"Rightarrow", L"Leftarrow", L"Leftrightarrow", L"triangleq",
        L"in", L"notin", L"subset", L"subseteq", L"supset", L"supseteq"
    };
    for (const wchar_t* k : kCmds) {
        if (cmd == k) return true;
    }
    return false;
}

bool IsOperandTailChar(wchar_t c) {
    return std::iswalnum(c) || c == L'}' || c == L')' || c == L']' ||
           c == L'|' || c == L'.' || c == L'!' || c == L'\'';
}

// Splits long inline LaTeX only at depth-0 relations/binary operators/commas so
// long equations wrap naturally across text lines without breaking brackets or fractions.
std::vector<InlineMathChunk> SplitInlineMathTopLevel(const std::wstring& tex) {
    std::vector<InlineMathChunk> out;
    const size_t n = tex.size();
    int brace_depth = 0;
    int delim_depth = 0;
    int round_depth = 0;
    int bracket_depth = 0;

    size_t seg_start = 0;
    bool has_operand_before_op = false;
    wchar_t last_non_ws = 0;

    auto push_slice = [&](size_t cut, bool append_empty_rhs) {
        size_t s = seg_start;
        while (s < cut && std::iswspace(tex[s])) ++s;
        size_t e = cut;
        while (e > s && std::iswspace(tex[e - 1])) --e;
        if (e <= s) return;
        std::wstring raw = tex.substr(s, e - s);
        std::wstring comp = append_empty_rhs ? (raw + L" {}") : raw;
        out.push_back({std::move(raw), std::move(comp)});
        seg_start = cut;
        has_operand_before_op = false;
    };

    for (size_t i = 0; i < n;) {
        wchar_t c = tex[i];
        if (std::iswspace(c)) {
            ++i;
            continue;
        }

        if (c == L'\\') {
            if (i + 1 < n && !std::iswalpha(tex[i + 1])) {
                // Escaped single symbol such as \{ \} \( \) \|
                i += 2;
                last_non_ws = L'x';
                has_operand_before_op = true;
                continue;
            }
            size_t j = i + 1;
            while (j < n && std::iswalpha(tex[j])) ++j;
            std::wstring cmd = tex.substr(i + 1, j - (i + 1));

            if (cmd == L"left") {
                ++delim_depth;
                i = j;
                last_non_ws = L'(';
                continue;
            }
            if (cmd == L"right") {
                if (delim_depth > 0) --delim_depth;
                i = j;
                last_non_ws = L')';
                has_operand_before_op = true;
                continue;
            }

            if (brace_depth == 0 && delim_depth == 0 && round_depth == 0 && bracket_depth == 0 &&
                has_operand_before_op && IsBreakableTeXRelCmd(cmd)) {
                size_t next = j;
                while (next < n && std::iswspace(tex[next])) ++next;
                if (next < n && (j - seg_start) >= 4) {
                    push_slice(j, true);
                    i = next;
                    last_non_ws = 0;
                    continue;
                }
            }

            i = j;
            last_non_ws = L'a';
            has_operand_before_op = true;
            continue;
        }

        if (c == L'{') {
            ++brace_depth;
        } else if (c == L'}') {
            if (brace_depth > 0) --brace_depth;
            has_operand_before_op = true;
        } else if (c == L'(' && brace_depth == 0 && delim_depth == 0) {
            ++round_depth;
        } else if (c == L')' && brace_depth == 0 && delim_depth == 0) {
            if (round_depth > 0) --round_depth;
            else if (bracket_depth > 0) --bracket_depth;
            has_operand_before_op = true;
        } else if (c == L'[' && brace_depth == 0 && delim_depth == 0) {
            ++bracket_depth;
        } else if (c == L']' && brace_depth == 0 && delim_depth == 0) {
            if (bracket_depth > 0) --bracket_depth;
            else if (round_depth > 0) --round_depth;
            has_operand_before_op = true;
        } else if (brace_depth == 0 && delim_depth == 0 && round_depth == 0 && bracket_depth == 0) {
            bool is_rel_op = (c == L'=' || c == L'<' || c == L'>' || c == L',' || c == L';' || c == L'+');
            bool is_bin_minus = (c == L'-' && last_non_ws != 0 && IsOperandTailChar(last_non_ws));
            if (has_operand_before_op && (is_rel_op || is_bin_minus)) {
                size_t next = i + 1;
                while (next < n && std::iswspace(tex[next])) ++next;
                // Avoid splitting inside double char ops like <=, >=, !=, :=
                bool multi_char = (next < n && (tex[next] == L'=' || tex[next] == L',' || tex[next] == L';'));
                if (!multi_char && next < n && (i + 1 - seg_start) >= 3) {
                    push_slice(i + 1, true);
                    i = next;
                    last_non_ws = 0;
                    continue;
                }
            }
            has_operand_before_op = IsOperandTailChar(c);
        }

        last_non_ws = c;
        ++i;
    }

    push_slice(n, false);
    if (out.size() <= 1) {
        out.clear();
        out.push_back({tex, tex});
    }
    return out;
}

}  // namespace

void DocumentLayout::Build(
    const MarkdownDocument& doc,
    float viewport_width,
    int zoom_percent,
    bool sidebar_visible,
    const ThemePalette& theme,
    ImageCache& images) {
    decor_behind_.clear();
    decor_front_.clear();
    rich_items_.clear();
    code_blocks_.clear();
    math_blocks_.clear();
    image_items_.clear();
    table_items_.clear();
    selectables_.clear();
    headings_.clear();
    anchor_map_.clear();
    line_anchors_.clear();

    auto* d2d = D2DContext::Instance();
    if (!d2d || !d2d->DWrite()) {
        total_height_ = 0.0f;
        return;
    }

    scale_ = static_cast<float>(std::clamp(zoom_percent, 50, 250)) / 100.0f;
    const float body_size = 16.0f * scale_;
    const float h_margin = std::clamp(viewport_width * 0.055f, 24.0f, 68.0f);
    const float base_measure = sidebar_visible ? 780.0f : 1040.0f;
    const float max_measure = std::clamp(base_measure + (scale_ - 1.0f) * 260.0f, 640.0f, 1340.0f);
    content_width_ = std::max(240.0f, std::min(viewport_width - 2.0f * h_margin, max_measure));
    content_left_ = std::max(h_margin, (viewport_width - content_width_) * 0.5f);

    LayoutContext ctx;
    ctx.doc = &doc;
    ctx.theme = &theme;
    ctx.images = &images;
    ctx.dw = d2d->DWrite();
    ctx.scale = scale_;
    ctx.body_size = body_size;

    float y = 28.0f * scale_;
    y = LayoutBlockList(doc.blocks, content_left_, y, content_width_, ctx, 0, false);
    total_height_ = y + 64.0f * scale_;
}

float DocumentLayout::LayoutBlockList(
    const std::vector<Block>& blocks,
    float x,
    float y,
    float width,
    const LayoutContext& ctx,
    int list_depth,
    bool compact) {
    bool prev_footnote = false;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        if (b.kind == Block::Kind::FootnoteDef && !prev_footnote) {
            DecorBox sep;
            sep.kind = DecorBox::Kind::RuleLine;
            sep.rect = D2D1::RectF(x, y + 8.0f * ctx.scale, x + width * 0.35f, y + 9.0f * ctx.scale);
            sep.fill = ctx.theme->hr_color;
            decor_behind_.push_back(sep);
            y += 22.0f * ctx.scale;
        }
        prev_footnote = (b.kind == Block::Kind::FootnoteDef);

        float next_y = LayoutBlock(b, x, y, width, ctx, list_depth, false);
        float gap = compact ? (6.0f * ctx.scale) : (14.0f * ctx.scale);
        if (b.kind == Block::Kind::Heading) gap = 10.0f * ctx.scale;
        if (i + 1 < blocks.size()) {
            next_y += gap;
        }
        y = next_y;
    }
    return y;
}

float DocumentLayout::LayoutBlock(
    const Block& b,
    float x,
    float y,
    float width,
    const LayoutContext& ctx,
    int list_depth,
    bool in_quote) {
    switch (b.kind) {
        case Block::Kind::Heading: {
            static constexpr float kMults[] = {2.0f, 1.55f, 1.28f, 1.12f, 1.0f, 0.92f};
            int lvl = std::clamp(b.heading_level, 1, 6);
            float top_pad = (lvl <= 2 ? 18.0f : 10.0f) * ctx.scale;
            if (y < 36.0f * ctx.scale) top_pad = 0.0f;
            float hy = y + top_pad;

            headings_.push_back({b.heading_id, b.slug, hy});
            anchor_map_[b.slug] = hy;

            float fsize = ctx.body_size * kMults[lvl - 1];
            D2D1_COLOR_F hcol = (lvl == 6) ? ctx.theme->text_secondary : ctx.theme->text_primary;
            float end_y = LayoutParagraphItem(b.inline_content, x, hy, width, fsize, true, lvl, hcol, ctx);
            if (lvl <= 2) {
                end_y += 7.0f * ctx.scale;
                DecorBox line;
                line.kind = DecorBox::Kind::RuleLine;
                line.rect = D2D1::RectF(x, end_y, x + width, end_y + 1.0f);
                line.fill = ctx.theme->hr_color;
                decor_behind_.push_back(line);
                end_y += 3.0f * ctx.scale;
            }
            RecordSourceAnchor(b.source_line, b.source_line, hy, end_y);
            return end_y;
        }

        case Block::Kind::Paragraph: {
            D2D1_COLOR_F col = in_quote ? ctx.theme->blockquote_text : ctx.theme->text_primary;
            float end_y = LayoutParagraphItem(b.inline_content, x, y, width, ctx.body_size, false, 0, col, ctx);
            int extra_lines = static_cast<int>(std::count(b.inline_content.plain_text.begin(), b.inline_content.plain_text.end(), L'\n'));
            RecordSourceAnchor(b.source_line, b.source_line + extra_lines, y, end_y);
            return end_y;
        }

        case Block::Kind::CodeBlock: {
            float end_y = LayoutCodeBlock(b, x, y, width, ctx);
            int extra_lines = static_cast<int>(std::count(b.code_text.begin(), b.code_text.end(), L'\n'));
            RecordSourceAnchor(b.source_line, b.source_line + extra_lines + 1, y, end_y);
            return end_y;
        }

        case Block::Kind::DisplayMath: {
            float end_y = LayoutDisplayMath(b, x, y, width, ctx);
            int extra_lines = static_cast<int>(std::count(b.math_tex.begin(), b.math_tex.end(), L'\n'));
            RecordSourceAnchor(b.source_line, b.source_line + extra_lines, y, end_y);
            return end_y;
        }

        case Block::Kind::Image: {
            float end_y = LayoutImageBlock(b, x, y, width, ctx);
            RecordSourceAnchor(b.source_line, b.source_line, y, end_y);
            return end_y;
        }

        case Block::Kind::ThematicBreak: {
            float mid_y = y + 8.0f * ctx.scale;
            DecorBox line;
            line.kind = DecorBox::Kind::RuleLine;
            line.rect = D2D1::RectF(x, mid_y, x + width, mid_y + 1.0f);
            line.fill = ctx.theme->hr_color;
            decor_behind_.push_back(line);
            float end_y = mid_y + 9.0f * ctx.scale;
            RecordSourceAnchor(b.source_line, b.source_line, y, end_y);
            return end_y;
        }

        case Block::Kind::BlockQuote: {
            float pad_x = 16.0f * ctx.scale;
            float pad_y = 10.0f * ctx.scale;
            size_t bg_idx = decor_behind_.size();
            decor_behind_.push_back({});  // background placeholder
            decor_behind_.push_back({});  // left bar placeholder

            float inner_y = y + pad_y;
            for (size_t i = 0; i < b.children.size(); ++i) {
                inner_y = LayoutBlock(b.children[i], x + pad_x, inner_y, std::max(80.0f, width - pad_x - 10.0f * ctx.scale), ctx, list_depth, true);
                if (i + 1 < b.children.size()) inner_y += 10.0f * ctx.scale;
            }
            float end_y = inner_y + pad_y;

            DecorBox bg;
            bg.kind = DecorBox::Kind::RoundedCard;
            bg.rect = D2D1::RectF(x, y, x + width, end_y);
            bg.fill = ctx.theme->blockquote_bg;
            bg.stroke = ctx.theme->blockquote_bg;
            bg.radius = 6.0f;
            decor_behind_[bg_idx] = bg;

            DecorBox bar;
            bar.kind = DecorBox::Kind::LeftBar;
            bar.rect = D2D1::RectF(x, y, x + 4.0f * ctx.scale, end_y);
            bar.fill = ctx.theme->blockquote_bar;
            bar.radius = 2.0f;
            decor_behind_[bg_idx + 1] = bar;
            return end_y;
        }

        case Block::Kind::Admonition: {
            D2D1_COLOR_F accent{}, bg_col{};
            std::wstring title;
            AdmonitionStyle(b.admonition_kind, *ctx.theme, accent, bg_col, title);

            float pad_x = 16.0f * ctx.scale;
            float pad_y = 12.0f * ctx.scale;
            size_t bg_idx = decor_behind_.size();
            decor_behind_.push_back({});
            decor_behind_.push_back({});

            float cur_y = y + pad_y;
            // Admonition header label
            ParagraphData hdr;
            InlineRun hr;
            hr.text = title;
            hr.bold = true;
            hdr.runs.push_back(hr);
            cur_y = LayoutParagraphItem(hdr, x + pad_x, cur_y, std::max(80.0f, width - pad_x - 12.0f * ctx.scale),
                                        ctx.body_size * 0.90f, false, 0, accent, ctx);
            cur_y += 6.0f * ctx.scale;

            for (size_t i = 0; i < b.children.size(); ++i) {
                cur_y = LayoutBlock(b.children[i], x + pad_x, cur_y, std::max(80.0f, width - pad_x - 12.0f * ctx.scale), ctx, list_depth, false);
                if (i + 1 < b.children.size()) cur_y += 8.0f * ctx.scale;
            }
            float end_y = cur_y + pad_y;

            DecorBox card;
            card.kind = DecorBox::Kind::RoundedCard;
            card.rect = D2D1::RectF(x, y, x + width, end_y);
            card.fill = bg_col;
            card.stroke = accent;
            card.stroke.a = 0.22f;
            card.radius = 8.0f;
            decor_behind_[bg_idx] = card;

            DecorBox bar;
            bar.kind = DecorBox::Kind::LeftBar;
            bar.rect = D2D1::RectF(x, y, x + 4.0f * ctx.scale, end_y);
            bar.fill = accent;
            bar.radius = 2.0f;
            decor_behind_[bg_idx + 1] = bar;
            return end_y;
        }

        case Block::Kind::List: {
            float indent = 26.0f * ctx.scale;
            float cur_y = y;
            uint32_t num = b.list_start;
            for (size_t i = 0; i < b.list_items.size(); ++i) {
                const auto& item = b.list_items[i];
                if (item.is_task) {
                    float box_sz = 14.0f * ctx.scale;
                    float bx = x + 4.0f * ctx.scale;
                    float by = cur_y + (ctx.body_size * 1.45f - box_sz) * 0.5f;
                    DecorBox cb;
                    cb.kind = DecorBox::Kind::Checkbox;
                    cb.rect = D2D1::RectF(bx, by, bx + box_sz, by + box_sz);
                    cb.fill = item.task_checked ? ctx.theme->accent : ctx.theme->bg_canvas;
                    cb.stroke = item.task_checked ? ctx.theme->accent : ctx.theme->border_strong;
                    cb.checked = item.task_checked;
                    cb.radius = 3.5f;
                    decor_behind_.push_back(cb);
                } else {
                    std::wstring marker;
                    if (b.list_ordered) {
                        marker = std::to_wstring(num++) + L".";
                    } else {
                        static const wchar_t* kBullets[] = {L"\u2022", L"\u25E6", L"\u25AA"};
                        marker = kBullets[list_depth % 3];
                    }
                    ParagraphData mp;
                    InlineRun mr;
                    mr.text = marker;
                    mp.runs.push_back(mr);
                    LayoutParagraphItem(mp, x, cur_y, indent - 6.0f * ctx.scale, ctx.body_size, false, 0,
                                        ctx.theme->text_secondary, ctx, TextAlign::Right);
                }

                float item_y = LayoutBlockList(item.children, x + indent, cur_y, std::max(80.0f, width - indent), ctx, list_depth + 1, true);
                if (item_y <= cur_y) {
                    item_y = cur_y + ctx.body_size * 1.4f;
                    RecordSourceAnchor(item.source_line, item.source_line, cur_y, item_y);
                }
                cur_y = item_y;
                if (i + 1 < b.list_items.size()) {
                    cur_y += (b.list_tight ? 4.0f : 8.0f) * ctx.scale;
                }
            }
            return cur_y;
        }

        case Block::Kind::Table:
            return LayoutTable(b, x, y, width, ctx);

        case Block::Kind::FootnoteDef: {
            std::string fn_slug = "fn-" + std::to_string(b.footnote_id);
            anchor_map_[fn_slug] = y;
            float badge_w = 34.0f * ctx.scale;

            ParagraphData mp;
            InlineRun mr;
            mr.text = L"[" + std::to_wstring(b.footnote_id) + L"]";
            mr.bold = true;
            mp.runs.push_back(mr);
            LayoutParagraphItem(mp, x, y, badge_w, ctx.body_size * 0.88f, false, 0, ctx.theme->accent, ctx);

            float end_y = LayoutBlockList(b.children, x + badge_w, y, std::max(80.0f, width - badge_w), ctx, list_depth, true);
            return std::max(end_y, y + ctx.body_size * 1.4f);
        }
    }
    return y;
}

float DocumentLayout::LayoutParagraphItem(
    const ParagraphData& p,
    float x,
    float y,
    float width,
    float font_size,
    bool is_heading,
    int heading_level,
    D2D1_COLOR_F base_color,
    const LayoutContext& ctx,
    TextAlign align) {
    RichTextItem item;
    float h = 0.0f;
    if (!BuildParagraphItem(p, width, font_size, is_heading, heading_level, base_color, ctx, align, item, h)) {
        return y;
    }
    item.rect = D2D1::RectF(x, y, x + width, y + h);
    int sel_id = static_cast<int>(selectables_.size());
    item.selectable_id = sel_id;
    selectables_.push_back({false, rich_items_.size()});
    rich_items_.push_back(std::move(item));
    return y + h;
}

bool DocumentLayout::BuildParagraphItem(
    const ParagraphData& p,
    float width,
    float font_size,
    bool is_heading,
    int heading_level,
    D2D1_COLOR_F base_color,
    const LayoutContext& ctx,
    TextAlign align,
    RichTextItem& item,
    float& out_height) {
    item = {};
    out_height = 0.0f;
    std::wstring& disp = item.text;

    struct SpanStyleApply {
        uint32_t start = 0;
        uint32_t len = 0;
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool underline = false;
        bool mono = false;
        bool has_custom_size = false;
        float custom_size = 0.0f;
        bool has_color = false;
        D2D1_COLOR_F color{};
        std::shared_ptr<tex::TeXRender> math_render;
        std::shared_ptr<LoadedImage> img_obj;
    };
    std::vector<SpanStyleApply> spans;

    const tex::color math_fg = ColorToTexArgb(base_color);
    const float math_size = font_size * 1.10f;
    const float split_thresh = std::max(115.0f * ctx.scale, width * 0.28f);

    for (const auto& run : p.runs) {
        uint32_t start = static_cast<uint32_t>(disp.size());
        if (run.kind == InlineRun::Kind::LineBreak) {
            disp.push_back(L'\n');
            continue;
        }

        if (run.kind == InlineRun::Kind::InlineMath) {
            auto tr = MathEngine::Instance().GetOrParse(run.text, false, 0.0f);
            if (tr) {
                tr->setTextSize(math_size);
                float nat_w = static_cast<float>(tr->getWidth());
                std::vector<std::shared_ptr<tex::TeXRender>> chunk_renders;
                std::vector<InlineMathChunk> chunks;

                if (!is_heading && width < 4000.0f && run.text.size() >= 18 && nat_w > split_thresh) {
                    chunks = SplitInlineMathTopLevel(run.text);
                    if (chunks.size() > 1) {
                        bool ok = true;
                        chunk_renders.reserve(chunks.size());
                        for (const auto& ch : chunks) {
                            auto ctr = MathEngine::Instance().GetOrParse(ch.compiled, false, 0.0f);
                            if (!ctr) { ok = false; break; }
                            chunk_renders.push_back(std::move(ctr));
                        }
                        if (!ok) chunk_renders.clear();
                    }
                }

                if (chunk_renders.size() > 1) {
                    for (size_t ci = 0; ci < chunk_renders.size(); ++ci) {
                        uint32_t pos = static_cast<uint32_t>(disp.size());
                        disp.push_back(L'\uFFFC');
                        std::wstring rep;
                        if (ci == 0) rep.push_back(L'$');
                        rep.append(chunks[ci].raw);
                        if (ci + 1 < chunk_renders.size()) rep.push_back(L' ');
                        else rep.push_back(L'$');
                        item.copy_replacements.push_back({pos, std::move(rep)});

                        SpanStyleApply s;
                        s.start = pos;
                        s.len = 1;
                        s.math_render = chunk_renders[ci];
                        if (run.is_link) {
                            s.has_color = true;
                            s.color = ctx.theme->link;
                            item.links.push_back({pos, 1, run.url});
                        }
                        spans.push_back(std::move(s));
                    }
                } else {
                    disp.push_back(L'\uFFFC');
                    item.copy_replacements.push_back({start, L"$" + run.text + L"$"});
                    SpanStyleApply s;
                    s.start = start;
                    s.len = 1;
                    s.math_render = tr;
                    if (run.is_link) {
                        s.has_color = true;
                        s.color = ctx.theme->link;
                        item.links.push_back({start, 1, run.url});
                    }
                    spans.push_back(std::move(s));
                }
            } else {
                std::wstring fallback = L"$" + run.text + L"$";
                disp.append(fallback);
                SpanStyleApply s;
                s.start = start;
                s.len = static_cast<uint32_t>(fallback.size());
                s.mono = true;
                s.has_custom_size = true;
                s.custom_size = font_size * 0.88f;
                s.has_color = true;
                s.color = ctx.theme->code_inline_text;
                item.code_pills.push_back({start, s.len});
                spans.push_back(std::move(s));
            }
            continue;
        }

        if (run.kind == InlineRun::Kind::InlineImage) {
            std::wstring resolved = ResolveDocumentResourcePath(ctx.doc->base_dir, run.url);
            auto loaded = ctx.images->GetOrLoad(resolved);
            disp.push_back(L'\uFFFC');
            item.copy_replacements.push_back({start, run.text});
            SpanStyleApply s;
            s.start = start;
            s.len = 1;
            s.img_obj = loaded;
            spans.push_back(std::move(s));
            if (run.is_link && !run.link_href.empty()) {
                item.links.push_back({start, 1, run.link_href});
            }
            continue;
        }

        if (run.text.empty()) continue;
        disp.append(run.text);
        uint32_t len = static_cast<uint32_t>(run.text.size());

        SpanStyleApply s;
        s.start = start;
        s.len = len;
        s.bold = run.bold || is_heading;
        s.italic = run.italic;
        s.strike = run.strike;
        s.underline = run.underline || run.is_link;
        if (run.kind == InlineRun::Kind::InlineCode) {
            s.mono = true;
            s.has_custom_size = true;
            s.custom_size = font_size * 0.87f;
            s.has_color = true;
            s.color = run.is_link ? ctx.theme->link : ctx.theme->code_inline_text;
            item.code_pills.push_back({start, len});
        } else if (run.superscript || run.subscript || run.kind == InlineRun::Kind::FootnoteRef) {
            s.has_custom_size = true;
            s.custom_size = font_size * 0.74f;
        }
        if (run.highlight) {
            item.mark_highlights.push_back({start, len});
        }
        if (run.is_link) {
            s.has_color = true;
            s.color = ctx.theme->link;
            item.links.push_back({start, len, run.url});
        } else if (!s.has_color) {
            s.has_color = true;
            s.color = base_color;
        }
        spans.push_back(std::move(s));
    }

    if (disp.empty()) return false;

    ComPtr<IDWriteTextFormat> fmt;
    DWRITE_FONT_WEIGHT base_weight = is_heading
        ? (heading_level == 1 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD)
        : DWRITE_FONT_WEIGHT_NORMAL;
    if (FAILED(ctx.dw->CreateTextFormat(
            PickUiFontFamily(ctx.dw), nullptr, base_weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, std::max(8.0f, font_size), L"", fmt.Put()))) {
        return false;
    }

    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    switch (align) {
        case TextAlign::Center: fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); break;
        case TextAlign::Right: fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); break;
        default: fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); break;
    }
    if (!is_heading) {
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.30f, 1.12f);
    } else {
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.18f, 1.05f);
    }

    if (FAILED(ctx.dw->CreateTextLayout(
            disp.c_str(), static_cast<UINT32>(disp.size()), fmt.Get(),
            std::max(24.0f, width), 100000.0f, item.layout.Put()))) {
        return false;
    }

    for (const auto& s : spans) {
        DWRITE_TEXT_RANGE r{s.start, s.len};
        if (s.math_render) {
            ComPtr<InlineMathObject> obj(new InlineMathObject(s.math_render, math_size, math_fg));
            item.layout->SetInlineObject(obj.Get(), r);
            if (s.has_color) {
                ComPtr<SpanColorEffect> eff(new SpanColorEffect(s.color));
                item.layout->SetDrawingEffect(eff.Get(), r);
            }
            continue;
        }
        if (s.img_obj) {
            ComPtr<InlineImageObject> obj(new InlineImageObject(s.img_obj, font_size * 1.45f, ctx.theme->text_muted));
            item.layout->SetInlineObject(obj.Get(), r);
            continue;
        }

        if (s.bold) item.layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
        if (s.italic) item.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
        if (s.strike) item.layout->SetStrikethrough(TRUE, r);
        if (s.underline) item.layout->SetUnderline(TRUE, r);
        if (s.mono) {
            item.layout->SetFontFamilyName(PickMonoFontFamily(ctx.dw), r);
            ComPtr<IDWriteTextLayout1> tl1;
            if (SUCCEEDED(item.layout->QueryInterface(__uuidof(IDWriteTextLayout1), tl1.PutVoid())) && tl1 && s.len > 0) {
                if (s.len == 1) {
                    tl1->SetCharacterSpacing(3.5f, 3.5f, 0.0f, {s.start, 1});
                } else {
                    tl1->SetCharacterSpacing(3.5f, 0.0f, 0.0f, {s.start, 1});
                    tl1->SetCharacterSpacing(0.0f, 3.5f, 0.0f, {s.start + s.len - 1, 1});
                }
            }
        }
        if (s.has_custom_size) item.layout->SetFontSize(std::max(8.0f, s.custom_size), r);
        if (s.has_color) {
            ComPtr<SpanColorEffect> eff(new SpanColorEffect(s.color));
            item.layout->SetDrawingEffect(eff.Get(), r);
        }
    }

    DWRITE_TEXT_METRICS tm{};
    item.layout->GetMetrics(&tm);
    out_height = std::max(font_size, tm.height);
    return true;
}

float DocumentLayout::LayoutCodeBlock(
    const Block& b, float x, float y, float width, const LayoutContext& ctx) {
    CodeBlockItem item;
    item.lang = b.code_lang;
    item.code = b.code_text;

    const float header_h = 30.0f * ctx.scale;
    const float pad_x = 16.0f * ctx.scale;
    const float pad_y = 12.0f * ctx.scale;
    const float code_size = std::max(10.0f, ctx.body_size * 0.86f);

    ComPtr<IDWriteTextFormat> fmt;
    if (SUCCEEDED(ctx.dw->CreateTextFormat(
            PickMonoFontFamily(ctx.dw), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            code_size, L"", fmt.Put()))) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.32f, 1.12f);

        const std::wstring& code_str = item.code.empty() ? L" " : item.code;
        if (SUCCEEDED(ctx.dw->CreateTextLayout(
                code_str.c_str(), static_cast<UINT32>(code_str.size()), fmt.Get(),
                100000.0f, 100000.0f, item.layout.Put())) && item.layout) {
            ComPtr<SpanColorEffect> base_eff(new SpanColorEffect(ctx.theme->syn_normal));
            item.layout->SetDrawingEffect(base_eff.Get(), {0, static_cast<UINT32>(code_str.size())});
            for (const auto& tok : b.code_tokens) {
                if (tok.start + tok.length <= code_str.size()) {
                    ComPtr<SpanColorEffect> eff(new SpanColorEffect(SyntaxTokenColor(*ctx.theme, tok.kind)));
                    item.layout->SetDrawingEffect(eff.Get(), {tok.start, tok.length});
                }
            }
        }
    }

    float code_h = code_size * 1.5f;
    float code_w = 100.0f;
    if (item.layout) {
        DWRITE_TEXT_METRICS tm{};
        item.layout->GetMetrics(&tm);
        code_h = std::max(code_size * 1.4f, tm.height);
        code_w = tm.widthIncludingTrailingWhitespace;
    }

    float body_w = std::max(40.0f, width - 2.0f * pad_x);
    item.content_width = code_w;
    item.max_scroll_x = std::max(0.0f, code_w - body_w);
    const float hscroll_space = (item.max_scroll_x > 1.0f) ? (12.0f * ctx.scale) : 0.0f;
    const float total_h = header_h + pad_y + code_h + pad_y + hscroll_space;

    item.card_rect = D2D1::RectF(x, y, x + width, y + total_h);
    item.header_rect = D2D1::RectF(x, y, x + width, y + header_h);
    float btn_w = 62.0f * ctx.scale;
    float btn_h = 20.0f * ctx.scale;
    item.copy_btn_rect = D2D1::RectF(
        x + width - btn_w - 10.0f * ctx.scale,
        y + (header_h - btn_h) * 0.5f,
        x + width - 10.0f * ctx.scale,
        y + (header_h + btn_h) * 0.5f);
    item.body_rect = D2D1::RectF(x + pad_x, y + header_h + pad_y, x + width - pad_x, y + header_h + pad_y + code_h);

    if (item.max_scroll_x > 1.0f) {
        float track_y = item.card_rect.bottom - 9.0f * ctx.scale;
        item.hscroll_track_rect = D2D1::RectF(x + pad_x, track_y, x + width - pad_x, track_y + 5.5f * ctx.scale);
        float track_w = item.hscroll_track_rect.right - item.hscroll_track_rect.left;
        float thumb_w = std::max(28.0f * ctx.scale, track_w * (body_w / code_w));
        item.hscroll_thumb_rect = D2D1::RectF(
            item.hscroll_track_rect.left, item.hscroll_track_rect.top,
            item.hscroll_track_rect.left + thumb_w, item.hscroll_track_rect.bottom);
    }

    item.selectable_id = static_cast<int>(selectables_.size());
    selectables_.push_back({true, code_blocks_.size()});
    code_blocks_.push_back(std::move(item));
    return y + total_h;
}

float DocumentLayout::LayoutDisplayMath(
    const Block& b, float x, float y, float width, const LayoutContext& ctx) {
    DisplayMathItem item;
    item.tex = b.math_tex;
    item.text_size = ctx.body_size * 1.18f;
    float avail_w = std::max(80.0f, width - 36.0f * ctx.scale);
    item.render = MathEngine::Instance().GetOrParse(b.math_tex, true, avail_w);

    const float pad_y = 14.0f * ctx.scale;
    if (item.render) {
        item.render->setTextSize(item.text_size);
        float nat_w = std::max(8.0f, static_cast<float>(item.render->getWidth()));
        float nat_h = std::max(16.0f, static_cast<float>(item.render->getHeight()));
        item.draw_scale = (nat_w > avail_w) ? std::max(0.68f, avail_w / nat_w) : 1.0f;
        item.formula_w = nat_w * item.draw_scale;
        item.formula_h = nat_h * item.draw_scale;
        item.max_scroll_x = std::max(0.0f, item.formula_w - avail_w);
    } else {
        item.formula_w = avail_w;
        item.formula_h = 28.0f * ctx.scale;
    }

    float total_h = item.formula_h + 2.0f * pad_y;
    item.card_rect = D2D1::RectF(x, y, x + width, y + total_h);
    float btn_w = 66.0f * ctx.scale;
    float btn_h = 20.0f * ctx.scale;
    item.copy_btn_rect = D2D1::RectF(
        x + width - btn_w - 8.0f * ctx.scale,
        y + 6.0f * ctx.scale,
        x + width - 8.0f * ctx.scale,
        y + 6.0f * ctx.scale + btn_h);

    math_blocks_.push_back(std::move(item));
    return y + total_h;
}

float DocumentLayout::LayoutImageBlock(
    const Block& b, float x, float y, float width, const LayoutContext& ctx) {
    ImageItem item;
    item.resolved_src = ResolveDocumentResourcePath(ctx.doc->base_dir, b.image_src);
    item.alt = b.image_alt;
    item.title = b.image_title;
    item.link_href = b.image_link_href;
    item.image = ctx.images->GetOrLoad(item.resolved_src);

    float max_img_w = std::max(80.0f, width);
    float draw_w = std::min(max_img_w, 360.0f * ctx.scale);
    float draw_h = 120.0f * ctx.scale;

    if (item.image && item.image->status == LoadedImage::Status::Ready &&
        item.image->width > 0.0f && item.image->height > 0.0f) {
        float nat_w = item.image->width;
        float nat_h = item.image->height;
        if (item.image->is_svg && nat_w < 240.0f * ctx.scale) {
            float boost = (240.0f * ctx.scale) / nat_w;
            nat_w *= boost;
            nat_h *= boost;
        }
        float fit = (nat_w > max_img_w) ? (max_img_w / nat_w) : 1.0f;
        draw_w = nat_w * fit;
        draw_h = nat_h * fit;
    }

    float img_x = x + (width - draw_w) * 0.5f;
    float img_y = y + 4.0f * ctx.scale;
    item.img_rect = D2D1::RectF(img_x, img_y, img_x + draw_w, img_y + draw_h);

    float bottom_y = item.img_rect.bottom;
    std::wstring caption = !item.title.empty() ? item.title : (item.alt.size() <= 100 ? item.alt : L"");
    if (!caption.empty()) {
        item.caption_layout = MakeSimpleLabel(ctx.dw, caption, ctx.body_size * 0.84f, false, false, width, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (item.caption_layout) {
            DWRITE_TEXT_METRICS tm{};
            item.caption_layout->GetMetrics(&tm);
            item.caption_rect = D2D1::RectF(x, bottom_y + 6.0f * ctx.scale, x + width, bottom_y + 6.0f * ctx.scale + tm.height);
            bottom_y = item.caption_rect.bottom;
        }
    }
    item.card_rect = D2D1::RectF(x, y, x + width, bottom_y + 4.0f * ctx.scale);
    image_items_.push_back(std::move(item));
    return bottom_y + 4.0f * ctx.scale;
}

float DocumentLayout::LayoutTable(
    const Block& b, float x, float y, float width, const LayoutContext& ctx) {
    if (b.table_rows.empty() || b.table_cols == 0) return y;
    const uint32_t cols = b.table_cols;
    const float pad_x = 12.0f * ctx.scale;
    const float pad_y = 9.0f * ctx.scale;
    const float cell_size = ctx.body_size * 0.94f;

    // Pass 1: Measure true DirectWrite minimum (unbreakable inline math/tokens) & natural widths per column.
    std::vector<float> col_min(cols, 76.0f * ctx.scale);
    std::vector<float> col_nat(cols, 76.0f * ctx.scale);
    for (const auto& row : b.table_rows) {
        for (uint32_t c = 0; c < cols && c < row.cells.size(); ++c) {
            ParagraphData cell_p = row.cells[c].content;
            if (row.is_header) {
                for (auto& run : cell_p.runs) run.bold = true;
            }
            RichTextItem meas;
            float mh = 0.0f;
            if (BuildParagraphItem(cell_p, 8000.0f, cell_size, false, 0,
                                   ctx.theme->text_primary, ctx, row.cells[c].align, meas, mh) &&
                meas.layout) {
                DWRITE_TEXT_METRICS tm{};
                meas.layout->GetMetrics(&tm);
                float nat_w = tm.widthIncludingTrailingWhitespace + 2.0f * pad_x + 4.0f * ctx.scale;
                float min_w = 0.0f;
                meas.layout->DetermineMinWidth(&min_w);
                min_w += 2.0f * pad_x + 4.0f * ctx.scale;
                if (row.is_header) {
                    // Keep short headers on 1-2 readable lines rather than wrapping per-character.
                    min_w = std::max(min_w, std::min(nat_w, 128.0f * ctx.scale));
                }
                col_min[c] = std::max(col_min[c], min_w);
                col_nat[c] = std::max(col_nat[c], nat_w);
            }
        }
    }

    const float target_cap = ((cols >= 5) ? 215.0f : 270.0f) * ctx.scale;
    const float floor_cap = 152.0f * ctx.scale;
    std::vector<float> col_target(cols);
    std::vector<float> col_floor(cols);
    float sum_target = 0.0f;
    float sum_floor = 0.0f;
    for (uint32_t c = 0; c < cols; ++c) {
        col_target[c] = std::max(col_min[c], std::min(col_nat[c], target_cap));
        col_floor[c] = std::max(col_min[c], std::min(col_target[c], floor_cap));
        sum_target += col_target[c];
        sum_floor += col_floor[c];
    }

    std::vector<float> col_widths(cols);
    float table_w = width;
    if (sum_target <= width) {
        col_widths = col_target;
        float extra = width - sum_target;
        float slack_sum = 0.0f;
        for (uint32_t c = 0; c < cols; ++c) slack_sum += std::max(0.0f, col_nat[c] - col_target[c]);
        for (uint32_t c = 0; c < cols; ++c) {
            float weight = (slack_sum > 1.0f)
                ? (std::max(0.0f, col_nat[c] - col_target[c]) / slack_sum)
                : (1.0f / static_cast<float>(cols));
            col_widths[c] += extra * weight;
        }
    } else if (sum_floor <= width) {
        col_widths = col_floor;
        float extra = width - sum_floor;
        float diff_sum = std::max(1.0f, sum_target - sum_floor);
        for (uint32_t c = 0; c < cols; ++c) {
            col_widths[c] += extra * ((col_target[c] - col_floor[c]) / diff_sum);
        }
    } else {
        // Wide table: do not crush columns below readable floor; enable horizontal scroll cleanly.
        col_widths = col_floor;
        table_w = sum_floor;
    }

    TableItem tbl;
    int tbl_idx = static_cast<int>(table_items_.size());
    tbl.content_width = table_w;
    tbl.max_scroll_x = std::max(0.0f, table_w - width);
    tbl.col_widths = col_widths;

    float cur_y = y;
    for (size_t r = 0; r < b.table_rows.size(); ++r) {
        const auto& row = b.table_rows[r];
        TableRowItem trow;
        trow.is_header = row.is_header;

        float max_cell_h = ctx.body_size * 1.4f;
        std::vector<int> row_cell_indices;
        float cur_x = x;
        for (uint32_t c = 0; c < cols; ++c) {
            float cw = col_widths[c];
            if (c < row.cells.size()) {
                ParagraphData cell_p = row.cells[c].content;
                if (row.is_header) {
                    for (auto& run : cell_p.runs) run.bold = true;
                }
                RichTextItem item;
                float ch = 0.0f;
                if (BuildParagraphItem(
                        cell_p, std::max(24.0f, cw - 2.0f * pad_x), cell_size,
                        false, 0, ctx.theme->text_primary, ctx, row.cells[c].align, item, ch)) {
                    ch += 2.0f * ctx.scale;  // Extra descender/CJK clearance above bottom border
                    item.rect = D2D1::RectF(cur_x + pad_x, cur_y + pad_y, cur_x + cw - pad_x, cur_y + pad_y + ch);
                    item.table_idx = tbl_idx;
                    item.selectable_id = static_cast<int>(selectables_.size());
                    int rich_idx = static_cast<int>(rich_items_.size());
                    selectables_.push_back({false, static_cast<size_t>(rich_idx)});
                    rich_items_.push_back(std::move(item));
                    row_cell_indices.push_back(rich_idx);
                    trow.cell_rich_indices.push_back(rich_idx);
                    max_cell_h = std::max(max_cell_h, ch);
                }
            }
            cur_x += cw;
        }

        float row_h = max_cell_h + 2.0f * pad_y;
        trow.rect = D2D1::RectF(x, cur_y, x + table_w, cur_y + row_h);

        // Set per-cell clip bounds spanning full row height
        cur_x = x;
        size_t ci_pos = 0;
        for (uint32_t c = 0; c < cols; ++c) {
            float cw = col_widths[c];
            if (c < row.cells.size() && ci_pos < row_cell_indices.size()) {
                int ridx = row_cell_indices[ci_pos];
                // Only match cells belonging to this column index (skip empty skipped cells if any)
                if (rich_items_[ridx].rect.left >= cur_x - 1.0f && rich_items_[ridx].rect.left <= cur_x + cw) {
                    rich_items_[ridx].cell_clip = D2D1::RectF(cur_x + 1.0f, cur_y + 1.0f, cur_x + cw - 1.0f, cur_y + row_h - 1.0f);
                    ++ci_pos;
                }
            }
            cur_x += cw;
        }

        RecordSourceAnchor(row.source_line, row.source_line, cur_y, cur_y + row_h);
        tbl.rows.push_back(std::move(trow));
        cur_y += row_h;
    }

    const float hscroll_space = (tbl.max_scroll_x > 1.0f) ? (16.0f * ctx.scale) : 0.0f;
    tbl.body_rect = D2D1::RectF(x, y, x + width, cur_y);
    tbl.card_rect = D2D1::RectF(x, y, x + width, cur_y + hscroll_space);
    if (tbl.max_scroll_x > 1.0f) {
        float pad_s = 10.0f * ctx.scale;
        float track_y = cur_y + 5.0f * ctx.scale;
        tbl.hscroll_track_rect = D2D1::RectF(x + pad_s, track_y, x + width - pad_s, track_y + 6.0f * ctx.scale);
        float track_w = std::max(10.0f, tbl.hscroll_track_rect.right - tbl.hscroll_track_rect.left);
        float thumb_w = std::max(28.0f * ctx.scale, track_w * (width / table_w));
        tbl.hscroll_thumb_rect = D2D1::RectF(
            tbl.hscroll_track_rect.left, tbl.hscroll_track_rect.top,
            tbl.hscroll_track_rect.left + thumb_w, tbl.hscroll_track_rect.bottom);
    }

    table_items_.push_back(std::move(tbl));
    return cur_y + hscroll_space;
}

void DocumentLayout::Render(
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
    int active_hscroll_code_idx,
    int active_hscroll_table_idx) {
    auto* d2d = D2DContext::Instance();
    if (!dc || !d2d) return;

    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(theme.text_primary, brush.Put());
    if (!brush) return;

    ComPtr<D2DTextRenderer> text_renderer(new D2DTextRenderer(dc, theme.text_primary));

    const float vis_top = scroll_y - 120.0f;
    const float vis_bot = scroll_y + viewport_height + 120.0f;
    auto is_visible = [&](const D2D1_RECT_F& r) {
        return r.bottom >= vis_top && r.top <= vis_bot;
    };

    auto draw_decor = [&](const DecorBox& d) {
        if (!is_visible(d.rect)) return;
        switch (d.kind) {
            case DecorBox::Kind::RectFill:
                if (d.fill.a > 0.0f) {
                    brush->SetColor(d.fill);
                    dc->FillRectangle(d.rect, brush.Get());
                }
                break;
            case DecorBox::Kind::RoundedCard:
                if (d.fill.a > 0.0f) {
                    brush->SetColor(d.fill);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(d.rect, d.radius, d.radius), brush.Get());
                }
                if (d.stroke.a > 0.0f) {
                    brush->SetColor(d.stroke);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(d.rect, d.radius, d.radius), brush.Get(), d.stroke_width);
                }
                break;
            case DecorBox::Kind::LeftBar:
                brush->SetColor(d.fill);
                dc->FillRoundedRectangle(D2D1::RoundedRect(d.rect, d.radius, d.radius), brush.Get());
                break;
            case DecorBox::Kind::RuleLine:
                brush->SetColor(d.fill);
                dc->FillRectangle(d.rect, brush.Get());
                break;
            case DecorBox::Kind::Checkbox: {
                brush->SetColor(d.fill);
                dc->FillRoundedRectangle(D2D1::RoundedRect(d.rect, d.radius, d.radius), brush.Get());
                brush->SetColor(d.stroke);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(d.rect, d.radius, d.radius), brush.Get(), 1.2f);
                if (d.checked) {
                    float w = d.rect.right - d.rect.left;
                    float h = d.rect.bottom - d.rect.top;
                    brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
                    dc->DrawLine(
                        D2D1::Point2F(d.rect.left + w * 0.24f, d.rect.top + h * 0.52f),
                        D2D1::Point2F(d.rect.left + w * 0.44f, d.rect.top + h * 0.72f),
                        brush.Get(), 1.8f);
                    dc->DrawLine(
                        D2D1::Point2F(d.rect.left + w * 0.44f, d.rect.top + h * 0.72f),
                        D2D1::Point2F(d.rect.left + w * 0.78f, d.rect.top + h * 0.28f),
                        brush.Get(), 1.8f);
                }
                break;
            }
        }
    };

    for (const auto& d : decor_behind_) draw_decor(d);

    // Normalized selection coordinates
    int sel_s_id = selection.start_sel;
    uint32_t sel_s_pos = selection.start_pos;
    int sel_e_id = selection.end_sel;
    uint32_t sel_e_pos = selection.end_pos;
    if (selection.HasSelection() && (sel_s_id > sel_e_id || (sel_s_id == sel_e_id && sel_s_pos > sel_e_pos))) {
        std::swap(sel_s_id, sel_e_id);
        std::swap(sel_s_pos, sel_e_pos);
    }

    auto paint_sel_and_search = [&](IDWriteTextLayout* tl, uint32_t text_len, int sel_id, float ox, float oy) {
        if (!tl || text_len == 0) return;
        for (size_t mi = 0; mi < search_matches.size(); ++mi) {
            const auto& m = search_matches[mi];
            if (m.selectable_id != sel_id) continue;
            brush->SetColor((static_cast<int>(mi) == active_search_idx) ? theme.search_active_bg : theme.search_match_bg);
            for (const auto& hb : HitTestRangeBoxes(tl, m.start, m.length, ox, oy)) {
                dc->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(hb.left, hb.top, hb.left + hb.width, hb.top + hb.height), 2.5f, 2.5f),
                    brush.Get());
            }
        }
        if (selection.HasSelection() && sel_id >= sel_s_id && sel_id <= sel_e_id) {
            uint32_t s0 = (sel_id == sel_s_id) ? sel_s_pos : 0;
            uint32_t s1 = (sel_id == sel_e_id) ? sel_e_pos : text_len;
            if (s1 > s0) {
                brush->SetColor(theme.selection_bg);
                for (const auto& hb : HitTestRangeBoxes(tl, s0, s1 - s0, ox, oy)) {
                    dc->FillRectangle(D2D1::RectF(hb.left, hb.top, hb.left + hb.width, hb.top + hb.height), brush.Get());
                }
            }
        }
    };

    auto draw_rich_item = [&](const RichTextItem& item, float draw_ox, float draw_oy) {
        if (!item.layout) return;
        for (const auto& mk : item.mark_highlights) {
            brush->SetColor(theme.highlight_bg);
            for (const auto& hb : HitTestRangeBoxes(item.layout.Get(), mk.start, mk.length, draw_ox, draw_oy)) {
                dc->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(hb.left - 1.5f, hb.top + 1.0f, hb.left + hb.width + 1.5f, hb.top + hb.height - 1.0f), 3.0f, 3.0f),
                    brush.Get());
            }
        }
        for (const auto& pill : item.code_pills) {
            for (const auto& hb : HitTestRangeBoxes(item.layout.Get(), pill.start, pill.length, draw_ox, draw_oy)) {
                D2D1_RECT_F pr = D2D1::RectF(hb.left + 1.5f, hb.top + 1.0f, hb.left + hb.width - 1.5f, hb.top + hb.height - 1.0f);
                brush->SetColor(theme.code_inline_bg);
                dc->FillRoundedRectangle(D2D1::RoundedRect(pr, 4.0f, 4.0f), brush.Get());
                brush->SetColor(theme.code_inline_border);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(pr, 4.0f, 4.0f), brush.Get(), 0.8f);
            }
        }
        paint_sel_and_search(item.layout.Get(), static_cast<uint32_t>(item.text.size()), item.selectable_id, draw_ox, draw_oy);
        item.layout->Draw(dc, text_renderer.Get(), draw_ox, draw_oy);
    };

    // Rich text items outside tables (paragraphs, headings, list text, blockquotes)
    for (const auto& item : rich_items_) {
        if (item.table_idx >= 0) continue;
        if (!is_visible(item.rect)) continue;
        draw_rich_item(item, item.rect.left, item.rect.top);
    }

    // Horizontally scrollable tables
    for (size_t tidx = 0; tidx < table_items_.size(); ++tidx) {
        const auto& tbl = table_items_[tidx];
        if (!is_visible(tbl.card_rect)) continue;

        dc->PushAxisAlignedClip(tbl.body_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        D2D1_MATRIX_3X2_F saved{};
        dc->GetTransform(&saved);
        D2D1::Matrix3x2F scroll_xform = D2D1::Matrix3x2F::Translation(-tbl.scroll_x, 0.0f);
        dc->SetTransform(scroll_xform * saved);

        const float gx0 = tbl.body_rect.left;
        const float gx1 = gx0 + tbl.content_width;
        const float gy0 = tbl.body_rect.top;
        const float gy1 = tbl.body_rect.bottom;

        for (size_t r = 0; r < tbl.rows.size(); ++r) {
            const auto& row = tbl.rows[r];
            if (!is_visible(row.rect)) continue;

            if (row.is_header) brush->SetColor(theme.table_header_bg);
            else if (r % 2 == 1) brush->SetColor(theme.table_stripe_bg);
            else brush->SetColor(theme.bg_canvas);
            dc->FillRectangle(row.rect, brush.Get());

            for (int ridx : row.cell_rich_indices) {
                if (ridx < 0 || ridx >= static_cast<int>(rich_items_.size())) continue;
                const auto& cell_rt = rich_items_[ridx];
                dc->PushAxisAlignedClip(cell_rt.cell_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                draw_rich_item(cell_rt, cell_rt.rect.left, cell_rt.rect.top);
                dc->PopAxisAlignedClip();
            }

            if (r + 1 < tbl.rows.size()) {
                brush->SetColor(theme.table_border);
                dc->FillRectangle(D2D1::RectF(gx0, row.rect.bottom, gx1, row.rect.bottom + 1.0f), brush.Get());
            }
        }

        // Column vertical dividers
        float col_x = gx0;
        brush->SetColor(theme.table_border);
        for (size_t c = 0; c + 1 < tbl.col_widths.size(); ++c) {
            col_x += tbl.col_widths[c];
            dc->FillRectangle(D2D1::RectF(col_x, gy0, col_x + 1.0f, gy1), brush.Get());
        }

        dc->SetTransform(saved);
        dc->PopAxisAlignedClip();

        if (tbl.max_scroll_x > 1.0f) {
            brush->SetColor(theme.table_stripe_bg);
            dc->FillRectangle(D2D1::RectF(tbl.body_rect.left, tbl.body_rect.bottom, tbl.body_rect.right, tbl.card_rect.bottom), brush.Get());
            brush->SetColor(theme.table_border);
            dc->FillRectangle(D2D1::RectF(tbl.body_rect.left, tbl.body_rect.bottom, tbl.body_rect.right, tbl.body_rect.bottom + 1.0f), brush.Get());

            brush->SetColor(theme.border);
            dc->FillRoundedRectangle(D2D1::RoundedRect(tbl.hscroll_track_rect, 3.0f, 3.0f), brush.Get());
            float track_w = tbl.hscroll_track_rect.right - tbl.hscroll_track_rect.left;
            float thumb_w = tbl.hscroll_thumb_rect.right - tbl.hscroll_thumb_rect.left;
            float ratio = tbl.scroll_x / tbl.max_scroll_x;
            float thumb_x = tbl.hscroll_track_rect.left + (track_w - thumb_w) * ratio;
            D2D1_RECT_F tr = D2D1::RectF(thumb_x, tbl.hscroll_track_rect.top, thumb_x + thumb_w, tbl.hscroll_track_rect.bottom);
            brush->SetColor(static_cast<int>(tidx) == active_hscroll_table_idx ? theme.scrollbar_thumb_hover : theme.scrollbar_thumb);
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 3.0f, 3.0f), brush.Get());
        }

        brush->SetColor(theme.table_border);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(tbl.card_rect, 6.0f, 6.0f), brush.Get(), 1.0f);
    }

    // Code blocks
    for (size_t idx = 0; idx < code_blocks_.size(); ++idx) {
        const auto& cb = code_blocks_[idx];
        if (!is_visible(cb.card_rect)) continue;

        brush->SetColor(theme.code_block_bg);
        dc->FillRoundedRectangle(D2D1::RoundedRect(cb.card_rect, 8.0f, 8.0f), brush.Get());

        // Header bar (clipped to top rounded corners)
        dc->PushAxisAlignedClip(cb.header_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        brush->SetColor(theme.code_block_header_bg);
        dc->FillRoundedRectangle(D2D1::RoundedRect(cb.card_rect, 8.0f, 8.0f), brush.Get());
        dc->PopAxisAlignedClip();

        brush->SetColor(theme.code_block_border);
        dc->DrawLine(
            D2D1::Point2F(cb.header_rect.left, cb.header_rect.bottom),
            D2D1::Point2F(cb.header_rect.right, cb.header_rect.bottom),
            brush.Get(), 1.0f);

        // Language tag
        std::wstring lang_w = cb.lang.empty() ? L"TEXT" : Utf8ToWide(cb.lang);
        if (auto ll = MakeSimpleLabel(d2d->DWrite(), lang_w, 11.5f * scale_, true, true, 180.0f)) {
            DWRITE_TEXT_METRICS tm{};
            ll->GetMetrics(&tm);
            brush->SetColor(theme.text_secondary);
            dc->DrawTextLayout(
                D2D1::Point2F(cb.header_rect.left + 14.0f * scale_,
                              cb.header_rect.top + ((cb.header_rect.bottom - cb.header_rect.top) - tm.height) * 0.5f),
                ll.Get(), brush.Get());
        }

        // Copy button
        const bool is_copied = (static_cast<int>(idx) == copied_code_idx);
        const bool is_hovered = (static_cast<int>(idx) == hovered_copy_code_idx);
        DrawSmallPillButton(
            dc, brush.Get(), d2d->DWrite(), cb.copy_btn_rect,
            is_copied ? L"\u2713 Copied" : L"Copy",
            is_hovered ? theme.bg_hover : theme.bg_elevated,
            is_copied ? theme.accent : theme.border,
            is_copied ? theme.accent : theme.text_secondary,
            11.0f * scale_);

        // Code body clipped + horizontally scrolled
        if (cb.layout) {
            dc->PushAxisAlignedClip(cb.body_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float tx = cb.body_rect.left - cb.scroll_x;
            float ty = cb.body_rect.top;
            paint_sel_and_search(cb.layout.Get(), static_cast<uint32_t>(cb.code.size()), cb.selectable_id, tx, ty);
            cb.layout->Draw(dc, text_renderer.Get(), tx, ty);
            dc->PopAxisAlignedClip();
        }

        // Optional horizontal scrollbar
        if (cb.max_scroll_x > 1.0f) {
            brush->SetColor(theme.border);
            dc->FillRoundedRectangle(D2D1::RoundedRect(cb.hscroll_track_rect, 2.0f, 2.0f), brush.Get());
            float track_w = cb.hscroll_track_rect.right - cb.hscroll_track_rect.left;
            float thumb_w = cb.hscroll_thumb_rect.right - cb.hscroll_thumb_rect.left;
            float ratio = cb.scroll_x / cb.max_scroll_x;
            float thumb_x = cb.hscroll_track_rect.left + (track_w - thumb_w) * ratio;
            D2D1_RECT_F tr = D2D1::RectF(thumb_x, cb.hscroll_track_rect.top, thumb_x + thumb_w, cb.hscroll_track_rect.bottom);
            brush->SetColor(static_cast<int>(idx) == active_hscroll_code_idx ? theme.scrollbar_thumb_hover : theme.scrollbar_thumb);
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 2.5f, 2.5f), brush.Get());
        }

        brush->SetColor(theme.code_block_border);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(cb.card_rect, 8.0f, 8.0f), brush.Get(), 1.0f);
    }

    // Display math blocks (centered!)
    for (size_t idx = 0; idx < math_blocks_.size(); ++idx) {
        const auto& mb = math_blocks_[idx];
        if (!is_visible(mb.card_rect)) continue;

        brush->SetColor(theme.math_block_bg);
        dc->FillRoundedRectangle(D2D1::RoundedRect(mb.card_rect, 8.0f, 8.0f), brush.Get());
        brush->SetColor(theme.math_block_border);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(mb.card_rect, 8.0f, 8.0f), brush.Get(), 1.0f);

        if (mb.render) {
            mb.render->setTextSize(mb.text_size);
            mb.render->setForeground(ColorToTexArgb(theme.text_primary));

            float card_w = mb.card_rect.right - mb.card_rect.left;
            float card_h = mb.card_rect.bottom - mb.card_rect.top;
            float draw_x = (mb.max_scroll_x > 1.0f)
                ? (mb.card_rect.left + 18.0f * scale_ - mb.scroll_x)
                : (mb.card_rect.left + (card_w - mb.formula_w) * 0.5f);
            float draw_y = mb.card_rect.top + (card_h - mb.formula_h) * 0.5f;

            dc->PushAxisAlignedClip(mb.card_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            D2D1_MATRIX_3X2_F saved{};
            dc->GetTransform(&saved);
            D2D1::Matrix3x2F xform = D2D1::Matrix3x2F::Scale(mb.draw_scale, mb.draw_scale) *
                                     D2D1::Matrix3x2F::Translation(draw_x, draw_y);
            dc->SetTransform(xform * saved);
            Graphics2D_d2d g2(dc);
            mb.render->draw(g2, 0, 0);
            dc->SetTransform(saved);
            dc->PopAxisAlignedClip();
        } else {
            if (auto fl = MakeSimpleLabel(d2d->DWrite(), L"LaTeX: " + mb.tex, 12.5f * scale_, false, true,
                                          mb.card_rect.right - mb.card_rect.left - 24.0f, DWRITE_TEXT_ALIGNMENT_CENTER)) {
                brush->SetColor(theme.adm_warning_accent);
                dc->DrawTextLayout(D2D1::Point2F(mb.card_rect.left + 12.0f, mb.card_rect.top + 12.0f), fl.Get(), brush.Get());
            }
        }

        const bool show_btn = (static_cast<int>(idx) == hovered_copy_math_idx || static_cast<int>(idx) == copied_math_idx);
        if (show_btn) {
            const bool is_copied = (static_cast<int>(idx) == copied_math_idx);
            DrawSmallPillButton(
                dc, brush.Get(), d2d->DWrite(), mb.copy_btn_rect,
                is_copied ? L"\u2713 TeX" : L"Copy TeX",
                theme.bg_elevated,
                is_copied ? theme.accent : theme.border,
                is_copied ? theme.accent : theme.text_secondary,
                10.5f * scale_);
        }
    }

    // Standalone image blocks
    for (const auto& img : image_items_) {
        if (!is_visible(img.card_rect)) continue;
        if (img.image && img.image->status == LoadedImage::Status::Ready) {
            DrawLoadedImage(dc, *img.image, img.img_rect);
            brush->SetColor(theme.border);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(img.img_rect, 4.0f, 4.0f), brush.Get(), 0.75f);
        } else {
            brush->SetColor(theme.bg_sidebar);
            dc->FillRoundedRectangle(D2D1::RoundedRect(img.img_rect, 8.0f, 8.0f), brush.Get());
            brush->SetColor(theme.border);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(img.img_rect, 8.0f, 8.0f), brush.Get(), 1.0f);

            std::wstring msg = (img.image && img.image->status == LoadedImage::Status::Loading)
                ? L"正在加载图片..."
                : (img.alt.empty() ? L"[图片不可用]" : (L"[图片] " + img.alt));
            if (auto ml = MakeSimpleLabel(d2d->DWrite(), msg, 12.5f * scale_, false, false,
                                          img.img_rect.right - img.img_rect.left - 24.0f, DWRITE_TEXT_ALIGNMENT_CENTER)) {
                DWRITE_TEXT_METRICS tm{};
                ml->GetMetrics(&tm);
                brush->SetColor(theme.text_muted);
                dc->DrawTextLayout(
                    D2D1::Point2F(img.img_rect.left + 12.0f,
                                  img.img_rect.top + ((img.img_rect.bottom - img.img_rect.top) - tm.height) * 0.5f),
                    ml.Get(), brush.Get());
            }
        }

        if (img.caption_layout) {
            brush->SetColor(theme.text_muted);
            dc->DrawTextLayout(D2D1::Point2F(img.caption_rect.left, img.caption_rect.top), img.caption_layout.Get(), brush.Get());
        }
    }

    for (const auto& d : decor_front_) draw_decor(d);
}

HitTestResult DocumentLayout::HitTest(float doc_x, float doc_y) const {
    HitTestResult res;

    for (size_t i = 0; i < code_blocks_.size(); ++i) {
        const auto& cb = code_blocks_[i];
        if (PointInRect(doc_x, doc_y, cb.copy_btn_rect)) {
            res.target = HitTestResult::Target::CopyCodeBtn;
            res.item_index = static_cast<int>(i);
            return res;
        }
        if (cb.max_scroll_x > 1.0f &&
            doc_x >= cb.hscroll_track_rect.left - 2.0f && doc_x <= cb.hscroll_track_rect.right + 2.0f &&
            doc_y >= cb.hscroll_track_rect.top - 5.0f && doc_y <= cb.card_rect.bottom + 2.0f) {
            res.target = HitTestResult::Target::CodeHScroll;
            res.item_index = static_cast<int>(i);
            return res;
        }
        if (PointInRect(doc_x, doc_y, cb.body_rect) && cb.layout) {
            BOOL trailing = FALSE, inside = FALSE;
            DWRITE_HIT_TEST_METRICS htm{};
            cb.layout->HitTestPoint(doc_x - (cb.body_rect.left - cb.scroll_x), doc_y - cb.body_rect.top, &trailing, &inside, &htm);
            res.target = HitTestResult::Target::Text;
            res.item_index = static_cast<int>(i);
            res.selectable_id = cb.selectable_id;
            res.text_pos = htm.textPosition + (trailing ? htm.length : 0);
            return res;
        }
    }

    for (size_t i = 0; i < math_blocks_.size(); ++i) {
        const auto& mb = math_blocks_[i];
        if (PointInRect(doc_x, doc_y, mb.copy_btn_rect)) {
            res.target = HitTestResult::Target::CopyMathBtn;
            res.item_index = static_cast<int>(i);
            return res;
        }
    }

    for (size_t i = 0; i < image_items_.size(); ++i) {
        const auto& img = image_items_[i];
        if (PointInRect(doc_x, doc_y, img.img_rect)) {
            if (!img.link_href.empty()) {
                res.target = HitTestResult::Target::Link;
                res.url = img.link_href;
            } else {
                res.target = HitTestResult::Target::Image;
                res.item_index = static_cast<int>(i);
            }
            return res;
        }
    }

    for (size_t i = 0; i < table_items_.size(); ++i) {
        const auto& tbl = table_items_[i];
        if (tbl.max_scroll_x > 1.0f &&
            doc_x >= tbl.hscroll_track_rect.left - 2.0f && doc_x <= tbl.hscroll_track_rect.right + 2.0f &&
            doc_y >= tbl.body_rect.bottom - 3.0f && doc_y <= tbl.card_rect.bottom + 2.0f) {
            res.target = HitTestResult::Target::TableHScroll;
            res.item_index = static_cast<int>(i);
            return res;
        }
    }

    for (size_t i = 0; i < rich_items_.size(); ++i) {
        const auto& item = rich_items_[i];
        if (!item.layout) continue;
        float sx = 0.0f;
        if (item.table_idx >= 0 && item.table_idx < static_cast<int>(table_items_.size())) {
            const auto& tbl = table_items_[item.table_idx];
            if (!PointInRect(doc_x, doc_y, tbl.body_rect)) continue;
            sx = tbl.scroll_x;
            if (!PointInRect(doc_x + sx, doc_y, item.cell_clip)) continue;
        } else if (!PointInRect(doc_x, doc_y, item.rect)) {
            continue;
        }
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS htm{};
        item.layout->HitTestPoint(doc_x - (item.rect.left - sx), doc_y - item.rect.top, &trailing, &inside, &htm);
        uint32_t pos = htm.textPosition + (trailing ? htm.length : 0);
        if (inside) {
            for (const auto& lk : item.links) {
                if (htm.textPosition >= lk.start && htm.textPosition < lk.start + lk.length) {
                    res.target = HitTestResult::Target::Link;
                    res.item_index = static_cast<int>(i);
                    res.selectable_id = item.selectable_id;
                    res.text_pos = pos;
                    res.url = lk.url;
                    return res;
                }
            }
        }
        res.target = HitTestResult::Target::Text;
        res.item_index = static_cast<int>(i);
        res.selectable_id = item.selectable_id;
        res.text_pos = pos;
        return res;
    }

    return res;
}

bool DocumentLayout::HitTestSelectableNearest(float doc_x, float doc_y, int& out_sel_id, uint32_t& out_pos) const {
    if (selectables_.empty()) return false;

    float best_dist = 1e30f;
    int best_id = -1;
    uint32_t best_pos = 0;

    for (int sid = 0; sid < static_cast<int>(selectables_.size()); ++sid) {
        const auto& slot = selectables_[sid];
        D2D1_RECT_F r{};
        IDWriteTextLayout* layout = nullptr;
        float ox = 0.0f, oy = 0.0f;
        if (slot.is_code) {
            const auto& cb = code_blocks_[slot.index];
            r = cb.body_rect;
            layout = cb.layout.Get();
            ox = cb.body_rect.left - cb.scroll_x;
            oy = cb.body_rect.top;
        } else {
            const auto& rt = rich_items_[slot.index];
            r = rt.rect;
            layout = rt.layout.Get();
            float sx = (rt.table_idx >= 0 && rt.table_idx < static_cast<int>(table_items_.size()))
                ? table_items_[rt.table_idx].scroll_x : 0.0f;
            ox = rt.rect.left - sx;
            oy = rt.rect.top;
            if (rt.table_idx >= 0 && rt.table_idx < static_cast<int>(table_items_.size())) {
                r.left = table_items_[rt.table_idx].body_rect.left;
                r.right = table_items_[rt.table_idx].body_rect.right;
            }
        }
        if (!layout) continue;

        float dy = (doc_y < r.top) ? (r.top - doc_y) : ((doc_y > r.bottom) ? (doc_y - r.bottom) : 0.0f);
        float dx = (doc_x < r.left) ? (r.left - doc_x) : ((doc_x > r.right) ? (doc_x - r.right) : 0.0f);
        float dist = dy * 4.0f + dx;
        if (dist < best_dist) {
            BOOL trailing = FALSE, inside = FALSE;
            DWRITE_HIT_TEST_METRICS htm{};
            layout->HitTestPoint(doc_x - ox, doc_y - oy, &trailing, &inside, &htm);
            best_dist = dist;
            best_id = sid;
            best_pos = htm.textPosition + (trailing ? htm.length : 0);
        }
    }

    if (best_id >= 0) {
        out_sel_id = best_id;
        out_pos = best_pos;
        return true;
    }
    return false;
}

std::wstring DocumentLayout::ExtractSelectedText(const TextSelection& sel) const {
    if (!sel.HasSelection() || selectables_.empty()) return {};
    int s_id = sel.start_sel;
    uint32_t s_pos = sel.start_pos;
    int e_id = sel.end_sel;
    uint32_t e_pos = sel.end_pos;
    if (s_id > e_id || (s_id == e_id && s_pos > e_pos)) {
        std::swap(s_id, e_id);
        std::swap(s_pos, e_pos);
    }
    s_id = std::clamp(s_id, 0, static_cast<int>(selectables_.size()) - 1);
    e_id = std::clamp(e_id, 0, static_cast<int>(selectables_.size()) - 1);

    std::wstring out;
    for (int sid = s_id; sid <= e_id; ++sid) {
        const auto& slot = selectables_[sid];
        if (!out.empty()) out.append(L"\r\n\r\n");

        if (slot.is_code) {
            const std::wstring& code = code_blocks_[slot.index].code;
            uint32_t a = (sid == s_id) ? std::min<uint32_t>(s_pos, static_cast<uint32_t>(code.size())) : 0;
            uint32_t b = (sid == e_id) ? std::min<uint32_t>(e_pos, static_cast<uint32_t>(code.size())) : static_cast<uint32_t>(code.size());
            if (b > a) {
                for (uint32_t k = a; k < b; ++k) {
                    if (code[k] == L'\n' && (k == a || code[k - 1] != L'\r')) out.push_back(L'\r');
                    out.push_back(code[k]);
                }
            }
        } else {
            const auto& rt = rich_items_[slot.index];
            uint32_t a = (sid == s_id) ? std::min<uint32_t>(s_pos, static_cast<uint32_t>(rt.text.size())) : 0;
            uint32_t b = (sid == e_id) ? std::min<uint32_t>(e_pos, static_cast<uint32_t>(rt.text.size())) : static_cast<uint32_t>(rt.text.size());
            for (uint32_t k = a; k < b; ++k) {
                if (rt.text[k] == L'\uFFFC') {
                    for (const auto& rep : rt.copy_replacements) {
                        if (rep.pos == k) {
                            out.append(rep.text);
                            break;
                        }
                    }
                } else if (rt.text[k] == L'\n') {
                    out.append(L"\r\n");
                } else {
                    out.push_back(rt.text[k]);
                }
            }
        }
    }
    return out;
}

std::wstring DocumentLayout::ExtractAllText() const {
    if (selectables_.empty()) return {};
    TextSelection all;
    all.start_sel = 0;
    all.start_pos = 0;
    all.end_sel = static_cast<int>(selectables_.size()) - 1;
    const auto& last = selectables_.back();
    all.end_pos = static_cast<uint32_t>(last.is_code ? code_blocks_[last.index].code.size() : rich_items_[last.index].text.size());
    return ExtractSelectedText(all);
}

void DocumentLayout::SelectWordAt(int sel_id, uint32_t pos, uint32_t& out_start, uint32_t& out_end) const {
    out_start = out_end = pos;
    if (sel_id < 0 || sel_id >= static_cast<int>(selectables_.size())) return;
    const auto& slot = selectables_[sel_id];
    const std::wstring& txt = slot.is_code ? code_blocks_[slot.index].code : rich_items_[slot.index].text;
    if (txt.empty()) return;
    uint32_t p = std::min<uint32_t>(pos, static_cast<uint32_t>(txt.size() - 1));
    auto is_word = [](wchar_t c) { return std::iswalnum(c) || c == L'_'; };
    if (!is_word(txt[p])) {
        out_start = p;
        out_end = p + 1;
        return;
    }
    uint32_t a = p, b = p + 1;
    while (a > 0 && is_word(txt[a - 1])) --a;
    while (b < txt.size() && is_word(txt[b])) ++b;
    out_start = a;
    out_end = b;
}

std::vector<SearchMatch> DocumentLayout::FindAllMatches(const std::wstring& query) const {
    std::vector<SearchMatch> matches;
    if (query.empty()) return matches;

    std::wstring q_low = query;
    for (wchar_t& ch : q_low) ch = static_cast<wchar_t>(std::towlower(ch));

    for (int sid = 0; sid < static_cast<int>(selectables_.size()); ++sid) {
        const auto& slot = selectables_[sid];
        const std::wstring& txt = slot.is_code ? code_blocks_[slot.index].code : rich_items_[slot.index].text;
        IDWriteTextLayout* layout = slot.is_code ? code_blocks_[slot.index].layout.Get() : rich_items_[slot.index].layout.Get();
        float base_y = slot.is_code ? code_blocks_[slot.index].body_rect.top : rich_items_[slot.index].rect.top;
        if (txt.empty()) continue;

        std::wstring low = txt;
        for (wchar_t& ch : low) ch = static_cast<wchar_t>(std::towlower(ch));

        size_t pos = 0;
        while ((pos = low.find(q_low, pos)) != std::wstring::npos) {
            float my = base_y;
            if (layout) {
                float px = 0.0f, py = 0.0f;
                DWRITE_HIT_TEST_METRICS htm{};
                if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(pos), FALSE, &px, &py, &htm))) {
                    my = base_y + htm.top;
                }
            }
            matches.push_back({sid, static_cast<uint32_t>(pos), static_cast<uint32_t>(q_low.size()), my});
            pos += q_low.size();
        }
    }
    return matches;
}

bool DocumentLayout::GetHeadingY(int heading_id, float& out_y) const {
    for (const auto& h : headings_) {
        if (h.heading_id == heading_id) {
            out_y = h.y;
            return true;
        }
    }
    return false;
}

bool DocumentLayout::GetAnchorY(const std::string& slug, float& out_y) const {
    auto it = anchor_map_.find(slug);
    if (it != anchor_map_.end()) {
        out_y = it->second;
        return true;
    }
    return false;
}

int DocumentLayout::FindActiveHeadingId(float scroll_y) const {
    if (headings_.empty()) return -1;
    int active = headings_.front().heading_id;
    const float probe = scroll_y + 72.0f;
    for (const auto& h : headings_) {
        if (h.y <= probe) {
            active = h.heading_id;
        } else {
            break;
        }
    }
    return active;
}

ScrollAnchor DocumentLayout::CaptureScrollAnchor(float scroll_y) const {
    ScrollAnchor best;
    if (scroll_y <= 1.0f) return best;

    const float probe_y = scroll_y + 12.0f;
    float best_top = 1e30f;

    auto consider = [&](ScrollAnchor::Kind k, int idx, float top, float bottom) {
        if (bottom < probe_y) return;
        if (top < best_top) {
            best_top = top;
            best.kind = k;
            best.index = idx;
            best.text_pos = 0;
            best.offset_y = scroll_y - top;
        }
    };

    for (size_t i = 0; i < rich_items_.size(); ++i) {
        consider(ScrollAnchor::Kind::RichText, static_cast<int>(i), rich_items_[i].rect.top, rich_items_[i].rect.bottom);
    }
    for (size_t i = 0; i < code_blocks_.size(); ++i) {
        consider(ScrollAnchor::Kind::Code, static_cast<int>(i), code_blocks_[i].card_rect.top, code_blocks_[i].card_rect.bottom);
    }
    for (size_t i = 0; i < math_blocks_.size(); ++i) {
        consider(ScrollAnchor::Kind::Math, static_cast<int>(i), math_blocks_[i].card_rect.top, math_blocks_[i].card_rect.bottom);
    }
    for (size_t i = 0; i < image_items_.size(); ++i) {
        consider(ScrollAnchor::Kind::Image, static_cast<int>(i), image_items_[i].card_rect.top, image_items_[i].card_rect.bottom);
    }

    if (best.kind == ScrollAnchor::Kind::RichText && best.index >= 0 && best.index < static_cast<int>(rich_items_.size())) {
        const auto& rt = rich_items_[best.index];
        if (rt.layout && probe_y >= rt.rect.top) {
            BOOL trailing = FALSE, inside = FALSE;
            DWRITE_HIT_TEST_METRICS htm{};
            rt.layout->HitTestPoint(4.0f, probe_y - rt.rect.top, &trailing, &inside, &htm);
            best.text_pos = htm.textPosition;
            best.offset_y = (scroll_y - rt.rect.top) - htm.top;
        }
    } else if (best.kind == ScrollAnchor::Kind::Code && best.index >= 0 && best.index < static_cast<int>(code_blocks_.size())) {
        const auto& cb = code_blocks_[best.index];
        if (cb.layout && probe_y >= cb.body_rect.top) {
            BOOL trailing = FALSE, inside = FALSE;
            DWRITE_HIT_TEST_METRICS htm{};
            cb.layout->HitTestPoint(4.0f, probe_y - cb.body_rect.top, &trailing, &inside, &htm);
            best.text_pos = htm.textPosition;
            best.offset_y = (scroll_y - cb.body_rect.top) - htm.top;
        }
    }
    return best;
}

float DocumentLayout::ResolveScrollAnchor(const ScrollAnchor& anchor) const {
    switch (anchor.kind) {
        case ScrollAnchor::Kind::RichText:
            if (anchor.index >= 0 && anchor.index < static_cast<int>(rich_items_.size())) {
                const auto& rt = rich_items_[anchor.index];
                if (rt.layout && anchor.text_pos > 0) {
                    float px = 0.0f, py = 0.0f;
                    DWRITE_HIT_TEST_METRICS htm{};
                    uint32_t clamped = std::min<uint32_t>(anchor.text_pos, static_cast<uint32_t>(rt.text.size()));
                    if (SUCCEEDED(rt.layout->HitTestTextPosition(clamped, FALSE, &px, &py, &htm))) {
                        return rt.rect.top + htm.top + anchor.offset_y;
                    }
                }
                return rt.rect.top + anchor.offset_y;
            }
            break;
        case ScrollAnchor::Kind::Code:
            if (anchor.index >= 0 && anchor.index < static_cast<int>(code_blocks_.size())) {
                const auto& cb = code_blocks_[anchor.index];
                if (cb.layout && anchor.text_pos > 0) {
                    float px = 0.0f, py = 0.0f;
                    DWRITE_HIT_TEST_METRICS htm{};
                    uint32_t clamped = std::min<uint32_t>(anchor.text_pos, static_cast<uint32_t>(cb.code.size()));
                    if (SUCCEEDED(cb.layout->HitTestTextPosition(clamped, FALSE, &px, &py, &htm))) {
                        return cb.body_rect.top + htm.top + anchor.offset_y;
                    }
                }
                return cb.card_rect.top + anchor.offset_y;
            }
            break;
        case ScrollAnchor::Kind::Math:
            if (anchor.index >= 0 && anchor.index < static_cast<int>(math_blocks_.size())) {
                return math_blocks_[anchor.index].card_rect.top + anchor.offset_y;
            }
            break;
        case ScrollAnchor::Kind::Image:
            if (anchor.index >= 0 && anchor.index < static_cast<int>(image_items_.size())) {
                return image_items_[anchor.index].card_rect.top + anchor.offset_y;
            }
            break;
        default:
            break;
    }
    return 0.0f;
}

void DocumentLayout::RecordSourceAnchor(int line_start, int line_end, float y_top, float y_bottom) {
    if (line_start < 0) line_start = 0;
    if (line_end < line_start) line_end = line_start;
    if (y_bottom < y_top) y_bottom = y_top;
    line_anchors_.push_back({line_start, line_end, y_top, y_bottom});
}

int DocumentLayout::CaptureSourceLineAtY(float scroll_y) const {
    if (line_anchors_.empty() || scroll_y <= 1.0f) return 0;
    const float probe_y = scroll_y + 20.0f * scale_;
    for (size_t i = 0; i < line_anchors_.size(); ++i) {
        const auto& a = line_anchors_[i];
        if (probe_y <= a.y_bottom) {
            if (probe_y <= a.y_top) {
                if (i > 0) {
                    const auto& prev = line_anchors_[i - 1];
                    float gap = a.y_top - prev.y_bottom;
                    if (gap > 1.0f && a.line_start > prev.line_end) {
                        float t = std::clamp((probe_y - prev.y_bottom) / gap, 0.0f, 1.0f);
                        return prev.line_end + static_cast<int>(std::round(t * static_cast<float>(a.line_start - prev.line_end)));
                    }
                }
                return a.line_start;
            }
            if (a.line_end > a.line_start && (a.y_bottom - a.y_top) > 1.0f) {
                float t = std::clamp((probe_y - a.y_top) / (a.y_bottom - a.y_top), 0.0f, 1.0f);
                return a.line_start + static_cast<int>(std::round(t * static_cast<float>(a.line_end - a.line_start)));
            }
            return a.line_start;
        }
    }
    return line_anchors_.back().line_end;
}

float DocumentLayout::ResolveYForSourceLine(int source_line) const {
    if (line_anchors_.empty() || source_line <= 0) return 0.0f;
    const float top_pad = 20.0f * scale_;
    // Find nearest anchors bracketing source_line (document layout emits non-decreasing block order)
    const SourceLineAnchor* prev_below = nullptr;
    for (const auto& a : line_anchors_) {
        if (source_line >= a.line_start && source_line <= a.line_end) {
            float y = a.y_top;
            if (a.line_end > a.line_start) {
                float t = static_cast<float>(source_line - a.line_start) / static_cast<float>(a.line_end - a.line_start);
                y = a.y_top + t * (a.y_bottom - a.y_top);
            }
            return std::max(0.0f, y - top_pad);
        }
        if (a.line_start > source_line) {
            if (prev_below && a.line_start > prev_below->line_end) {
                float t = static_cast<float>(source_line - prev_below->line_end) /
                          static_cast<float>(a.line_start - prev_below->line_end);
                float y = prev_below->y_bottom + t * (a.y_top - prev_below->y_bottom);
                return std::max(0.0f, y - top_pad);
            }
            return std::max(0.0f, a.y_top - top_pad);
        }
        if (!prev_below || a.line_end >= prev_below->line_end) {
            prev_below = &a;
        }
    }
    return std::max(0.0f, line_anchors_.back().y_bottom - top_pad);
}

bool DocumentLayout::ScrollCodeBlockBy(int code_idx, float dx) {
    if (code_idx < 0 || code_idx >= static_cast<int>(code_blocks_.size())) return false;
    auto& cb = code_blocks_[code_idx];
    if (cb.max_scroll_x <= 1.0f) return false;
    float old = cb.scroll_x;
    cb.scroll_x = std::clamp(cb.scroll_x + dx, 0.0f, cb.max_scroll_x);
    return std::abs(cb.scroll_x - old) > 0.1f;
}

bool DocumentLayout::ScrollMathBlockBy(int math_idx, float dx) {
    if (math_idx < 0 || math_idx >= static_cast<int>(math_blocks_.size())) return false;
    auto& mb = math_blocks_[math_idx];
    if (mb.max_scroll_x <= 1.0f) return false;
    float old = mb.scroll_x;
    mb.scroll_x = std::clamp(mb.scroll_x + dx, 0.0f, mb.max_scroll_x);
    return std::abs(mb.scroll_x - old) > 0.1f;
}

bool DocumentLayout::ScrollTableBy(int table_idx, float dx) {
    if (table_idx < 0 || table_idx >= static_cast<int>(table_items_.size())) return false;
    auto& tbl = table_items_[table_idx];
    if (tbl.max_scroll_x <= 1.0f) return false;
    float old = tbl.scroll_x;
    tbl.scroll_x = std::clamp(tbl.scroll_x + dx, 0.0f, tbl.max_scroll_x);
    return std::abs(tbl.scroll_x - old) > 0.1f;
}

}  // namespace mr
