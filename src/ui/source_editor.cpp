#include "ui/source_editor.h"
#include "render/text_renderer.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>

namespace mr {

namespace {

int CharClass(wchar_t ch) {
    if (std::iswspace(ch)) return 0;
    if (std::iswalnum(ch) || ch == L'_' || ch >= 0x4E00) return 1;
    return 2;
}

bool IsHighSurrogateW(wchar_t ch) {
    return ch >= 0xD800 && ch <= 0xDBFF;
}

bool IsLowSurrogateW(wchar_t ch) {
    return ch >= 0xDC00 && ch <= 0xDFFF;
}

std::vector<std::wstring> SplitLinesKeepEmpty(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') {
            out.push_back(s.substr(start, i - start));
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            start = i + 1;
        } else if (s[i] == L'\n') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

std::vector<DWRITE_HIT_TEST_METRICS> HitTestLineRange(
    IDWriteTextLayout* tl, uint32_t start, uint32_t len, float ox, float oy) {
    std::vector<DWRITE_HIT_TEST_METRICS> out;
    if (!tl || len == 0) return out;
    UINT32 count = 0;
    tl->HitTestTextRange(start, len, ox, oy, nullptr, 0, &count);
    if (count == 0) return out;
    out.resize(count);
    if (SUCCEEDED(tl->HitTestTextRange(start, len, ox, oy, out.data(), count, &count))) {
        out.resize(count);
    } else {
        out.clear();
    }
    return out;
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

}  // namespace

void SourceEditor::SetDocument(const std::string& utf8) {
    use_crlf_ = (utf8.find("\r\n") != std::string::npos) || (utf8.find('\n') == std::string::npos);
    std::wstring wide = Utf8ToWide(utf8);
    auto split = SplitLinesKeepEmpty(wide);

    lines_.clear();
    lines_.reserve(split.size());
    for (auto& ln : split) {
        LineEntry e;
        e.text = std::move(ln);
        e.layout_dirty = true;
        lines_.push_back(std::move(e));
    }
    if (lines_.empty()) {
        lines_.push_back(LineEntry{});
    }

    caret_ = {0, 0};
    sel_anchor_ = {0, 0};
    preferred_x_ = -1.0f;
    dragging_ = false;
    pending_scroll_top_line_ = -1;
    undo_stack_.clear();
    undo_pos_ = 0;
    saved_undo_pos_ = 0;
    saved_reachable_ = true;
    scroll_y_ = 0.0f;
    target_scroll_y_ = 0.0f;
    dirty_fence_from_ = 0;
    layouts_dirty_ = true;
}

std::string SourceEditor::GetUtf8() const {
    const std::string nl = use_crlf_ ? "\r\n" : "\n";
    std::string out;
    out.reserve(lines_.size() * 48);
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) out.append(nl);
        out.append(WideToUtf8(lines_[i].text));
    }
    return out;
}

bool SourceEditor::IsModified() const {
    return !saved_reachable_ || (undo_pos_ != saved_undo_pos_);
}

void SourceEditor::MarkSaved() {
    saved_undo_pos_ = undo_pos_;
    saved_reachable_ = true;
}

void SourceEditor::InvalidateAllLayouts() {
    for (auto& le : lines_) {
        le.layout.Reset();
        le.layout_dirty = true;
    }
    layouts_dirty_ = true;
}

void SourceEditor::EnsureFormats(IDWriteFactory5* dw, float font_size, float num_size) {
    if (!dw) return;
    if (!text_fmt_) {
        if (SUCCEEDED(dw->CreateTextFormat(
                PickMonoFontFamily(dw), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                font_size, L"zh-cn", text_fmt_.Put())) && text_fmt_) {
            text_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            text_fmt_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.48f, 1.15f);
        }
    }
    if (!num_fmt_) {
        if (SUCCEEDED(dw->CreateTextFormat(
                PickMonoFontFamily(dw), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                num_size, L"", num_fmt_.Put())) && num_fmt_) {
            num_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            num_fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            num_fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }
}

void SourceEditor::RefreshTokenBrushes(const ThemePalette& theme) {
    if (has_cached_theme_ && cached_dark_ == theme.is_dark && token_effects_[0]) return;
    cached_dark_ = theme.is_dark;
    has_cached_theme_ = true;
    for (int k = 0; k < 9; ++k) {
        TokenKind tk = static_cast<TokenKind>(k);
        D2D1_COLOR_F col = (tk == TokenKind::Normal) ? theme.text_primary : SyntaxTokenColor(theme, tk);
        token_effects_[k] = ComPtr<IUnknown>(static_cast<IUnknown*>(new SpanColorEffect(col)));
    }
}

void SourceEditor::RecomputeFencesFrom(int start_line) {
    if (lines_.empty()) return;
    int idx = std::clamp(start_line, 0, static_cast<int>(lines_.size()) - 1);
    MarkdownFenceState state = (idx > 0) ? lines_[idx - 1].fence_out : MarkdownFenceState{};

    for (int i = idx; i < static_cast<int>(lines_.size()); ++i) {
        LineEntry& le = lines_[i];
        bool in_changed =
            (le.fence_in.in_code_fence != state.in_code_fence ||
             le.fence_in.in_math_fence != state.in_math_fence ||
             le.fence_in.fence_char != state.fence_char ||
             le.fence_in.fence_len != state.fence_len ||
             le.fence_in.fence_lang != state.fence_lang);
        le.fence_in = state;
        HighlightMarkdownLine(le.text, state);
        bool out_changed =
            (le.fence_out.in_code_fence != state.in_code_fence ||
             le.fence_out.in_math_fence != state.in_math_fence ||
             le.fence_out.fence_char != state.fence_char ||
             le.fence_out.fence_len != state.fence_len ||
             le.fence_out.fence_lang != state.fence_lang);
        le.fence_out = state;
        if (in_changed) {
            le.layout_dirty = true;
            layouts_dirty_ = true;
        }
        if (i > idx && !in_changed && !out_changed) {
            break;
        }
    }
    dirty_fence_from_ = static_cast<int>(lines_.size());
}

void SourceEditor::BuildLineLayout(int idx, IDWriteFactory5* dw, float wrap_w, const ThemePalette& theme) {
    LineEntry& le = lines_[idx];
    le.layout.Reset();
    le.layout_dirty = false;
    if (!dw || !text_fmt_) {
        le.height = line_h_;
        return;
    }

    if (SUCCEEDED(dw->CreateTextLayout(
            le.text.c_str(), static_cast<UINT32>(le.text.size()),
            text_fmt_.Get(), std::max(80.0f, wrap_w), 100000.0f, le.layout.Put())) && le.layout) {
        uint32_t text_len = static_cast<uint32_t>(le.text.size());
        if (text_len > 0) {
            if (token_effects_[0]) {
                le.layout->SetDrawingEffect(token_effects_[0].Get(), {0, text_len});
            }
            MarkdownFenceState st = le.fence_in;
            auto spans = HighlightMarkdownLine(le.text, st);
            for (const auto& sp : spans) {
                if (sp.start + sp.length > text_len) continue;
                DWRITE_TEXT_RANGE r{sp.start, sp.length};
                size_t k = static_cast<size_t>(sp.kind);
                if (k < token_effects_.size() && token_effects_[k]) {
                    le.layout->SetDrawingEffect(token_effects_[k].Get(), r);
                }
                if (sp.bold) {
                    le.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
                }
                if (sp.italic) {
                    le.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                }
            }
        }
        DWRITE_TEXT_METRICS tm{};
        le.layout->GetMetrics(&tm);
        le.height = std::max(line_h_, tm.height);
    } else {
        le.height = line_h_;
    }
    (void)theme;
}

void SourceEditor::RecomputeMetrics(float vp_h) {
    float y = top_pad_;
    for (auto& le : lines_) {
        le.top = y;
        y += le.height;
    }
    total_height_ = y + std::max(64.0f * scale_, vp_h * 0.25f);
    max_scroll_y_ = std::max(0.0f, total_height_ - vp_h);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_y_);
    target_scroll_y_ = std::clamp(target_scroll_y_, 0.0f, max_scroll_y_);
    UpdateScrollbarRects();
}

void SourceEditor::UpdateScrollbarRects() {
    float vp_w = std::max(1.0f, viewport_.right - viewport_.left);
    float vp_h = std::max(1.0f, viewport_.bottom - viewport_.top);
    (void)vp_w;
    if (max_scroll_y_ <= 1.0f || total_height_ <= vp_h) {
        vscroll_track_ = {};
        vscroll_thumb_ = {};
        return;
    }
    float track_top = viewport_.top + 6.0f * scale_;
    float track_bot = viewport_.bottom - 6.0f * scale_;
    vscroll_track_ = D2D1::RectF(viewport_.right - 10.0f * scale_, track_top, viewport_.right - 4.0f * scale_, track_bot);
    float track_h = std::max(24.0f, track_bot - track_top);
    float thumb_h = std::clamp(track_h * (vp_h / total_height_), 28.0f * scale_, track_h);
    float ratio = scroll_y_ / max_scroll_y_;
    float ty = track_top + ratio * (track_h - thumb_h);
    vscroll_thumb_ = D2D1::RectF(vscroll_track_.left, ty, vscroll_track_.right, ty + thumb_h);
}

void SourceEditor::EnsureLayout(
    const D2D1_RECT_F& viewport,
    int zoom_percent,
    IDWriteFactory5* dw,
    const ThemePalette& theme) {
    viewport_ = viewport;
    scale_ = static_cast<float>(std::clamp(zoom_percent, 50, 250)) / 100.0f;
    font_size_ = 14.5f * scale_;
    float num_size = 12.0f * scale_;
    line_h_ = font_size_ * 1.48f;
    top_pad_ = 16.0f * scale_;

    int digits = std::max(3, static_cast<int>(std::to_wstring(std::max<size_t>(1, lines_.size())).size()));
    gutter_w_ = 26.0f * scale_ + static_cast<float>(digits) * (num_size * 0.64f);
    text_left_ = viewport.left + gutter_w_ + 16.0f * scale_;
    float right_margin = 24.0f * scale_;
    text_wrap_w_ = std::max(160.0f, (viewport.right - right_margin) - text_left_);

    bool theme_changed = (!has_cached_theme_ || cached_dark_ != theme.is_dark);
    RefreshTokenBrushes(theme);

    if (cached_zoom_ != zoom_percent) {
        cached_zoom_ = zoom_percent;
        text_fmt_.Reset();
        num_fmt_.Reset();
        InvalidateAllLayouts();
    } else if (std::fabs(cached_wrap_w_ - text_wrap_w_) > 0.5f || theme_changed) {
        cached_wrap_w_ = text_wrap_w_;
        InvalidateAllLayouts();
    }
    cached_wrap_w_ = text_wrap_w_;

    EnsureFormats(dw, font_size_, num_size);

    if (dirty_fence_from_ < static_cast<int>(lines_.size())) {
        RecomputeFencesFrom(dirty_fence_from_);
    }

    if (layouts_dirty_) {
        for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
            if (lines_[i].layout_dirty || !lines_[i].layout) {
                BuildLineLayout(i, dw, text_wrap_w_, theme);
            }
        }
        layouts_dirty_ = false;
    }

    RecomputeMetrics(viewport.bottom - viewport.top);

    if (pending_scroll_top_line_ >= 0 && !lines_.empty()) {
        int clamped = std::clamp(pending_scroll_top_line_, 0, static_cast<int>(lines_.size()) - 1);
        float y = std::clamp(lines_[clamped].top - top_pad_, 0.0f, max_scroll_y_);
        scroll_y_ = y;
        target_scroll_y_ = y;
        pending_scroll_top_line_ = -1;
        UpdateScrollbarRects();
    }
}

void SourceEditor::Render(
    ID2D1DeviceContext5* dc,
    const D2D1_RECT_F& viewport,
    const ThemePalette& theme,
    bool editor_focused,
    bool caret_visible,
    const std::wstring& ime_comp,
    const std::vector<EditorFindMatch>& find_matches,
    int active_find_idx) {
    if (!dc) return;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(theme.text_primary, brush.Put())) || !brush) return;

    dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);

    // Subtle gutter strip & separator divider
    D2D1_RECT_F gutter_bg = D2D1::RectF(viewport.left, viewport.top, viewport.left + gutter_w_, viewport.bottom);
    brush->SetColor(theme.is_dark ? WithAlpha(theme.bg_sidebar, 0.55f) : WithAlpha(theme.bg_sidebar, 0.65f));
    dc->FillRectangle(gutter_bg, brush.Get());

    brush->SetColor(WithAlpha(theme.border, 0.75f));
    float div_x = viewport.left + gutter_w_;
    dc->DrawLine(D2D1::Point2F(div_x, viewport.top), D2D1::Point2F(div_x, viewport.bottom), brush.Get(), 1.0f);

    ComPtr<D2DTextRenderer> text_renderer(new D2DTextRenderer(dc, theme.text_primary));
    SourcePos sel_s = NormalizedStart();
    SourcePos sel_e = NormalizedEnd();
    const bool has_sel = HasSelection();

    const float view_top_doc = scroll_y_;
    const float view_bot_doc = scroll_y_ + (viewport.bottom - viewport.top);

    auto it = std::lower_bound(
        lines_.begin(), lines_.end(), view_top_doc,
        [](const LineEntry& le, float v) { return (le.top + le.height) < v; });
    int first_vis = static_cast<int>(it - lines_.begin());

    for (int i = first_vis; i < static_cast<int>(lines_.size()); ++i) {
        const auto& le = lines_[i];
        if (le.top > view_bot_doc) break;

        float screen_y = viewport.top + (le.top - scroll_y_);

        // Active cursor line tint
        if (i == caret_.line && !has_sel) {
            D2D1_RECT_F cur_bg = D2D1::RectF(
                div_x + 1.0f, screen_y,
                viewport.right, screen_y + le.height);
            brush->SetColor(WithAlpha(theme.accent, theme.is_dark ? 0.085f : 0.055f));
            dc->FillRectangle(cur_bg, brush.Get());
        }

        // Line number
        if (num_fmt_) {
            wchar_t nbuf[24]{};
            swprintf_s(nbuf, L"%d", i + 1);
            brush->SetColor((i == caret_.line) ? theme.text_primary : theme.text_muted);
            D2D1_RECT_F nrect = D2D1::RectF(
                viewport.left + 4.0f * scale_,
                screen_y + 1.5f * scale_,
                div_x - 10.0f * scale_,
                screen_y + le.height);
            dc->DrawText(nbuf, static_cast<UINT32>(wcslen(nbuf)), num_fmt_.Get(), nrect, brush.Get());
        }

        // Search highlights on this line
        if (!find_matches.empty() && le.layout) {
            for (size_t mi = 0; mi < find_matches.size(); ++mi) {
                const auto& m = find_matches[mi];
                if (m.line != i) continue;
                brush->SetColor((static_cast<int>(mi) == active_find_idx) ? theme.search_active_bg : theme.search_match_bg);
                for (const auto& hb : HitTestLineRange(le.layout.Get(), m.start, m.length, text_left_, screen_y)) {
                    dc->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(hb.left, hb.top, hb.left + hb.width, hb.top + hb.height), 2.5f, 2.5f),
                        brush.Get());
                }
            }
        }

        // Selection background on this line
        if (has_sel && i >= sel_s.line && i <= sel_e.line) {
            uint32_t line_len = static_cast<uint32_t>(le.text.size());
            uint32_t s0 = (i == sel_s.line) ? std::min(sel_s.col, line_len) : 0;
            uint32_t s1 = (i == sel_e.line) ? std::min(sel_e.col, line_len) : line_len;
            brush->SetColor(theme.selection_bg);
            if (s1 > s0 && le.layout) {
                for (const auto& hb : HitTestLineRange(le.layout.Get(), s0, s1 - s0, text_left_, screen_y)) {
                    dc->FillRectangle(D2D1::RectF(hb.left, hb.top, hb.left + hb.width, hb.top + hb.height), brush.Get());
                }
            }
            if (i < sel_e.line) {
                // Highlight newline glyph space at end of wrapped line when selection spans next line
                float ex = text_left_, ey = screen_y;
                float eh = line_h_;
                if (le.layout) {
                    DWRITE_HIT_TEST_METRICS htm{};
                    float px = 0.0f, py = 0.0f;
                    if (SUCCEEDED(le.layout->HitTestTextPosition(line_len, FALSE, &px, &py, &htm))) {
                        ex = text_left_ + px;
                        ey = screen_y + py;
                        eh = std::max(line_h_, htm.height);
                    }
                }
                dc->FillRectangle(D2D1::RectF(ex, ey, ex + font_size_ * 0.45f, ey + eh), brush.Get());
            }
        }

        // Line text
        if (le.layout && !le.text.empty()) {
            le.layout->Draw(dc, text_renderer.Get(), text_left_, screen_y);
        }

        // Caret & IME composition preedit
        if (i == caret_.line && editor_focused) {
            float cx = text_left_;
            float cy = screen_y;
            float ch = line_h_;
            if (le.layout) {
                float px = 0.0f, py = 0.0f;
                DWRITE_HIT_TEST_METRICS htm{};
                uint32_t clamped = std::min(caret_.col, static_cast<uint32_t>(le.text.size()));
                if (SUCCEEDED(le.layout->HitTestTextPosition(clamped, FALSE, &px, &py, &htm))) {
                    cx = text_left_ + px;
                    cy = screen_y + py;
                    ch = std::max(line_h_ * 0.88f, htm.height);
                }
            }

            if (!ime_comp.empty() && text_fmt_) {
                float est_w = std::max(28.0f, static_cast<float>(ime_comp.size()) * font_size_ * 0.85f + 12.0f * scale_);
                D2D1_RECT_F comp_bg = D2D1::RectF(cx, cy, cx + est_w, cy + ch);
                brush->SetColor(theme.bg_elevated);
                dc->FillRoundedRectangle(D2D1::RoundedRect(comp_bg, 3.0f, 3.0f), brush.Get());
                brush->SetColor(theme.text_primary);
                dc->DrawText(
                    ime_comp.c_str(), static_cast<UINT32>(ime_comp.size()),
                    text_fmt_.Get(),
                    D2D1::RectF(cx + 4.0f * scale_, cy, cx + est_w + 120.0f, cy + ch),
                    brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                brush->SetColor(theme.accent);
                dc->DrawLine(D2D1::Point2F(cx, cy + ch - 1.5f), D2D1::Point2F(cx + est_w, cy + ch - 1.5f), brush.Get(), 2.0f);
            } else if (caret_visible) {
                brush->SetColor(theme.accent);
                dc->DrawLine(
                    D2D1::Point2F(cx, cy + 2.0f * scale_),
                    D2D1::Point2F(cx, cy + ch - 2.0f * scale_),
                    brush.Get(), 1.8f);
            }
        }
    }

    dc->PopAxisAlignedClip();
}

void SourceEditor::SetScrollY(float y) {
    float clamped = std::clamp(y, 0.0f, max_scroll_y_);
    scroll_y_ = clamped;
    target_scroll_y_ = clamped;
    pending_scroll_top_line_ = -1;
    UpdateScrollbarRects();
}

void SourceEditor::ScrollBy(float dy) {
    target_scroll_y_ = std::clamp(target_scroll_y_ + dy, 0.0f, max_scroll_y_);
    pending_scroll_top_line_ = -1;
}

bool SourceEditor::AnimateScroll() {
    float diff = target_scroll_y_ - scroll_y_;
    if (std::fabs(diff) < 0.4f) {
        if (scroll_y_ != target_scroll_y_) {
            scroll_y_ = target_scroll_y_;
            UpdateScrollbarRects();
            return true;
        }
        return false;
    }
    scroll_y_ += diff * 0.34f;
    UpdateScrollbarRects();
    return true;
}

int SourceEditor::GetTopVisibleLine() const {
    if (lines_.empty()) return 0;
    if (pending_scroll_top_line_ >= 0) {
        return std::clamp(pending_scroll_top_line_, 0, static_cast<int>(lines_.size()) - 1);
    }
    float probe = scroll_y_ + top_pad_ * 0.5f;
    auto it = std::lower_bound(
        lines_.begin(), lines_.end(), probe,
        [](const LineEntry& le, float v) { return (le.top + le.height) <= v; });
    if (it == lines_.end()) return static_cast<int>(lines_.size()) - 1;
    return static_cast<int>(it - lines_.begin());
}

void SourceEditor::ScrollToLineAtTop(int line) {
    if (lines_.empty()) return;
    int clamped = std::clamp(line, 0, static_cast<int>(lines_.size()) - 1);
    pending_scroll_top_line_ = clamped;
    caret_ = {clamped, 0};
    sel_anchor_ = caret_;
    if (!layouts_dirty_) {
        float y = std::clamp(lines_[clamped].top - top_pad_, 0.0f, max_scroll_y_);
        scroll_y_ = y;
        target_scroll_y_ = y;
        pending_scroll_top_line_ = -1;
        UpdateScrollbarRects();
    }
}

void SourceEditor::SetCaretToLine(int line) {
    if (lines_.empty()) return;
    int clamped = std::clamp(line, 0, static_cast<int>(lines_.size()) - 1);
    caret_ = {clamped, 0};
    sel_anchor_ = caret_;
    ScrollToLineAtTop(clamped);
}

void SourceEditor::EnsureCaretVisible() {
    if (lines_.empty() || layouts_dirty_) return;
    int li = std::clamp(caret_.line, 0, static_cast<int>(lines_.size()) - 1);
    const auto& le = lines_[li];
    float caret_top = le.top;
    float caret_bot = le.top + line_h_;
    if (le.layout) {
        float px = 0.0f, py = 0.0f;
        DWRITE_HIT_TEST_METRICS htm{};
        uint32_t c = std::min(caret_.col, static_cast<uint32_t>(le.text.size()));
        if (SUCCEEDED(le.layout->HitTestTextPosition(c, FALSE, &px, &py, &htm))) {
            caret_top = le.top + py;
            caret_bot = caret_top + std::max(line_h_, htm.height);
        }
    }
    float vp_h = std::max(1.0f, viewport_.bottom - viewport_.top);
    float margin = 24.0f * scale_;
    if (caret_top < scroll_y_ + margin) {
        float y = std::clamp(caret_top - margin, 0.0f, max_scroll_y_);
        scroll_y_ = y;
        target_scroll_y_ = y;
    } else if (caret_bot > scroll_y_ + vp_h - margin) {
        float y = std::clamp(caret_bot - vp_h + margin, 0.0f, max_scroll_y_);
        scroll_y_ = y;
        target_scroll_y_ = y;
    }
    UpdateScrollbarRects();
}

std::wstring SourceEditor::GetSelectedText() const {
    if (!HasSelection() || lines_.empty()) return {};
    SourcePos s = NormalizedStart();
    SourcePos e = NormalizedEnd();
    if (s.line == e.line) {
        return lines_[s.line].text.substr(s.col, e.col - s.col);
    }
    std::wstring out = lines_[s.line].text.substr(s.col);
    for (int i = s.line + 1; i < e.line; ++i) {
        out.append(use_crlf_ ? L"\r\n" : L"\n");
        out.append(lines_[i].text);
    }
    out.append(use_crlf_ ? L"\r\n" : L"\n");
    out.append(lines_[e.line].text.substr(0, e.col));
    return out;
}

void SourceEditor::SelectAll() {
    if (lines_.empty()) return;
    sel_anchor_ = {0, 0};
    int last = static_cast<int>(lines_.size()) - 1;
    caret_ = {last, static_cast<uint32_t>(lines_[last].text.size())};
}

void SourceEditor::ApplyEdit(
    SourcePos from, SourcePos to, const std::wstring& replacement, int merge_kind, bool select_after) {
    if (to < from) std::swap(from, to);
    from.line = std::clamp(from.line, 0, static_cast<int>(lines_.size()) - 1);
    to.line = std::clamp(to.line, 0, static_cast<int>(lines_.size()) - 1);
    from.col = std::min(from.col, static_cast<uint32_t>(lines_[from.line].text.size()));
    to.col = std::min(to.col, static_cast<uint32_t>(lines_[to.line].text.size()));

    SourcePos anchor_before = sel_anchor_;
    SourcePos caret_before = caret_;

    std::vector<std::wstring> old_slice;
    old_slice.reserve(static_cast<size_t>(to.line - from.line + 1));
    for (int i = from.line; i <= to.line; ++i) {
        old_slice.push_back(lines_[i].text);
    }

    std::wstring prefix = lines_[from.line].text.substr(0, from.col);
    std::wstring suffix = lines_[to.line].text.substr(to.col);
    auto rep_parts = SplitLinesKeepEmpty(replacement);

    SourcePos end_pos;
    std::vector<std::wstring> new_slice;
    if (rep_parts.size() == 1) {
        new_slice.push_back(prefix + rep_parts[0] + suffix);
        end_pos = {from.line, static_cast<uint32_t>(prefix.size() + rep_parts[0].size())};
    } else {
        new_slice.reserve(rep_parts.size());
        new_slice.push_back(prefix + rep_parts.front());
        for (size_t k = 1; k + 1 < rep_parts.size(); ++k) {
            new_slice.push_back(rep_parts[k]);
        }
        new_slice.push_back(rep_parts.back() + suffix);
        end_pos = {from.line + static_cast<int>(rep_parts.size()) - 1,
                   static_cast<uint32_t>(rep_parts.back().size())};
    }

    // Splice lines_ (copy txt so new_slice remains intact for the undo entry)
    lines_.erase(lines_.begin() + from.line, lines_.begin() + to.line + 1);
    std::vector<LineEntry> inserted;
    inserted.reserve(new_slice.size());
    for (const auto& txt : new_slice) {
        LineEntry le;
        le.text = txt;
        le.layout_dirty = true;
        inserted.push_back(std::move(le));
    }
    lines_.insert(lines_.begin() + from.line, std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));

    caret_ = end_pos;
    sel_anchor_ = select_after ? from : end_pos;
    preferred_x_ = -1.0f;
    dirty_fence_from_ = std::min(dirty_fence_from_, from.line);
    layouts_dirty_ = true;

    // Truncate redo tail & record undo patch
    if (undo_pos_ < saved_undo_pos_) {
        saved_reachable_ = false;
    }
    if (undo_pos_ < undo_stack_.size()) {
        undo_stack_.resize(undo_pos_);
    }

    uint64_t now = GetTickCount64();
    if (merge_kind != 0 && !undo_stack_.empty() &&
        undo_stack_.back().merge_kind == merge_kind &&
        (now - undo_stack_.back().timestamp_ms) < 650 &&
        undo_stack_.back().new_lines.size() == 1 && new_slice.size() == 1 &&
        undo_stack_.back().start_line == from.line &&
        (merge_kind == 1 ? (from.col == undo_stack_.back().caret_after.col && to.col == from.col)
                         : (to.col == undo_stack_.back().caret_after.col))) {
        undo_stack_.back().new_lines = std::move(new_slice);
        undo_stack_.back().caret_after = caret_;
        undo_stack_.back().anchor_after = sel_anchor_;
        undo_stack_.back().timestamp_ms = now;
        return;
    }

    UndoEntry entry;
    entry.start_line = from.line;
    entry.old_lines = std::move(old_slice);
    entry.new_lines = std::move(new_slice);
    entry.anchor_before = anchor_before;
    entry.caret_before = caret_before;
    entry.anchor_after = sel_anchor_;
    entry.caret_after = caret_;
    entry.timestamp_ms = now;
    entry.merge_kind = merge_kind;
    undo_stack_.push_back(std::move(entry));
    if (undo_stack_.size() > 256) {
        undo_stack_.erase(undo_stack_.begin());
        if (saved_undo_pos_ > 0) --saved_undo_pos_;
        else saved_reachable_ = false;
    }
    undo_pos_ = undo_stack_.size();
}

void SourceEditor::InsertText(const std::wstring& text, int merge_kind) {
    if (lines_.empty()) return;
    SourcePos s = NormalizedStart();
    SourcePos e = NormalizedEnd();
    ApplyEdit(s, e, text, HasSelection() ? 0 : merge_kind);
}

void SourceEditor::DeleteSelection() {
    if (!HasSelection() || lines_.empty()) return;
    ApplyEdit(NormalizedStart(), NormalizedEnd(), L"", 0);
}

void SourceEditor::PasteText(const std::wstring& text) {
    if (text.empty()) return;
    InsertText(text, 0);
}

bool SourceEditor::Undo() {
    if (undo_pos_ == 0 || lines_.empty()) return false;
    const UndoEntry& e = undo_stack_[undo_pos_ - 1];
    int del_end = std::min(static_cast<int>(lines_.size()), e.start_line + static_cast<int>(e.new_lines.size()));
    lines_.erase(lines_.begin() + e.start_line, lines_.begin() + del_end);
    std::vector<LineEntry> restored;
    restored.reserve(e.old_lines.size());
    for (const auto& t : e.old_lines) {
        LineEntry le;
        le.text = t;
        le.layout_dirty = true;
        restored.push_back(std::move(le));
    }
    lines_.insert(lines_.begin() + e.start_line, std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
    if (lines_.empty()) lines_.push_back(LineEntry{});
    sel_anchor_ = e.anchor_before;
    caret_ = e.caret_before;
    preferred_x_ = -1.0f;
    dirty_fence_from_ = std::min(dirty_fence_from_, e.start_line);
    layouts_dirty_ = true;
    --undo_pos_;
    return true;
}

bool SourceEditor::Redo() {
    if (undo_pos_ >= undo_stack_.size() || lines_.empty()) return false;
    const UndoEntry& e = undo_stack_[undo_pos_];
    int del_end = std::min(static_cast<int>(lines_.size()), e.start_line + static_cast<int>(e.old_lines.size()));
    lines_.erase(lines_.begin() + e.start_line, lines_.begin() + del_end);
    std::vector<LineEntry> restored;
    restored.reserve(e.new_lines.size());
    for (const auto& t : e.new_lines) {
        LineEntry le;
        le.text = t;
        le.layout_dirty = true;
        restored.push_back(std::move(le));
    }
    lines_.insert(lines_.begin() + e.start_line, std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
    if (lines_.empty()) lines_.push_back(LineEntry{});
    sel_anchor_ = e.anchor_after;
    caret_ = e.caret_after;
    preferred_x_ = -1.0f;
    dirty_fence_from_ = std::min(dirty_fence_from_, e.start_line);
    layouts_dirty_ = true;
    ++undo_pos_;
    return true;
}

std::vector<EditorFindMatch> SourceEditor::FindAllMatches(const std::wstring& query) const {
    std::vector<EditorFindMatch> out;
    if (query.empty()) return out;
    std::wstring q_low;
    q_low.reserve(query.size());
    for (wchar_t ch : query) q_low.push_back(static_cast<wchar_t>(std::towlower(ch)));

    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        const std::wstring& txt = lines_[i].text;
        if (txt.size() < q_low.size()) continue;
        std::wstring t_low;
        t_low.reserve(txt.size());
        for (wchar_t ch : txt) t_low.push_back(static_cast<wchar_t>(std::towlower(ch)));

        size_t pos = 0;
        while ((pos = t_low.find(q_low, pos)) != std::string::npos) {
            out.push_back({i, static_cast<uint32_t>(pos), static_cast<uint32_t>(q_low.size())});
            pos += q_low.size();
        }
    }
    return out;
}

void SourceEditor::ScrollMatchIntoView(const EditorFindMatch& m) {
    if (m.line < 0 || m.line >= static_cast<int>(lines_.size())) return;
    sel_anchor_ = {m.line, m.start};
    caret_ = {m.line, std::min(static_cast<uint32_t>(lines_[m.line].text.size()), m.start + m.length)};
    EnsureCaretVisible();
}

SourcePos SourceEditor::HitTestWindowPoint(float win_x, float win_y) const {
    if (lines_.empty()) return {0, 0};
    float doc_y = (win_y - viewport_.top) + scroll_y_;
    int line_idx = 0;
    if (doc_y >= lines_.front().top) {
        auto it = std::lower_bound(
            lines_.begin(), lines_.end(), doc_y,
            [](const LineEntry& le, float v) { return (le.top + le.height) < v; });
        if (it == lines_.end()) line_idx = static_cast<int>(lines_.size()) - 1;
        else line_idx = static_cast<int>(it - lines_.begin());
    }
    const auto& le = lines_[line_idx];
    uint32_t col = 0;
    if (le.layout && !le.text.empty()) {
        float lx = std::max(0.0f, win_x - text_left_);
        float ly = std::clamp(doc_y - le.top, 0.0f, std::max(0.0f, le.height - 1.0f));
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS htm{};
        if (SUCCEEDED(le.layout->HitTestPoint(lx, ly, &trailing, &inside, &htm))) {
            col = htm.textPosition + (trailing ? htm.length : 0);
        }
    }
    col = std::min(col, static_cast<uint32_t>(le.text.size()));
    return {line_idx, col};
}

bool SourceEditor::OnLButtonDown(float win_x, float win_y, bool shift_down) {
    if (win_x < viewport_.left || win_x > viewport_.right || win_y < viewport_.top || win_y > viewport_.bottom) {
        return false;
    }
    // Click in gutter selects the entire line cleanly
    if (win_x <= viewport_.left + gutter_w_ && !lines_.empty()) {
        float doc_y = (win_y - viewport_.top) + scroll_y_;
        auto it = std::lower_bound(
            lines_.begin(), lines_.end(), doc_y,
            [](const LineEntry& le, float v) { return (le.top + le.height) < v; });
        int li = (it == lines_.end()) ? static_cast<int>(lines_.size()) - 1 : static_cast<int>(it - lines_.begin());
        sel_anchor_ = {li, 0};
        if (li + 1 < static_cast<int>(lines_.size())) caret_ = {li + 1, 0};
        else caret_ = {li, static_cast<uint32_t>(lines_[li].text.size())};
        dragging_ = true;
        preferred_x_ = -1.0f;
        return true;
    }

    SourcePos p = HitTestWindowPoint(win_x, win_y);
    caret_ = p;
    if (!shift_down) sel_anchor_ = p;
    dragging_ = true;
    preferred_x_ = -1.0f;
    return true;
}

bool SourceEditor::OnLButtonDblClick(float win_x, float win_y) {
    if (win_x < viewport_.left + gutter_w_ || win_x > viewport_.right ||
        win_y < viewport_.top || win_y > viewport_.bottom || lines_.empty()) {
        return false;
    }
    SourcePos p = HitTestWindowPoint(win_x, win_y);
    const std::wstring& txt = lines_[p.line].text;
    if (txt.empty()) {
        caret_ = p;
        sel_anchor_ = p;
        return true;
    }
    uint32_t idx = std::min(p.col, static_cast<uint32_t>(txt.size() - 1));
    int cls = CharClass(txt[idx]);
    uint32_t s = idx;
    uint32_t e = idx + 1;
    while (s > 0 && CharClass(txt[s - 1]) == cls) --s;
    while (e < txt.size() && CharClass(txt[e]) == cls) ++e;
    sel_anchor_ = {p.line, s};
    caret_ = {p.line, e};
    dragging_ = false;
    preferred_x_ = -1.0f;
    return true;
}

bool SourceEditor::OnMouseMove(float win_x, float win_y) {
    if (!dragging_) return false;
    caret_ = HitTestWindowPoint(win_x, win_y);
    preferred_x_ = -1.0f;
    return true;
}

void SourceEditor::OnLButtonUp() {
    dragging_ = false;
}

void SourceEditor::MoveCaretHorizontal(int dir, bool word_step, bool keep_sel) {
    if (lines_.empty()) return;
    if (!keep_sel && HasSelection() && !word_step) {
        caret_ = (dir < 0) ? NormalizedStart() : NormalizedEnd();
        sel_anchor_ = caret_;
        preferred_x_ = -1.0f;
        return;
    }

    auto step_left = [&]() {
        if (caret_.col > 0) {
            --caret_.col;
            if (caret_.col > 0 &&
                IsLowSurrogateW(lines_[caret_.line].text[caret_.col]) &&
                IsHighSurrogateW(lines_[caret_.line].text[caret_.col - 1])) {
                --caret_.col;
            }
        } else if (caret_.line > 0) {
            --caret_.line;
            caret_.col = static_cast<uint32_t>(lines_[caret_.line].text.size());
        }
    };
    auto step_right = [&]() {
        uint32_t len = static_cast<uint32_t>(lines_[caret_.line].text.size());
        if (caret_.col < len) {
            if (IsHighSurrogateW(lines_[caret_.line].text[caret_.col]) && caret_.col + 1 < len &&
                IsLowSurrogateW(lines_[caret_.line].text[caret_.col + 1])) {
                caret_.col += 2;
            } else {
                ++caret_.col;
            }
        } else if (caret_.line + 1 < static_cast<int>(lines_.size())) {
            ++caret_.line;
            caret_.col = 0;
        }
    };

    if (!word_step) {
        if (dir < 0) step_left();
        else step_right();
    } else {
        if (dir < 0) {
            step_left();
            const std::wstring& t = lines_[caret_.line].text;
            while (caret_.col > 0 && std::iswspace(t[caret_.col])) --caret_.col;
            if (caret_.col > 0) {
                int cls = CharClass(t[caret_.col]);
                while (caret_.col > 0 && CharClass(t[caret_.col - 1]) == cls) --caret_.col;
            }
        } else {
            const std::wstring& t = lines_[caret_.line].text;
            if (caret_.col >= t.size()) {
                step_right();
            } else {
                int cls = CharClass(t[caret_.col]);
                while (caret_.col < t.size() && CharClass(t[caret_.col]) == cls) ++caret_.col;
                while (caret_.col < t.size() && std::iswspace(t[caret_.col])) ++caret_.col;
            }
        }
    }

    if (!keep_sel) sel_anchor_ = caret_;
    preferred_x_ = -1.0f;
}

void SourceEditor::MoveCaretVertical(int delta_lines, bool keep_sel) {
    if (lines_.empty()) return;
    if (preferred_x_ < 0.0f) {
        preferred_x_ = 0.0f;
        const auto& le = lines_[caret_.line];
        if (le.layout) {
            float px = 0.0f, py = 0.0f;
            DWRITE_HIT_TEST_METRICS htm{};
            uint32_t c = std::min(caret_.col, static_cast<uint32_t>(le.text.size()));
            if (SUCCEEDED(le.layout->HitTestTextPosition(c, FALSE, &px, &py, &htm))) {
                preferred_x_ = px;
            }
        }
    }

    int target_line = std::clamp(caret_.line + delta_lines, 0, static_cast<int>(lines_.size()) - 1);
    const auto& dst = lines_[target_line];
    uint32_t col = 0;
    if (dst.layout && !dst.text.empty()) {
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS htm{};
        if (SUCCEEDED(dst.layout->HitTestPoint(preferred_x_, 4.0f, &trailing, &inside, &htm))) {
            col = htm.textPosition + (trailing ? htm.length : 0);
        }
    }
    col = std::min(col, static_cast<uint32_t>(dst.text.size()));
    caret_ = {target_line, col};
    if (!keep_sel) sel_anchor_ = caret_;
}

void SourceEditor::MoveCaretLineBoundary(bool to_end, bool keep_sel) {
    if (lines_.empty()) return;
    const std::wstring& txt = lines_[caret_.line].text;
    if (to_end) {
        caret_.col = static_cast<uint32_t>(txt.size());
    } else {
        uint32_t indent = 0;
        while (indent < txt.size() && (txt[indent] == L' ' || txt[indent] == L'\t')) ++indent;
        caret_.col = (caret_.col == indent || indent == txt.size()) ? 0 : indent;
    }
    if (!keep_sel) sel_anchor_ = caret_;
    preferred_x_ = -1.0f;
}

void SourceEditor::IndentSelectedLines(bool unindent) {
    SourcePos s = NormalizedStart();
    SourcePos e = NormalizedEnd();
    int end_line = (e.line > s.line && e.col == 0) ? (e.line - 1) : e.line;
    std::wstring block;
    for (int i = s.line; i <= end_line; ++i) {
        if (i > s.line) block.push_back(L'\n');
        std::wstring line = lines_[i].text;
        if (!unindent) {
            block.append(L"    ");
            block.append(line);
        } else {
            size_t drop = 0;
            if (!line.empty() && line[0] == L'\t') drop = 1;
            else {
                while (drop < 4 && drop < line.size() && line[drop] == L' ') ++drop;
            }
            block.append(line.substr(drop));
        }
    }
    ApplyEdit({s.line, 0}, {end_line, static_cast<uint32_t>(lines_[end_line].text.size())}, block, 0, true);
}

bool SourceEditor::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
    if (alt || lines_.empty()) return false;

    if (ctrl) {
        switch (vk) {
            case 'A':
                SelectAll();
                return true;
            case 'Z':
                if (shift) return Redo();
                return Undo();
            case 'Y':
                return Redo();
            case VK_LEFT:
                MoveCaretHorizontal(-1, true, shift);
                EnsureCaretVisible();
                return true;
            case VK_RIGHT:
                MoveCaretHorizontal(1, true, shift);
                EnsureCaretVisible();
                return true;
            case VK_HOME:
                caret_ = {0, 0};
                if (!shift) sel_anchor_ = caret_;
                preferred_x_ = -1.0f;
                EnsureCaretVisible();
                return true;
            case VK_END: {
                int last = static_cast<int>(lines_.size()) - 1;
                caret_ = {last, static_cast<uint32_t>(lines_[last].text.size())};
                if (!shift) sel_anchor_ = caret_;
                preferred_x_ = -1.0f;
                EnsureCaretVisible();
                return true;
            }
            case VK_BACK: {
                if (HasSelection()) {
                    DeleteSelection();
                } else if (caret_.col > 0 || caret_.line > 0) {
                    SourcePos end = caret_;
                    MoveCaretHorizontal(-1, true, false);
                    ApplyEdit(caret_, end, L"", 0);
                }
                EnsureCaretVisible();
                return true;
            }
            case VK_DELETE: {
                if (HasSelection()) {
                    DeleteSelection();
                } else {
                    SourcePos start = caret_;
                    MoveCaretHorizontal(1, true, false);
                    if (caret_ != start) ApplyEdit(start, caret_, L"", 0);
                }
                EnsureCaretVisible();
                return true;
            }
            default:
                return false;
        }
    }

    switch (vk) {
        case VK_LEFT:
            MoveCaretHorizontal(-1, false, shift);
            EnsureCaretVisible();
            return true;
        case VK_RIGHT:
            MoveCaretHorizontal(1, false, shift);
            EnsureCaretVisible();
            return true;
        case VK_UP:
            MoveCaretVertical(-1, shift);
            EnsureCaretVisible();
            return true;
        case VK_DOWN:
            MoveCaretVertical(1, shift);
            EnsureCaretVisible();
            return true;
        case VK_HOME:
            MoveCaretLineBoundary(false, shift);
            EnsureCaretVisible();
            return true;
        case VK_END:
            MoveCaretLineBoundary(true, shift);
            EnsureCaretVisible();
            return true;
        case VK_PRIOR:
        case VK_NEXT: {
            float vp_h = std::max(line_h_, viewport_.bottom - viewport_.top);
            int step = std::max(1, static_cast<int>(vp_h / line_h_) - 2);
            MoveCaretVertical(vk == VK_PRIOR ? -step : step, shift);
            EnsureCaretVisible();
            return true;
        }
        case VK_TAB:
            if (HasSelection() || shift) {
                IndentSelectedLines(shift);
            } else {
                InsertText(L"    ", 0);
            }
            EnsureCaretVisible();
            return true;
        case VK_RETURN: {
            SourcePos s = NormalizedStart();
            const std::wstring& cur = lines_[s.line].text;
            std::wstring indent;
            for (uint32_t i = 0; i < s.col && i < cur.size(); ++i) {
                if (cur[i] == L' ' || cur[i] == L'\t') indent.push_back(cur[i]);
                else break;
            }
            InsertText(L"\n" + indent, 0);
            EnsureCaretVisible();
            return true;
        }
        case VK_BACK: {
            if (HasSelection()) {
                DeleteSelection();
            } else if (caret_.col > 0) {
                uint32_t del_len = 1;
                if (caret_.col >= 2 &&
                    IsLowSurrogateW(lines_[caret_.line].text[caret_.col - 1]) &&
                    IsHighSurrogateW(lines_[caret_.line].text[caret_.col - 2])) {
                    del_len = 2;
                }
                ApplyEdit({caret_.line, caret_.col - del_len}, caret_, L"", 2);
            } else if (caret_.line > 0) {
                uint32_t prev_len = static_cast<uint32_t>(lines_[caret_.line - 1].text.size());
                ApplyEdit({caret_.line - 1, prev_len}, {caret_.line, 0}, L"", 0);
            }
            EnsureCaretVisible();
            return true;
        }
        case VK_DELETE: {
            if (HasSelection()) {
                DeleteSelection();
            } else {
                uint32_t len = static_cast<uint32_t>(lines_[caret_.line].text.size());
                if (caret_.col < len) {
                    uint32_t del_len = 1;
                    if (IsHighSurrogateW(lines_[caret_.line].text[caret_.col]) &&
                        caret_.col + 1 < len &&
                        IsLowSurrogateW(lines_[caret_.line].text[caret_.col + 1])) {
                        del_len = 2;
                    }
                    ApplyEdit(caret_, {caret_.line, caret_.col + del_len}, L"", 0);
                } else if (caret_.line + 1 < static_cast<int>(lines_.size())) {
                    ApplyEdit(caret_, {caret_.line + 1, 0}, L"", 0);
                }
            }
            EnsureCaretVisible();
            return true;
        }
        default:
            return false;
    }
}

bool SourceEditor::OnChar(wchar_t ch) {
    if (ch < 32 || ch == 127) return false;
    std::wstring s(1, ch);
    int merge = (ch == L' ' || ch == L'\t') ? 0 : 1;
    InsertText(s, merge);
    EnsureCaretVisible();
    return true;
}

D2D1_POINT_2F SourceEditor::GetCaretClientDIP() const {
    if (lines_.empty()) return D2D1::Point2F(viewport_.left + gutter_w_ + 16.0f, viewport_.top + 24.0f);
    int li = std::clamp(caret_.line, 0, static_cast<int>(lines_.size()) - 1);
    const auto& le = lines_[li];
    float cx = text_left_;
    float cy = viewport_.top + (le.top - scroll_y_) + line_h_;
    if (le.layout) {
        float px = 0.0f, py = 0.0f;
        DWRITE_HIT_TEST_METRICS htm{};
        uint32_t c = std::min(caret_.col, static_cast<uint32_t>(le.text.size()));
        if (SUCCEEDED(le.layout->HitTestTextPosition(c, FALSE, &px, &py, &htm))) {
            cx = text_left_ + px;
            cy = viewport_.top + (le.top - scroll_y_) + py + std::max(line_h_, htm.height);
        }
    }
    cx = std::clamp(cx, viewport_.left, viewport_.right);
    cy = std::clamp(cy, viewport_.top, viewport_.bottom);
    return D2D1::Point2F(cx, cy);
}

}  // namespace mr
