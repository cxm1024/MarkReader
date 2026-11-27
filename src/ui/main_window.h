#pragma once

#include "core/file_watcher.h"
#include "core/markdown_doc.h"
#include "core/types.h"
#include "render/d2d_context.h"
#include "render/doc_layout.h"
#include "render/image_loader.h"
#include "ui/source_editor.h"
#include "ui/theme.h"

#include <windows.h>
#include <imm.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace mr {

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();

    bool Create(HINSTANCE hinst, int nCmdShow, const std::wstring& initial_file);
    HWND Handle() const { return hwnd_; }

    void OpenFile(const std::wstring& path, bool push_history = true);
    void LoadMarkdownString(const std::string& utf8, const std::wstring& virtual_title, const std::wstring& real_path = L"");

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void UpdateDwmAppearance();
    void ComputeRegions(
        D2D1_RECT_F& out_toolbar,
        D2D1_RECT_F& out_sidebar,
        D2D1_RECT_F& out_splitter,
        D2D1_RECT_F& out_viewport,
        D2D1_RECT_F& out_vscroll) const;

    struct ToolbarHitRects {
        D2D1_RECT_F icon_brand{};
        D2D1_RECT_F btn_outline{};
        D2D1_RECT_F btn_open{};
        D2D1_RECT_F btn_edit{};
        D2D1_RECT_F btn_find{};
        D2D1_RECT_F btn_zoom_out{};
        D2D1_RECT_F btn_zoom_reset{};
        D2D1_RECT_F btn_zoom_in{};
        D2D1_RECT_F btn_theme{};
        D2D1_RECT_F btn_min{};
        D2D1_RECT_F btn_max{};
        D2D1_RECT_F btn_close{};
    };
    ToolbarHitRects ComputeToolbarButtons(const D2D1_RECT_F& toolbar) const;
    LRESULT OnNcHitTest(POINT screen_pt) const;

    void RebuildLayout(bool preserve_scroll_ratio = true);
    void Render();
    void RenderToolbar(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& toolbar);
    void RenderSidebar(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& sidebar);
    void RenderFindBar(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& viewport);
    void RenderLightbox(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t);
    void RenderOverlays(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& viewport);

    void QueueResize();
    bool ApplyResize();
    void OnDpiChanged(UINT new_dpi, const RECT* suggested_rect);
    void OnMouseMove(float x_dip, float y_dip, WPARAM wp);
    void OnLButtonDown(float x_dip, float y_dip, WPARAM wp);
    void OnLButtonDblClick(float x_dip, float y_dip);
    void OnLButtonUp(float x_dip, float y_dip);
    void OnMButtonDown(float x_dip, float y_dip);
    void OnMButtonUp(float x_dip, float y_dip);
    void OnRButtonUp(float x_dip, float y_dip, POINT screen_pt);
    void OnMouseWheel(float x_dip, float y_dip, int delta, bool horizontal, WPARAM wp);
    void StopAutoScroll();
    void UpdateHScrollDrag(float x_dip);
    void RenderAutoScrollIndicator(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t) const;
    void OnKeyDown(WPARAM vk);
    void OnChar(wchar_t ch);
    void OnImeComposition(LPARAM lparam);
    void UpdateImePosition();
    void UpdateImeEnablement();
    void SetFindFocused(bool focused);
    void ToggleEditMode();
    bool SaveCurrentDocument(bool show_toast = true);
    bool ConfirmDiscardOrSaveBeforeLeave();
    void OnDropFiles(HDROP hdrop);
    void OnTimer(UINT_PTR id);

    void SetZoom(int new_zoom);
    void CycleTheme();
    void SetTheme(ThemeMode mode);
    void ToggleOutline();
    void ShowFindBar(bool show);
    void RunSearch(const std::wstring& query, bool jump_to_first = true);
    void JumpToMatch(int delta);
    void ScrollToY(float target_y, bool animate = true);
    void HandleLinkClick(const std::wstring& url);
    void OpenFileDialog();
    void ReloadCurrentFile(bool show_toast = false);
    void NavigateHistory(int dir);
    void CopyTextToClipboard(const std::wstring& text, const std::wstring& toast_msg);
    std::wstring ReadTextFromClipboard();
    void ShowToast(const std::wstring& msg);
    void UpdateWindowTitle();

    float PxToDip(int px) const { return static_cast<float>(px) * 96.0f / dpi_; }
    int DipToPx(float dip) const { return static_cast<int>(dip * dpi_ / 96.0f + 0.5f); }
    float MaxDocScrollY() const;
    float MaxOutlineScrollY() const;
    D2D1_RECT_F GetFindBarRect(const D2D1_RECT_F& viewport) const;
    D2D1_RECT_F GetFindInputRect(const D2D1_RECT_F& bar) const;

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HIMC saved_himc_ = nullptr;

    float dpi_ = 96.0f;
    bool in_size_move_ = false;
    bool resize_pending_ = false;
    bool resize_timer_active_ = false;
    float client_w_dip_ = 1240.0f;
    float client_h_dip_ = 840.0f;

    AppConfig config_;
    D2DContext d2d_;
    ImageCache images_;
    FileWatcher watcher_;

    MarkdownDocument doc_;
    std::string source_utf8_;
    DocumentLayout layout_;
    TextSelection selection_;
    SourceEditor editor_;
    bool edit_mode_ = false;
    bool editor_caret_visible_ = true;

    // Navigation history
    std::vector<std::wstring> history_;
    int history_index_ = -1;

    // Scroll & animations
    float scroll_y_ = 0.0f;
    float target_scroll_y_ = 0.0f;
    float outline_scroll_y_ = 0.0f;
    bool anim_timer_active_ = false;

    // Interactive state
    bool tracking_mouse_leave_ = false;
    int hovered_toolbar_btn_ = -1;
    int hovered_outline_idx_ = -1;
    int hovered_copy_code_idx_ = -1;
    int copied_code_idx_ = -1;
    ULONGLONG copied_code_until_ = 0;
    int hovered_copy_math_idx_ = -1;
    int copied_math_idx_ = -1;
    ULONGLONG copied_math_until_ = 0;
    std::wstring hovered_link_url_;

    enum class HScrollKind : uint8_t { None = 0, Code, Table };

    bool dragging_selection_ = false;
    bool dragging_vscroll_ = false;
    float vscroll_drag_offset_ = 0.0f;
    bool dragging_splitter_ = false;

    HScrollKind hovered_hscroll_kind_ = HScrollKind::None;
    int hovered_hscroll_idx_ = -1;
    HScrollKind dragging_hscroll_kind_ = HScrollKind::None;
    int active_hscroll_idx_ = -1;
    float hscroll_drag_offset_ = 0.0f;

    // Browser-style middle-mouse-button auto-scroll
    bool autoscroll_active_ = false;
    bool autoscroll_moved_ = false;
    HScrollKind autoscroll_h_kind_ = HScrollKind::None;
    int autoscroll_h_idx_ = -1;
    float autoscroll_anchor_x_ = 0.0f;
    float autoscroll_anchor_y_ = 0.0f;
    float autoscroll_cur_x_ = 0.0f;
    float autoscroll_cur_y_ = 0.0f;
    POINT autoscroll_last_screen_pt_{};

    // Find bar (native Direct2D text input + Win32 IME)
    bool find_visible_ = false;
    bool find_focused_ = false;
    bool find_sel_all_ = false;
    bool find_caret_visible_ = true;
    size_t find_caret_ = 0;
    float find_scroll_x_ = 0.0f;
    std::wstring find_query_;
    std::wstring ime_comp_;
    std::vector<SearchMatch> search_matches_;
    std::vector<EditorFindMatch> editor_find_matches_;
    int active_match_idx_ = -1;
    int hovered_find_btn_ = -1;

    // Lightbox
    int lightbox_image_idx_ = -1;

    // Toast
    std::wstring toast_text_;
    ULONGLONG toast_until_ = 0;
};

}  // namespace mr
