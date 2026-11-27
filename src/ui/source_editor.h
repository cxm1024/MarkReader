#pragma once

#include "core/syntax_hl.h"
#include "core/types.h"
#include "ui/theme.h"

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mr {

struct SourcePos {
    int line = 0;
    uint32_t col = 0;

    bool operator==(const SourcePos& o) const { return line == o.line && col == o.col; }
    bool operator!=(const SourcePos& o) const { return !(*this == o); }
    bool operator<(const SourcePos& o) const {
        return line != o.line ? (line < o.line) : (col < o.col);
    }
};

struct EditorFindMatch {
    int line = 0;
    uint32_t start = 0;
    uint32_t length = 0;
};

// Lightweight Direct2D / DirectWrite Markdown source editor for quick emergency edits.
class SourceEditor {
public:
    void SetDocument(const std::string& utf8);
    std::string GetUtf8() const;

    bool IsModified() const;
    void MarkSaved();
    int LineCount() const { return static_cast<int>(lines_.size()); }
    SourcePos Caret() const { return caret_; }
    bool HasSelection() const { return sel_anchor_ != caret_; }

    void InvalidateAllLayouts();

    void EnsureLayout(
        const D2D1_RECT_F& viewport,
        int zoom_percent,
        IDWriteFactory5* dw,
        const ThemePalette& theme);

    void Render(
        ID2D1DeviceContext5* dc,
        const D2D1_RECT_F& viewport,
        const ThemePalette& theme,
        bool editor_focused,
        bool caret_visible,
        const std::wstring& ime_comp,
        const std::vector<EditorFindMatch>& find_matches,
        int active_find_idx);

    float ScrollY() const { return scroll_y_; }
    float TargetScrollY() const { return target_scroll_y_; }
    float MaxScrollY() const { return max_scroll_y_; }
    float TotalHeight() const { return total_height_; }
    void SetScrollY(float y);
    void ScrollBy(float dy);
    bool AnimateScroll();

    int GetTopVisibleLine() const;
    void ScrollToLineAtTop(int line);
    void SetCaretToLine(int line);
    void EnsureCaretVisible();

    D2D1_RECT_F ScrollbarTrackRect() const { return vscroll_track_; }
    D2D1_RECT_F ScrollbarThumbRect() const { return vscroll_thumb_; }

    std::wstring GetSelectedText() const;
    void SelectAll();
    void InsertText(const std::wstring& text, int merge_kind = 0);
    void DeleteSelection();
    void PasteText(const std::wstring& text);

    bool CanUndo() const { return undo_pos_ > 0; }
    bool CanRedo() const { return undo_pos_ < undo_stack_.size(); }
    bool Undo();
    bool Redo();

    std::vector<EditorFindMatch> FindAllMatches(const std::wstring& query) const;
    void ScrollMatchIntoView(const EditorFindMatch& m);

    bool OnLButtonDown(float win_x, float win_y, bool shift_down);
    bool OnLButtonDblClick(float win_x, float win_y);
    bool OnMouseMove(float win_x, float win_y);
    void OnLButtonUp();
    bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt);
    bool OnChar(wchar_t ch);

    D2D1_POINT_2F GetCaretClientDIP() const;

private:
    struct LineEntry {
        std::wstring text;
        MarkdownFenceState fence_in;
        MarkdownFenceState fence_out;
        ComPtr<IDWriteTextLayout> layout;
        float top = 0.0f;
        float height = 22.0f;
        bool layout_dirty = true;
    };

    struct UndoEntry {
        int start_line = 0;
        std::vector<std::wstring> old_lines;
        std::vector<std::wstring> new_lines;
        SourcePos anchor_before;
        SourcePos caret_before;
        SourcePos anchor_after;
        SourcePos caret_after;
        uint64_t timestamp_ms = 0;
        int merge_kind = 0;
    };

    void EnsureFormats(IDWriteFactory5* dw, float font_size, float num_size);
    void RefreshTokenBrushes(const ThemePalette& theme);
    void RecomputeFencesFrom(int start_line);
    void BuildLineLayout(int idx, IDWriteFactory5* dw, float wrap_w, const ThemePalette& theme);
    void RecomputeMetrics(float vp_h);
    void UpdateScrollbarRects();

    void ApplyEdit(
        SourcePos from,
        SourcePos to,
        const std::wstring& replacement,
        int merge_kind,
        bool select_after = false);

    SourcePos NormalizedStart() const { return (caret_ < sel_anchor_) ? caret_ : sel_anchor_; }
    SourcePos NormalizedEnd() const { return (caret_ < sel_anchor_) ? sel_anchor_ : caret_; }
    SourcePos HitTestWindowPoint(float win_x, float win_y) const;
    void MoveCaretHorizontal(int dir, bool word_step, bool keep_sel);
    void MoveCaretVertical(int delta_lines, bool keep_sel);
    void MoveCaretLineBoundary(bool to_end, bool keep_sel);
    void IndentSelectedLines(bool unindent);

    std::vector<LineEntry> lines_;
    bool use_crlf_ = true;

    SourcePos sel_anchor_{};
    SourcePos caret_{};
    float preferred_x_ = -1.0f;
    bool dragging_ = false;
    int pending_scroll_top_line_ = -1;

    std::vector<UndoEntry> undo_stack_;
    size_t undo_pos_ = 0;
    size_t saved_undo_pos_ = 0;
    bool saved_reachable_ = true;

    // Layout & rendering caches
    D2D1_RECT_F viewport_{};
    float scale_ = 1.0f;
    float font_size_ = 15.0f;
    float line_h_ = 22.5f;
    float gutter_w_ = 56.0f;
    float text_left_ = 72.0f;
    float text_wrap_w_ = 640.0f;
    float top_pad_ = 16.0f;
    float total_height_ = 0.0f;

    float scroll_y_ = 0.0f;
    float target_scroll_y_ = 0.0f;
    float max_scroll_y_ = 0.0f;
    D2D1_RECT_F vscroll_track_{};
    D2D1_RECT_F vscroll_thumb_{};

    int cached_zoom_ = 0;
    float cached_wrap_w_ = -1.0f;
    int dirty_fence_from_ = 0;
    bool layouts_dirty_ = true;
    bool cached_dark_ = false;
    bool has_cached_theme_ = false;

    ComPtr<IDWriteTextFormat> text_fmt_;
    ComPtr<IDWriteTextFormat> num_fmt_;
    std::array<ComPtr<IUnknown>, 9> token_effects_{};
};

}  // namespace mr
