#include "ui/main_window.h"
#include "core/sample_doc.h"
#include "math/d2d_tex_backend.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mr {

namespace {

constexpr UINT_PTR kTimerAnim = 1;
constexpr UINT_PTR kTimerWatch = 2;
constexpr UINT_PTR kTimerCaret = 3;
constexpr UINT_PTR kTimerAutoScroll = 4;

constexpr float kToolbarH = 44.0f;
constexpr float kSplitterW = 5.0f;
constexpr float kOutlineRowH = 29.0f;
constexpr float kOutlineHeaderH = 42.0f;
constexpr float kOutlineBottomPad = 8.0f;

enum ToolbarBtnId {
    BTN_OUTLINE = 0,
    BTN_OPEN,
    BTN_EDIT,
    BTN_FIND,
    BTN_ZOOM_OUT,
    BTN_ZOOM_RESET,
    BTN_ZOOM_IN,
    BTN_THEME,
    BTN_MIN,
    BTN_MAX,
    BTN_CLOSE
};

enum FindBtnId {
    FBTN_PREV = 0,
    FBTN_NEXT,
    FBTN_CLOSE
};

enum ContextMenuId {
    CMD_COPY = 1001,
    CMD_CUT,
    CMD_PASTE,
    CMD_UNDO,
    CMD_REDO,
    CMD_SAVE,
    CMD_TOGGLE_EDIT,
    CMD_SELECT_ALL,
    CMD_OPEN_FILE,
    CMD_RELOAD,
    CMD_TOGGLE_OUTLINE,
    CMD_FIND,
    CMD_ZOOM_IN,
    CMD_ZOOM_OUT,
    CMD_ZOOM_RESET,
    CMD_THEME_LIGHT,
    CMD_THEME_DARK,
    CMD_THEME_SEPIA,
    CMD_WELCOME,
    CMD_RECENT_BASE = 1100
};

bool RectContains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

int GetDpiSystemMetric(int index, UINT dpi) {
    using GetMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static auto fn = reinterpret_cast<GetMetricsForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (fn && dpi > 0) return fn(index, dpi);
    return GetSystemMetrics(index);
}

ComPtr<IDWriteTextLayout> MakeUiText(
    IDWriteFactory5* dw,
    const std::wstring& text,
    float size,
    bool bold,
    float max_w,
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
    bool ellipsis = false) {
    ComPtr<IDWriteTextLayout> layout;
    if (!dw) return layout;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw->CreateTextFormat(
            L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"", fmt.Put()))) {
        return layout;
    }
    fmt->SetTextAlignment(align);
    fmt->SetWordWrapping(ellipsis ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    if (ellipsis) {
        ComPtr<IDWriteInlineObject> trim_sign;
        dw->CreateEllipsisTrimmingSign(fmt.Get(), trim_sign.Put());
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trim, trim_sign.Get());
    }
    dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(),
                         std::max(10.0f, max_w), 2000.0f, layout.Put());
    return layout;
}

ComPtr<ID2D1StrokeStyle> MakeRoundStrokeStyle(ID2D1Factory4* factory) {
    ComPtr<ID2D1StrokeStyle> style;
    if (!factory) return style;
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 4.0f);
    factory->CreateStrokeStyle(&props, nullptr, 0, style.Put());
    return style;
}

void DrawButtonBox(
    ID2D1DeviceContext5* dc,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& r,
    bool hovered,
    bool active,
    const ThemePalette& t,
    float radius = 6.0f) {
    if (active) {
        brush->SetColor(t.accent_soft);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    } else if (hovered) {
        brush->SetColor(t.bg_hover);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    }
}

void DrawToolbarText(
    ID2D1DeviceContext5* dc,
    ID2D1SolidColorBrush* brush,
    IDWriteFactory5* dw,
    const D2D1_RECT_F& r,
    const std::wstring& label,
    float size,
    bool bold,
    D2D1_COLOR_F color) {
    if (auto tl = MakeUiText(dw, label, size, bold, r.right - r.left, DWRITE_TEXT_ALIGNMENT_CENTER, true)) {
        DWRITE_TEXT_METRICS tm{};
        tl->GetMetrics(&tm);
        float y = r.top + ((r.bottom - r.top) - tm.height) * 0.5f;
        brush->SetColor(color);
        dc->DrawTextLayout(D2D1::Point2F(r.left, y), tl.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
}

// Vector sidebar toggle icon: window frame with left ~36% shaded solid.
void DrawSidebarIcon(
    ID2D1DeviceContext5* dc,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& btn_rect,
    D2D1_COLOR_F color,
    bool is_open) {
    float cx = (btn_rect.left + btn_rect.right) * 0.5f;
    float cy = (btn_rect.top + btn_rect.bottom) * 0.5f;
    D2D1_RECT_F frame = D2D1::RectF(cx - 8.0f, cy - 6.0f, cx + 8.0f, cy + 6.0f);
    float split_x = frame.left + 5.6f;

    D2D1_COLOR_F pane_col = color;
    if (!is_open) pane_col.a *= 0.85f;
    brush->SetColor(pane_col);

    dc->PushAxisAlignedClip(D2D1::RectF(frame.left, frame.top, split_x, frame.bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->FillRoundedRectangle(D2D1::RoundedRect(frame, 2.2f, 2.2f), brush);
    dc->PopAxisAlignedClip();

    brush->SetColor(color);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(frame, 2.2f, 2.2f), brush, 1.45f);
    dc->DrawLine(D2D1::Point2F(split_x, frame.top), D2D1::Point2F(split_x, frame.bottom), brush, 1.3f);
}

// Vector magnifying glass icon.
void DrawSearchIcon(
    ID2D1DeviceContext5* dc,
    ID2D1SolidColorBrush* brush,
    ID2D1StrokeStyle* round_stroke,
    const D2D1_RECT_F& btn_rect,
    D2D1_COLOR_F color) {
    float cx = (btn_rect.left + btn_rect.right) * 0.5f;
    float cy = (btn_rect.top + btn_rect.bottom) * 0.5f;
    brush->SetColor(color);
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 1.5f, cy - 1.5f), 4.6f, 4.6f), brush, 1.55f);
    dc->DrawLine(
        D2D1::Point2F(cx + 1.9f, cy + 1.9f),
        D2D1::Point2F(cx + 6.0f, cy + 6.0f),
        brush, 1.65f, round_stroke);
}

// Crisp vector theme icons (Sun, Crescent Moon, Open Book).
void DrawThemeIcon(
    ID2D1DeviceContext5* dc,
    ID2D1Factory4* factory,
    ID2D1SolidColorBrush* brush,
    ID2D1StrokeStyle* round_stroke,
    float cx,
    float cy,
    ThemeMode mode,
    D2D1_COLOR_F color) {
    brush->SetColor(color);
    if (mode == ThemeMode::Light) {
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), brush, 1.4f);
        constexpr float kPi = 3.14159265f;
        for (int i = 0; i < 8; ++i) {
            float ang = i * (kPi / 4.0f);
            float cs = std::cos(ang), sn = std::sin(ang);
            dc->DrawLine(
                D2D1::Point2F(cx + cs * 4.8f, cy + sn * 4.8f),
                D2D1::Point2F(cx + cs * 6.5f, cy + sn * 6.5f),
                brush, 1.35f, round_stroke);
        }
        return;
    }

    if (mode == ThemeMode::Dark) {
        if (!factory) return;
        ComPtr<ID2D1PathGeometry> moon;
        if (SUCCEEDED(factory->CreatePathGeometry(moon.Put())) && moon) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(moon->Open(sink.Put())) && sink) {
                // Smooth crescent via two opposing circular arcs
                sink->BeginFigure(D2D1::Point2F(cx + 1.2f, cy - 6.0f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 1.2f, cy + 6.0f),
                    D2D1::SizeF(6.2f, 6.2f), 0.0f,
                    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 1.2f, cy - 6.0f),
                    D2D1::SizeF(4.8f, 5.8f), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(moon.Get(), brush);
            }
        }
        return;
    }

    // Sepia: clean open book vector icon
    if (!factory) return;
    ComPtr<ID2D1PathGeometry> book;
    if (SUCCEEDED(factory->CreatePathGeometry(book.Put())) && book) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(book->Open(sink.Put())) && sink) {
            sink->BeginFigure(D2D1::Point2F(cx, cy - 3.6f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx - 6.2f, cy - 5.0f));
            sink->AddLine(D2D1::Point2F(cx - 6.2f, cy + 4.4f));
            sink->AddLine(D2D1::Point2F(cx, cy + 5.6f));
            sink->AddLine(D2D1::Point2F(cx + 6.2f, cy + 4.4f));
            sink->AddLine(D2D1::Point2F(cx + 6.2f, cy - 5.0f));
            sink->AddLine(D2D1::Point2F(cx, cy - 3.6f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            dc->DrawGeometry(book.Get(), brush, 1.35f, round_stroke);
        }
    }
    dc->DrawLine(D2D1::Point2F(cx, cy - 3.6f), D2D1::Point2F(cx, cy + 5.6f), brush, 1.25f, round_stroke);
}

// Compact software mark: M + underline pulled cleanly inward inside rounded badge.
void DrawAppBrandIcon(
    ID2D1DeviceContext5* dc,
    ID2D1Factory4* factory,
    ID2D1SolidColorBrush* brush,
    ID2D1StrokeStyle* round_stroke,
    const D2D1_RECT_F& rect) {
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;
    float half = std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f;
    D2D1_RECT_F box = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);

    brush->SetColor(D2D1::ColorF(0x111827));
    dc->FillRoundedRectangle(D2D1::RoundedRect(box, half * 0.44f, half * 0.44f), brush);
    brush->SetColor(D2D1::ColorF(0x2563EB));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(box, half * 0.44f, half * 0.44f), brush, 1.15f);

    if (factory) {
        ComPtr<ID2D1PathGeometry> m_path;
        if (SUCCEEDED(factory->CreatePathGeometry(m_path.Put())) && m_path) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(m_path->Open(sink.Put())) && sink) {
                float mx0 = cx - half * 0.40f;
                float mx1 = cx + half * 0.40f;
                float my_top = cy - half * 0.42f;
                float my_mid = cy - half * 0.01f;
                float my_bot = cy + half * 0.20f;
                sink->BeginFigure(D2D1::Point2F(mx0, my_bot), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(mx0, my_top));
                sink->AddLine(D2D1::Point2F(cx, my_mid));
                sink->AddLine(D2D1::Point2F(mx1, my_top));
                sink->AddLine(D2D1::Point2F(mx1, my_bot));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                brush->SetColor(D2D1::ColorF(0xF1F5F9));
                dc->DrawGeometry(m_path.Get(), brush, 1.45f, round_stroke);
            }
        }
    }

    brush->SetColor(D2D1::ColorF(0x60A5FA));
    dc->DrawLine(
        D2D1::Point2F(cx - half * 0.40f, cy + half * 0.45f),
        D2D1::Point2F(cx + half * 0.40f, cy + half * 0.45f),
        brush, 1.45f, round_stroke);
}

void DrawCaptionIcon(
    ID2D1DeviceContext5* dc,
    ID2D1SolidColorBrush* brush,
    ID2D1StrokeStyle* round_stroke,
    const D2D1_RECT_F& btn_rect,
    int btn_id,
    bool zoomed,
    D2D1_COLOR_F color) {
    float cx = (btn_rect.left + btn_rect.right) * 0.5f;
    float cy = (btn_rect.top + btn_rect.bottom) * 0.5f;
    brush->SetColor(color);
    if (btn_id == BTN_MIN) {
        dc->DrawLine(D2D1::Point2F(cx - 5.0f, cy), D2D1::Point2F(cx + 5.0f, cy), brush, 1.25f, round_stroke);
    } else if (btn_id == BTN_MAX) {
        if (!zoomed) {
            dc->DrawRectangle(D2D1::RectF(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f), brush, 1.2f);
        } else {
            D2D1_RECT_F back = D2D1::RectF(cx - 2.5f, cy - 5.0f, cx + 5.0f, cy + 2.5f);
            dc->DrawLine(D2D1::Point2F(back.left, back.top + 2.0f), D2D1::Point2F(back.left, back.top), brush, 1.15f);
            dc->DrawLine(D2D1::Point2F(back.left, back.top), D2D1::Point2F(back.right, back.top), brush, 1.15f);
            dc->DrawLine(D2D1::Point2F(back.right, back.top), D2D1::Point2F(back.right, back.bottom), brush, 1.15f);
            dc->DrawLine(D2D1::Point2F(back.right, back.bottom), D2D1::Point2F(back.right - 2.0f, back.bottom), brush, 1.15f);
            dc->DrawRectangle(D2D1::RectF(cx - 5.0f, cy - 2.5f, cx + 2.5f, cy + 5.0f), brush, 1.15f);
        }
    } else if (btn_id == BTN_CLOSE) {
        dc->DrawLine(D2D1::Point2F(cx - 4.5f, cy - 4.5f), D2D1::Point2F(cx + 4.5f, cy + 4.5f), brush, 1.3f, round_stroke);
        dc->DrawLine(D2D1::Point2F(cx + 4.5f, cy - 4.5f), D2D1::Point2F(cx - 4.5f, cy + 4.5f), brush, 1.3f, round_stroke);
    }
}

bool IsMarkdownFileExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    return _wcsicmp(ext.c_str(), L".md") == 0 ||
           _wcsicmp(ext.c_str(), L".markdown") == 0 ||
           _wcsicmp(ext.c_str(), L".mdown") == 0 ||
           _wcsicmp(ext.c_str(), L".txt") == 0;
}

}  // namespace

MainWindow::~MainWindow() = default;

bool MainWindow::Create(HINSTANCE hinst, int nCmdShow, const std::wstring& initial_file) {
    hinst_ = hinst;
    config_ = LoadAppConfig();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = hinst_;
    wc.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MarkReaderMainWindow";
    if (!RegisterClassExW(&wc)) return false;

    // Center window on primary work area (config stores DIPs; scale to monitor DPI).
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HDC screen_dc = GetDC(nullptr);
    int sys_dpi = screen_dc ? GetDeviceCaps(screen_dc, LOGPIXELSX) : 96;
    if (screen_dc) ReleaseDC(nullptr, screen_dc);
    float init_scale = static_cast<float>(std::max(96, sys_dpi)) / 96.0f;

    int target_w = static_cast<int>(config_.window_width * init_scale + 0.5f);
    int target_h = static_cast<int>(config_.window_height * init_scale + 0.5f);
    int w = std::min(target_w, static_cast<int>(work.right - work.left - 32));
    int h = std::min(target_h, static_cast<int>(work.bottom - work.top - 32));
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        L"MarkReader",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        nullptr, nullptr, hinst_, this);
    if (!hwnd_) return false;

    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    if (dpi_ <= 0.0f) dpi_ = 96.0f;

    UpdateDwmAppearance();
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT cw = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    UINT ch = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    client_w_dip_ = PxToDip(static_cast<int>(cw));
    client_h_dip_ = PxToDip(static_cast<int>(ch));

    if (!d2d_.Initialize(hwnd_, cw, ch, dpi_)) {
        MessageBoxW(hwnd_, L"Direct2D 初始化失败，无法启动渲染。", L"MarkReader", MB_ICONERROR);
        return false;
    }

    images_.SetNotifyHwnd(hwnd_);
    MathEngine::Instance().Initialize();

    // Save IME context and enable it dynamically only while typing inside the Find input box.
    saved_himc_ = ImmAssociateContext(hwnd_, nullptr);

    if (!initial_file.empty() && GetFileAttributesW(initial_file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        OpenFile(initial_file, true);
    } else {
        LoadMarkdownString(GetWelcomeMarkdownUtf8(), L"欢迎指南");
    }

    SetTimer(hwnd_, kTimerWatch, 800, nullptr);

    if (config_.window_maximized && nCmdShow == SW_SHOWNORMAL) {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    } else {
        ShowWindow(hwnd_, nCmdShow);
    }
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (wp == TRUE) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                UINT win_dpi = GetDpiForWindow(hwnd_);
                if (win_dpi == 0) win_dpi = static_cast<UINT>(dpi_);
                int fx = GetDpiSystemMetric(SM_CXFRAME, win_dpi) + GetDpiSystemMetric(SM_CXPADDEDBORDER, win_dpi);
                int fy = GetDpiSystemMetric(SM_CYFRAME, win_dpi) + GetDpiSystemMetric(SM_CXPADDEDBORDER, win_dpi);
                if (IsZoomed(hwnd_)) {
                    params->rgrc[0].left += fx;
                    params->rgrc[0].right -= fx;
                    params->rgrc[0].top += fy;
                    params->rgrc[0].bottom -= fy;
                    HMONITOR mon = MonitorFromRect(&params->rgrc[0], MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{sizeof(mi)};
                    if (mon && GetMonitorInfoW(mon, &mi)) {
                        params->rgrc[0].left = std::max(params->rgrc[0].left, mi.rcWork.left);
                        params->rgrc[0].top = std::max(params->rgrc[0].top, mi.rcWork.top);
                        params->rgrc[0].right = std::min(params->rgrc[0].right, mi.rcWork.right);
                        params->rgrc[0].bottom = std::min(params->rgrc[0].bottom, mi.rcWork.bottom);
                    }
                }
                return 0;
            }
            break;
        }

        case WM_NCHITTEST: {
            return OnNcHitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        }

        case WM_GETMINMAXINFO: {
            DefWindowProcW(hwnd_, msg, wp, lp);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = DipToPx(590.0f);
            mmi->ptMinTrackSize.y = DipToPx(380.0f);
            return 0;
        }

        case WM_NCRBUTTONUP: {
            if (wp == HTCAPTION) {
                HMENU sys = GetSystemMenu(hwnd_, FALSE);
                if (sys) {
                    UINT cmd = TrackPopupMenu(
                        sys, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                        GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0, hwnd_, nullptr);
                    if (cmd != 0) SendMessageW(hwnd_, WM_SYSCOMMAND, cmd, 0);
                }
                return 0;
            }
            break;
        }

        case WM_NCMOUSEMOVE:
        case WM_MOUSELEAVE: {
            tracking_mouse_leave_ = false;
            if (hovered_toolbar_btn_ != -1) {
                hovered_toolbar_btn_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (msg == WM_MOUSELEAVE) return 0;
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_SIZE: {
            if (wp != SIZE_MINIMIZED) {
                UINT cw = LOWORD(lp);
                UINT ch = HIWORD(lp);
                OnSize(cw, ch);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            OnDpiChanged(HIWORD(wp), reinterpret_cast<const RECT*>(lp));
            return 0;
        }

        case WM_MOUSEMOVE: {
            OnMouseMove(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp)), wp);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            if (autoscroll_active_) {
                StopAutoScroll();
                return 0;
            }
            OnLButtonDown(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp)), wp);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (autoscroll_active_) {
                StopAutoScroll();
                return 0;
            }
            OnLButtonDblClick(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp)));
            return 0;
        }

        case WM_LBUTTONUP: {
            OnLButtonUp(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp)));
            return 0;
        }

        case WM_MBUTTONDOWN: {
            SetFocus(hwnd_);
            OnMButtonDown(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp)));
            return 0;
        }

        case WM_MBUTTONUP: {
            OnMButtonUp(PxToDip(GET_X_LPARAM(lp)), PxToDip(GET_Y_LPARAM(lp)));
            return 0;
        }

        case WM_RBUTTONDOWN: {
            if (autoscroll_active_) {
                StopAutoScroll();
                return 0;
            }
            break;
        }

        case WM_RBUTTONUP: {
            if (autoscroll_active_) {
                StopAutoScroll();
                return 0;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT spt = pt;
            ClientToScreen(hwnd_, &spt);
            OnRButtonUp(PxToDip(pt.x), PxToDip(pt.y), spt);
            return 0;
        }

        case WM_KILLFOCUS:
        case WM_CANCELMODE: {
            if (autoscroll_active_) StopAutoScroll();
            break;
        }

        case WM_CAPTURECHANGED: {
            if (autoscroll_active_ && reinterpret_cast<HWND>(lp) != hwnd_) {
                autoscroll_active_ = false;
                KillTimer(hwnd_, kTimerAutoScroll);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            bool shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
            OnMouseWheel(PxToDip(pt.x), PxToDip(pt.y), delta, shift, wp);
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            OnMouseWheel(PxToDip(pt.x), PxToDip(pt.y), -delta, true, wp);
            return 0;
        }

        case WM_KEYDOWN: {
            OnKeyDown(wp);
            return 0;
        }

        case WM_CHAR: {
            if ((find_visible_ && find_focused_) || (edit_mode_ && !find_focused_)) {
                OnChar(static_cast<wchar_t>(wp));
            }
            return 0;
        }

        case WM_IME_STARTCOMPOSITION: {
            if ((find_visible_ && find_focused_) || (edit_mode_ && !find_focused_)) {
                UpdateImePosition();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }

        case WM_IME_COMPOSITION: {
            if ((find_visible_ && find_focused_) || (edit_mode_ && !find_focused_)) {
                OnImeComposition(lp);
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }

        case WM_IME_ENDCOMPOSITION: {
            if (!ime_comp_.empty()) {
                ime_comp_.clear();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }

        case WM_IME_NOTIFY: {
            if (((find_visible_ && find_focused_) || (edit_mode_ && !find_focused_)) &&
                (wp == IMN_OPENCANDIDATE || wp == IMN_CHANGECANDIDATE || wp == IMN_SETCONVERSIONMODE)) {
                UpdateImePosition();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }

        case WM_DROPFILES: {
            OnDropFiles(reinterpret_cast<HDROP>(wp));
            return 0;
        }

        case WM_TIMER: {
            OnTimer(static_cast<UINT_PTR>(wp));
            return 0;
        }

        case WM_ACTIVATE: {
            if (LOWORD(wp) != WA_INACTIVE && watcher_.HasChanged() && !(edit_mode_ && editor_.IsModified())) {
                ReloadCurrentFile(true);
            }
            return 0;
        }

        case WM_CLOSE: {
            if (!ConfirmDiscardOrSaveBeforeLeave()) return 0;
            break;
        }

        case WM_APP_IMAGE_LOADED: {
            images_.ProcessCompletedDownloads();
            RebuildLayout(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_COMMAND: {
            int cmd = LOWORD(wp);
            if (cmd == CMD_COPY) {
                if (find_visible_ && find_focused_ && !find_query_.empty()) {
                    CopyTextToClipboard(find_query_, L"已复制搜索文本");
                } else if (edit_mode_ && editor_.HasSelection()) {
                    CopyTextToClipboard(editor_.GetSelectedText(), L"已复制到剪贴板");
                } else if (selection_.HasSelection()) {
                    CopyTextToClipboard(layout_.ExtractSelectedText(selection_), L"已复制选中文本");
                }
            } else if (cmd == CMD_CUT && edit_mode_ && editor_.HasSelection()) {
                CopyTextToClipboard(editor_.GetSelectedText(), L"");
                editor_.DeleteSelection();
                if (find_visible_ && !find_query_.empty()) RunSearch(find_query_, false);
                UpdateWindowTitle();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (cmd == CMD_PASTE && edit_mode_) {
                std::wstring pasted = ReadTextFromClipboard();
                if (!pasted.empty()) {
                    editor_.PasteText(pasted);
                    if (find_visible_ && !find_query_.empty()) RunSearch(find_query_, false);
                    UpdateWindowTitle();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else if (cmd == CMD_UNDO && edit_mode_) {
                if (editor_.Undo()) {
                    editor_.EnsureCaretVisible();
                    if (find_visible_ && !find_query_.empty()) RunSearch(find_query_, false);
                    UpdateWindowTitle();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else if (cmd == CMD_REDO && edit_mode_) {
                if (editor_.Redo()) {
                    editor_.EnsureCaretVisible();
                    if (find_visible_ && !find_query_.empty()) RunSearch(find_query_, false);
                    UpdateWindowTitle();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else if (cmd == CMD_SAVE && edit_mode_) {
                SaveCurrentDocument(true);
            } else if (cmd == CMD_TOGGLE_EDIT) {
                ToggleEditMode();
            } else if (cmd == CMD_SELECT_ALL) {
                if (edit_mode_ && !find_focused_) {
                    editor_.SelectAll();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else {
                    OnKeyDown('A');
                }
            } else if (cmd == CMD_OPEN_FILE) {
                OpenFileDialog();
            } else if (cmd == CMD_RELOAD) {
                if (!edit_mode_ || ConfirmDiscardOrSaveBeforeLeave()) {
                    ReloadCurrentFile(true);
                }
            } else if (cmd == CMD_TOGGLE_OUTLINE) {
                ToggleOutline();
            } else if (cmd == CMD_FIND) {
                ShowFindBar(true);
            } else if (cmd == CMD_ZOOM_IN) {
                SetZoom(config_.zoom_percent + 10);
            } else if (cmd == CMD_ZOOM_OUT) {
                SetZoom(config_.zoom_percent - 10);
            } else if (cmd == CMD_ZOOM_RESET) {
                SetZoom(100);
            } else if (cmd == CMD_THEME_LIGHT) {
                SetTheme(ThemeMode::Light);
            } else if (cmd == CMD_THEME_DARK) {
                SetTheme(ThemeMode::Dark);
            } else if (cmd == CMD_THEME_SEPIA) {
                SetTheme(ThemeMode::Sepia);
            } else if (cmd == CMD_WELCOME) {
                if (ConfirmDiscardOrSaveBeforeLeave()) {
                    edit_mode_ = false;
                    watcher_.Clear();
                    LoadMarkdownString(GetWelcomeMarkdownUtf8(), L"欢迎指南");
                    UpdateImeEnablement();
                }
            } else if (cmd >= CMD_RECENT_BASE && cmd < CMD_RECENT_BASE + 10) {
                size_t idx = static_cast<size_t>(cmd - CMD_RECENT_BASE);
                if (idx < config_.recent_files.size() && ConfirmDiscardOrSaveBeforeLeave()) {
                    edit_mode_ = false;
                    OpenFile(config_.recent_files[idx], true);
                    UpdateImeEnablement();
                }
            }
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd_, kTimerAnim);
            KillTimer(hwnd_, kTimerWatch);
            KillTimer(hwnd_, kTimerCaret);
            KillTimer(hwnd_, kTimerAutoScroll);
            if (saved_himc_) {
                ImmAssociateContext(hwnd_, saved_himc_);
                saved_himc_ = nullptr;
            }
            WINDOWPLACEMENT wp_info{sizeof(WINDOWPLACEMENT)};
            if (GetWindowPlacement(hwnd_, &wp_info)) {
                config_.window_maximized = (wp_info.showCmd == SW_SHOWMAXIMIZED);
                RECT& nr = wp_info.rcNormalPosition;
                float s = dpi_ / 96.0f;
                config_.window_width = std::max(640, static_cast<int>((nr.right - nr.left) / s + 0.5f));
                config_.window_height = std::max(480, static_cast<int>((nr.bottom - nr.top) / s + 0.5f));
            }
            SaveAppConfig(config_);
            images_.Clear();
            d2d_.Shutdown();
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::UpdateDwmAppearance() {
    if (!hwnd_) return;
    const ThemePalette& t = GetThemePalette(config_.theme);
    BOOL dark = t.is_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    MARGINS m{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
}

LRESULT MainWindow::OnNcHitTest(POINT screen_pt) const {
    POINT pt = screen_pt;
    ScreenToClient(hwnd_, &pt);
    float x = PxToDip(pt.x);
    float y = PxToDip(pt.y);

    if (lightbox_image_idx_ >= 0) {
        return HTCLIENT;
    }

    if (!IsZoomed(hwnd_)) {
        constexpr float kResizeSide = 5.0f;
        constexpr float kResizeTop = 4.0f;
        const bool left = x < kResizeSide;
        const bool right = x >= client_w_dip_ - kResizeSide;
        const bool top = y < kResizeTop;
        const bool bottom = y >= client_h_dip_ - kResizeSide;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    if (RectContains(tb, x, y)) {
        ToolbarHitRects tr = ComputeToolbarButtons(tb);
        if (RectContains(tr.btn_outline, x, y) ||
            RectContains(tr.btn_open, x, y) ||
            RectContains(tr.btn_edit, x, y) ||
            RectContains(tr.btn_find, x, y) ||
            RectContains(tr.btn_zoom_out, x, y) ||
            RectContains(tr.btn_zoom_reset, x, y) ||
            RectContains(tr.btn_zoom_in, x, y) ||
            RectContains(tr.btn_theme, x, y) ||
            RectContains(tr.btn_min, x, y) ||
            RectContains(tr.btn_max, x, y) ||
            RectContains(tr.btn_close, x, y)) {
            return HTCLIENT;
        }
        return HTCAPTION;
    }
    return HTCLIENT;
}

void MainWindow::ComputeRegions(
    D2D1_RECT_F& out_toolbar,
    D2D1_RECT_F& out_sidebar,
    D2D1_RECT_F& out_splitter,
    D2D1_RECT_F& out_viewport,
    D2D1_RECT_F& out_vscroll) const {
    out_toolbar = D2D1::RectF(0.0f, 0.0f, client_w_dip_, kToolbarH);
    const float body_top = kToolbarH;
    const float body_bot = std::max(kToolbarH + 1.0f, client_h_dip_);

    float sb_w = 0.0f;
    if (config_.outline_visible && client_w_dip_ >= 560.0f) {
        sb_w = std::clamp(config_.outline_width, 180.0f, std::min(440.0f, client_w_dip_ * 0.42f));
        out_sidebar = D2D1::RectF(0.0f, body_top, sb_w, body_bot);
        out_splitter = D2D1::RectF(sb_w, body_top, sb_w + kSplitterW, body_bot);
        sb_w += kSplitterW;
    } else {
        out_sidebar = D2D1::RectF(0, body_top, 0, body_bot);
        out_splitter = D2D1::RectF(0, body_top, 0, body_bot);
    }

    const float vscroll_w = 12.0f;
    out_viewport = D2D1::RectF(sb_w, body_top, std::max(sb_w + 1.0f, client_w_dip_), body_bot);
    out_vscroll = D2D1::RectF(std::max(sb_w, client_w_dip_ - vscroll_w), body_top + 4.0f, client_w_dip_ - 2.0f, body_bot - 4.0f);
}

MainWindow::ToolbarHitRects MainWindow::ComputeToolbarButtons(const D2D1_RECT_F& toolbar) const {
    ToolbarHitRects r;
    const float cy = (toolbar.top + toolbar.bottom) * 0.5f;
    const float bh = 28.0f;
    const float y0 = cy - bh * 0.5f;
    const float y1 = cy + bh * 0.5f;

    float lx = 12.0f;
    r.icon_brand = D2D1::RectF(lx, cy - 10.0f, lx + 20.0f, cy + 10.0f);
    lx += 28.0f;
    r.btn_outline = D2D1::RectF(lx, y0, lx + 34.0f, y1);
    lx += 40.0f;
    r.btn_open = D2D1::RectF(lx, y0, lx + 56.0f, y1);
    lx += 62.0f;
    r.btn_edit = D2D1::RectF(lx, y0, lx + 56.0f, y1);

    const float cap_w = 42.0f;
    const float cap_bot = toolbar.bottom - 1.0f;
    r.btn_close = D2D1::RectF(toolbar.right - cap_w, toolbar.top, toolbar.right, cap_bot);
    r.btn_max = D2D1::RectF(r.btn_close.left - cap_w, toolbar.top, r.btn_close.left, cap_bot);
    r.btn_min = D2D1::RectF(r.btn_max.left - cap_w, toolbar.top, r.btn_max.left, cap_bot);

    float rx = r.btn_min.left - 16.0f;
    r.btn_theme = D2D1::RectF(rx - 74.0f, y0, rx, y1);
    rx -= 82.0f;

    r.btn_zoom_in = D2D1::RectF(rx - 28.0f, y0, rx, y1);
    rx -= 28.0f;
    r.btn_zoom_reset = D2D1::RectF(rx - 50.0f, y0, rx, y1);
    rx -= 50.0f;
    r.btn_zoom_out = D2D1::RectF(rx - 28.0f, y0, rx, y1);
    rx -= 36.0f;

    r.btn_find = D2D1::RectF(rx - 34.0f, y0, rx, y1);
    return r;
}

void MainWindow::OpenFile(const std::wstring& path, bool push_history) {
    std::string utf8;
    std::wstring err;
    if (!LoadTextFileUtf8(path, utf8, err)) {
        ShowToast(err);
        return;
    }

    if (push_history) {
        if (history_index_ >= 0 && history_index_ + 1 < static_cast<int>(history_.size())) {
            history_.erase(history_.begin() + history_index_ + 1, history_.end());
        }
        if (history_.empty() || _wcsicmp(history_.back().c_str(), path.c_str()) != 0) {
            history_.push_back(path);
            history_index_ = static_cast<int>(history_.size()) - 1;
        }
    }

    AddRecentFile(config_, path);
    SaveAppConfig(config_);
    watcher_.Watch(path);
    LoadMarkdownString(utf8, L"", path);
}

void MainWindow::ReloadCurrentFile(bool show_toast) {
    if (doc_.file_path.empty()) return;
    std::string utf8;
    std::wstring err;
    if (!LoadTextFileUtf8(doc_.file_path, utf8, err)) return;

    ScrollAnchor anchor = layout_.CaptureScrollAnchor(scroll_y_);
    int edit_top_line = edit_mode_ ? editor_.GetTopVisibleLine() : 0;
    source_utf8_ = utf8;
    doc_ = ParseMarkdownDocument(utf8, doc_.file_path);
    selection_.Clear();
    lightbox_image_idx_ = -1;
    RebuildLayout(false);
    if (edit_mode_) {
        editor_.SetDocument(source_utf8_);
        D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
        ComputeRegions(tb, sb, sp, vp, vs);
        editor_.EnsureLayout(vp, config_.zoom_percent, d2d_.DWrite(), GetThemePalette(config_.theme));
        editor_.ScrollToLineAtTop(edit_top_line);
    } else {
        scroll_y_ = target_scroll_y_ = std::clamp(layout_.ResolveScrollAnchor(anchor), 0.0f, MaxDocScrollY());
    }
    if (!find_query_.empty()) RunSearch(find_query_, false);
    UpdateWindowTitle();
    if (show_toast) ShowToast(L"文件已重新加载");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::LoadMarkdownString(const std::string& utf8, const std::wstring& virtual_title, const std::wstring& real_path) {
    edit_mode_ = false;
    source_utf8_ = utf8;
    doc_ = ParseMarkdownDocument(utf8, real_path);
    if (!virtual_title.empty() && real_path.empty()) {
        doc_.doc_title = virtual_title;
    }
    editor_.SetDocument(source_utf8_);
    editor_find_matches_.clear();
    selection_.Clear();
    lightbox_image_idx_ = -1;
    scroll_y_ = 0.0f;
    target_scroll_y_ = 0.0f;
    outline_scroll_y_ = 0.0f;
    hovered_link_url_.clear();

    RebuildLayout(false);
    if (find_visible_ && !find_query_.empty()) {
        RunSearch(find_query_, false);
    } else {
        search_matches_.clear();
        active_match_idx_ = -1;
    }
    UpdateImeEnablement();
    UpdateWindowTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateWindowTitle() {
    if (!hwnd_) return;
    std::wstring base = doc_.doc_title.empty() ? L"MarkReader" : (doc_.doc_title + L" — MarkReader");
    std::wstring title = (edit_mode_ && editor_.IsModified()) ? (L"• " + base) : base;
    SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::RebuildLayout(bool preserve_anchor) {
    if (autoscroll_active_) StopAutoScroll();
    dragging_hscroll_kind_ = HScrollKind::None;
    hovered_hscroll_kind_ = HScrollKind::None;
    ScrollAnchor anchor;
    if (preserve_anchor && !edit_mode_) {
        anchor = layout_.CaptureScrollAnchor(scroll_y_);
    }

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    float vp_w = std::max(240.0f, vp.right - vp.left);
    bool sb_vis = (sb.right > sb.left);

    layout_.Build(doc_, vp_w, config_.zoom_percent, sb_vis, GetThemePalette(config_.theme), images_);

    if (edit_mode_) {
        int top_line = preserve_anchor ? editor_.GetTopVisibleLine() : -1;
        editor_.EnsureLayout(vp, config_.zoom_percent, d2d_.DWrite(), GetThemePalette(config_.theme));
        if (top_line >= 0) {
            editor_.ScrollToLineAtTop(top_line);
        }
        if (find_visible_ && !find_query_.empty()) {
            editor_find_matches_ = editor_.FindAllMatches(find_query_);
            if (active_match_idx_ >= static_cast<int>(editor_find_matches_.size())) {
                active_match_idx_ = editor_find_matches_.empty() ? -1 : 0;
            }
        }
        return;
    }

    float new_max = MaxDocScrollY();
    if (preserve_anchor && anchor.kind != ScrollAnchor::Kind::None) {
        float resolved_y = layout_.ResolveScrollAnchor(anchor);
        scroll_y_ = target_scroll_y_ = std::clamp(resolved_y, 0.0f, new_max);
    } else {
        scroll_y_ = std::clamp(scroll_y_, 0.0f, new_max);
        target_scroll_y_ = std::clamp(target_scroll_y_, 0.0f, new_max);
    }
    if (find_visible_ && !find_query_.empty()) {
        search_matches_ = layout_.FindAllMatches(find_query_);
        if (active_match_idx_ >= static_cast<int>(search_matches_.size())) {
            active_match_idx_ = search_matches_.empty() ? -1 : 0;
        }
    }
}

float MainWindow::MaxDocScrollY() const {
    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    float vp_h = std::max(1.0f, vp.bottom - vp.top);
    return std::max(0.0f, layout_.TotalHeight() - vp_h);
}

float MainWindow::MaxOutlineScrollY() const {
    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    float avail_h = std::max(1.0f, (sb.bottom - sb.top) - kOutlineHeaderH - kOutlineBottomPad);
    float total_h = static_cast<float>(doc_.outline.size()) * kOutlineRowH + 12.0f;
    return std::max(0.0f, total_h - avail_h);
}

D2D1_RECT_F MainWindow::GetFindBarRect(const D2D1_RECT_F& viewport) const {
    float bar_w = std::min(360.0f, std::max(240.0f, (viewport.right - viewport.left) - 36.0f));
    float bar_h = 40.0f;
    return D2D1::RectF(
        viewport.right - bar_w - 20.0f,
        viewport.top + 10.0f,
        viewport.right - 20.0f,
        viewport.top + 10.0f + bar_h);
}

D2D1_RECT_F MainWindow::GetFindInputRect(const D2D1_RECT_F& bar) const {
    return D2D1::RectF(bar.left + 8.0f, bar.top + 6.0f, bar.right - 128.0f, bar.bottom - 6.0f);
}

void MainWindow::UpdateImeEnablement() {
    if (!hwnd_) return;
    const bool want_ime = (find_visible_ && find_focused_) || (edit_mode_ && !find_focused_);
    if (want_ime) {
        if (saved_himc_) ImmAssociateContext(hwnd_, saved_himc_);
        find_caret_visible_ = true;
        editor_caret_visible_ = true;
        SetTimer(hwnd_, kTimerCaret, 530, nullptr);
        UpdateImePosition();
    } else {
        ime_comp_.clear();
        KillTimer(hwnd_, kTimerCaret);
        if (saved_himc_) ImmAssociateContext(hwnd_, nullptr);
    }
}

void MainWindow::SetFindFocused(bool focused) {
    if (find_focused_ == focused) {
        if (focused) UpdateImePosition();
        return;
    }
    find_focused_ = focused;
    if (!focused) {
        ime_comp_.clear();
        find_sel_all_ = false;
    }
    UpdateImeEnablement();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateImePosition() {
    if (!hwnd_) return;
    if (find_visible_ && find_focused_) {
        HIMC himc = ImmGetContext(hwnd_);
        if (!himc) return;

        D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
        ComputeRegions(tb, sb, sp, vp, vs);
        D2D1_RECT_F bar = GetFindBarRect(vp);
        D2D1_RECT_F inp = GetFindInputRect(bar);

        float caret_x_dip = inp.left + 8.0f;
        std::wstring display = find_query_.substr(0, std::min(find_caret_, find_query_.size())) + ime_comp_;
        if (!display.empty() && d2d_.DWrite()) {
            if (auto tl = MakeUiText(d2d_.DWrite(), display, 13.0f, false, 4000.0f)) {
                DWRITE_TEXT_METRICS tm{};
                tl->GetMetrics(&tm);
                caret_x_dip = std::min(inp.right - 8.0f, inp.left + 8.0f + tm.widthIncludingTrailingWhitespace - find_scroll_x_);
            }
        }

        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = DipToPx(caret_x_dip);
        cf.ptCurrentPos.y = DipToPx(inp.bottom + 2.0f);
        ImmSetCompositionWindow(himc, &cf);

        LOGFONTW lf{};
        lf.lfHeight = -DipToPx(13.0f);
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        ImmSetCompositionFontW(himc, &lf);

        ImmReleaseContext(hwnd_, himc);
        return;
    }

    if (edit_mode_ && !find_focused_) {
        HIMC himc = ImmGetContext(hwnd_);
        if (!himc) return;

        D2D1_POINT_2F cp = editor_.GetCaretClientDIP();
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = DipToPx(cp.x);
        cf.ptCurrentPos.y = DipToPx(cp.y + 2.0f);
        ImmSetCompositionWindow(himc, &cf);

        float zscale = static_cast<float>(std::clamp(config_.zoom_percent, 50, 250)) / 100.0f;
        LOGFONTW lf{};
        lf.lfHeight = -DipToPx(14.5f * zscale);
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Consolas");
        ImmSetCompositionFontW(himc, &lf);

        ImmReleaseContext(hwnd_, himc);
    }
}

void MainWindow::ShowFindBar(bool show) {
    find_visible_ = show;
    if (show) {
        find_caret_ = find_query_.size();
        find_sel_all_ = !find_query_.empty();
        SetFindFocused(true);
    } else {
        SetFindFocused(false);
        search_matches_.clear();
        editor_find_matches_.clear();
        active_match_idx_ = -1;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnChar(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return;

    if (find_visible_ && find_focused_) {
        if (find_sel_all_) {
            find_query_.clear();
            find_caret_ = 0;
            find_sel_all_ = false;
        }
        find_caret_ = std::min(find_caret_, find_query_.size());
        find_query_.insert(find_caret_, 1, ch);
        ++find_caret_;
        find_caret_visible_ = true;
        RunSearch(find_query_, true);
        UpdateImePosition();
        return;
    }

    if (edit_mode_ && !find_focused_) {
        if (editor_.OnChar(ch)) {
            editor_caret_visible_ = true;
            if (find_visible_ && !find_query_.empty()) RunSearch(find_query_, false);
            UpdateWindowTitle();
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

void MainWindow::OnImeComposition(LPARAM lparam) {
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) return;

    if (lparam & GCS_COMPSTR) {
        if (find_visible_ && find_focused_ && find_sel_all_) {
            find_query_.clear();
            find_caret_ = 0;
            find_sel_all_ = false;
        }
        LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
        if (bytes > 0) {
            std::wstring comp(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(himc, GCS_COMPSTR, comp.data(), static_cast<DWORD>(bytes));
            ime_comp_ = std::move(comp);
        } else {
            ime_comp_.clear();
        }
        find_caret_visible_ = true;
        editor_caret_visible_ = true;
        UpdateImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (lparam & GCS_RESULTSTR) {
        ime_comp_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    ImmReleaseContext(hwnd_, himc);
}

void MainWindow::RunSearch(const std::wstring& query, bool jump_to_first) {
    find_query_ = query;
    if (edit_mode_) {
        editor_find_matches_ = editor_.FindAllMatches(query);
        if (editor_find_matches_.empty()) {
            active_match_idx_ = -1;
        } else if (jump_to_first) {
            int top_ln = editor_.GetTopVisibleLine();
            active_match_idx_ = 0;
            for (size_t i = 0; i < editor_find_matches_.size(); ++i) {
                if (editor_find_matches_[i].line >= top_ln) {
                    active_match_idx_ = static_cast<int>(i);
                    break;
                }
            }
            editor_.ScrollMatchIntoView(editor_find_matches_[active_match_idx_]);
        } else if (active_match_idx_ >= static_cast<int>(editor_find_matches_.size())) {
            active_match_idx_ = 0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    search_matches_ = layout_.FindAllMatches(query);
    if (search_matches_.empty()) {
        active_match_idx_ = -1;
    } else if (jump_to_first) {
        active_match_idx_ = 0;
        for (size_t i = 0; i < search_matches_.size(); ++i) {
            if (search_matches_[i].y >= scroll_y_) {
                active_match_idx_ = static_cast<int>(i);
                break;
            }
        }
        D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
        ComputeRegions(tb, sb, sp, vp, vs);
        float vp_h = vp.bottom - vp.top;
        ScrollToY(search_matches_[active_match_idx_].y - vp_h * 0.35f, true);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::JumpToMatch(int delta) {
    if (edit_mode_) {
        if (editor_find_matches_.empty()) return;
        int n = static_cast<int>(editor_find_matches_.size());
        if (active_match_idx_ < 0) active_match_idx_ = 0;
        else active_match_idx_ = (active_match_idx_ + delta % n + n) % n;
        editor_.ScrollMatchIntoView(editor_find_matches_[active_match_idx_]);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (search_matches_.empty()) return;
    int n = static_cast<int>(search_matches_.size());
    if (active_match_idx_ < 0) active_match_idx_ = 0;
    else active_match_idx_ = (active_match_idx_ + delta % n + n) % n;

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    float vp_h = vp.bottom - vp.top;
    ScrollToY(search_matches_[active_match_idx_].y - vp_h * 0.35f, true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::SaveCurrentDocument(bool show_toast) {
    if (!edit_mode_) return true;
    std::string updated = editor_.GetUtf8();
    if (doc_.file_path.empty()) {
        source_utf8_ = std::move(updated);
        editor_.MarkSaved();
        UpdateWindowTitle();
        if (show_toast) ShowToast(L"已更新文档缓存");
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    std::wstring err;
    if (!SaveTextFileUtf8(doc_.file_path, updated, err)) {
        ShowToast(err);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }

    source_utf8_ = std::move(updated);
    editor_.MarkSaved();
    watcher_.Watch(doc_.file_path);
    doc_ = ParseMarkdownDocument(source_utf8_, doc_.file_path);
    UpdateWindowTitle();
    if (show_toast) ShowToast(L"已保存");
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool MainWindow::ConfirmDiscardOrSaveBeforeLeave() {
    if (!edit_mode_ || !editor_.IsModified()) return true;
    if (doc_.file_path.empty()) {
        int res = MessageBoxW(
            hwnd_, L"当前示例文档有未保留的修改，是否仍要放弃这些修改？",
            L"MarkReader", MB_YESNO | MB_ICONQUESTION);
        return res == IDYES;
    }
    int res = MessageBoxW(
        hwnd_, L"当前文档有未保存的修改，是否先保存再继续？",
        L"MarkReader", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (res == IDCANCEL) return false;
    if (res == IDYES) return SaveCurrentDocument(false);
    return true;
}

void MainWindow::ToggleEditMode() {
    if (autoscroll_active_) StopAutoScroll();
    if (anim_timer_active_) {
        KillTimer(hwnd_, kTimerAnim);
        anim_timer_active_ = false;
    }
    dragging_selection_ = false;
    dragging_vscroll_ = false;
    dragging_hscroll_kind_ = HScrollKind::None;
    active_hscroll_idx_ = -1;

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);

    if (!edit_mode_) {
        int anchor_line = layout_.CaptureSourceLineAtY(scroll_y_);
        edit_mode_ = true;
        selection_.Clear();
        lightbox_image_idx_ = -1;
        hovered_link_url_.clear();

        editor_.SetDocument(source_utf8_);
        editor_.EnsureLayout(vp, config_.zoom_percent, d2d_.DWrite(), GetThemePalette(config_.theme));
        editor_.ScrollToLineAtTop(anchor_line);

        if (find_visible_ && !find_query_.empty()) {
            RunSearch(find_query_, false);
        }
        UpdateImeEnablement();
        UpdateWindowTitle();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    int top_line = editor_.GetTopVisibleLine();
    bool was_modified = editor_.IsModified();
    if (was_modified && !doc_.file_path.empty()) {
        if (!SaveCurrentDocument(false)) {
            return;
        }
    } else if (was_modified) {
        source_utf8_ = editor_.GetUtf8();
        editor_.MarkSaved();
    }

    std::wstring virtual_title = doc_.file_path.empty() ? doc_.doc_title : L"";
    doc_ = ParseMarkdownDocument(source_utf8_, doc_.file_path);
    if (!virtual_title.empty()) doc_.doc_title = virtual_title;

    edit_mode_ = false;
    editor_find_matches_.clear();
    ime_comp_.clear();
    selection_.Clear();
    hovered_link_url_.clear();

    RebuildLayout(false);
    float target_y = layout_.ResolveYForSourceLine(top_line);
    scroll_y_ = target_scroll_y_ = std::clamp(target_y, 0.0f, MaxDocScrollY());

    if (find_visible_ && !find_query_.empty()) {
        RunSearch(find_query_, false);
    } else {
        search_matches_.clear();
        active_match_idx_ = -1;
    }
    UpdateImeEnablement();
    UpdateWindowTitle();
    if (was_modified) {
        ShowToast(doc_.file_path.empty() ? L"已更新并返回阅读模式" : L"已保存并返回阅读模式");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Render() {
    auto* dc = d2d_.D2D();
    if (!dc) return;

    const ThemePalette& t = GetThemePalette(config_.theme);
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(t.text_primary, brush.Put());
    if (!brush) return;

    D2D1_RECT_F toolbar{}, sidebar{}, splitter{}, viewport{}, vscroll{};
    ComputeRegions(toolbar, sidebar, splitter, viewport, vscroll);

    if (!d2d_.BeginDraw()) return;

    dc->Clear(t.bg_canvas);

    // 1. Document Viewport (Read Mode OR Quick Source Edit Mode)
    if (edit_mode_) {
        editor_.EnsureLayout(viewport, config_.zoom_percent, d2d_.DWrite(), t);
        editor_.Render(
            dc, viewport, t, !find_focused_, editor_caret_visible_,
            find_focused_ ? L"" : ime_comp_, editor_find_matches_, active_match_idx_);

        D2D1_RECT_F thumb = editor_.ScrollbarThumbRect();
        if (thumb.bottom > thumb.top) {
            brush->SetColor(dragging_vscroll_ ? t.scrollbar_thumb_hover : t.scrollbar_thumb);
            dc->FillRoundedRectangle(D2D1::RoundedRect(thumb, 3.0f, 3.0f), brush.Get());
        }

        dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        RenderFindBar(dc, brush.Get(), t, viewport);
        RenderOverlays(dc, brush.Get(), t, viewport);
        dc->PopAxisAlignedClip();
    } else {
        dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(viewport.left, viewport.top - scroll_y_));

        ULONGLONG now = GetTickCount64();
        int active_copied_code = (now < copied_code_until_) ? copied_code_idx_ : -1;
        int active_copied_math = (now < copied_math_until_) ? copied_math_idx_ : -1;
        int active_code_hs = (dragging_hscroll_kind_ == HScrollKind::Code) ? active_hscroll_idx_
            : ((hovered_hscroll_kind_ == HScrollKind::Code) ? hovered_hscroll_idx_ : -1);
        int active_table_hs = (dragging_hscroll_kind_ == HScrollKind::Table) ? active_hscroll_idx_
            : ((hovered_hscroll_kind_ == HScrollKind::Table) ? hovered_hscroll_idx_ : -1);

        layout_.Render(
            dc, t, scroll_y_, viewport.bottom - viewport.top,
            selection_, search_matches_, active_match_idx_,
            hovered_copy_code_idx_, active_copied_code,
            hovered_copy_math_idx_, active_copied_math,
            active_code_hs, active_table_hs);

        dc->SetTransform(D2D1::Matrix3x2F::Identity());

        // Vertical overlay scrollbar
        float max_s = MaxDocScrollY();
        if (max_s > 1.0f) {
            float vp_h = viewport.bottom - viewport.top;
            float track_h = vscroll.bottom - vscroll.top;
            float thumb_h = std::max(32.0f, track_h * (vp_h / layout_.TotalHeight()));
            float thumb_y = vscroll.top + (track_h - thumb_h) * (scroll_y_ / max_s);
            D2D1_RECT_F thumb = D2D1::RectF(vscroll.right - 6.0f, thumb_y, vscroll.right, thumb_y + thumb_h);
            brush->SetColor(dragging_vscroll_ ? t.scrollbar_thumb_hover : t.scrollbar_thumb);
            dc->FillRoundedRectangle(D2D1::RoundedRect(thumb, 3.0f, 3.0f), brush.Get());
        }

        RenderFindBar(dc, brush.Get(), t, viewport);
        RenderOverlays(dc, brush.Get(), t, viewport);
        dc->PopAxisAlignedClip();
    }

    if (autoscroll_active_) {
        RenderAutoScrollIndicator(dc, brush.Get(), t);
    }

    // 2. Sidebar Outline + Splitter
    if (sidebar.right > sidebar.left) {
        RenderSidebar(dc, brush.Get(), t, sidebar);
        brush->SetColor(t.border);
        dc->FillRectangle(D2D1::RectF(splitter.left, splitter.top, splitter.left + 1.0f, splitter.bottom), brush.Get());
    }

    // 3. Top Toolbar
    RenderToolbar(dc, brush.Get(), t, toolbar);

    // 4. Image Lightbox Overlay
    if (lightbox_image_idx_ >= 0) {
        RenderLightbox(dc, brush.Get(), t);
    }

    d2d_.EndDraw();
}

void MainWindow::RenderToolbar(
    ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& toolbar) {
    auto* dw = d2d_.DWrite();
    auto round_stroke = MakeRoundStrokeStyle(d2d_.D2DFactory());

    brush->SetColor(t.bg_toolbar);
    dc->FillRectangle(toolbar, brush);

    brush->SetColor(t.border);
    dc->FillRectangle(D2D1::RectF(toolbar.left, toolbar.bottom - 1.0f, toolbar.right, toolbar.bottom), brush);

    ToolbarHitRects r = ComputeToolbarButtons(toolbar);

    // Compact M + underline software mark on merged title/toolbar
    DrawAppBrandIcon(dc, d2d_.D2DFactory(), brush, round_stroke.Get(), r.icon_brand);

    // Sidebar toggle vector icon (window with left third shaded)
    DrawButtonBox(dc, brush, r.btn_outline, hovered_toolbar_btn_ == BTN_OUTLINE, config_.outline_visible, t);
    DrawSidebarIcon(dc, brush, r.btn_outline,
                    config_.outline_visible ? t.accent : t.text_primary,
                    config_.outline_visible);

    // Open button
    brush->SetColor(t.bg_elevated);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r.btn_open, 6.0f, 6.0f), brush);
    brush->SetColor(t.border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r.btn_open, 6.0f, 6.0f), brush, 1.0f);
    DrawButtonBox(dc, brush, r.btn_open, hovered_toolbar_btn_ == BTN_OPEN, false, t);
    DrawToolbarText(dc, brush, dw, r.btn_open, L"打开", 12.5f, true, t.text_primary);

    // Edit / Done toggle button
    if (edit_mode_) {
        brush->SetColor(t.accent);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r.btn_edit, 6.0f, 6.0f), brush);
        if (hovered_toolbar_btn_ == BTN_EDIT) {
            brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r.btn_edit, 6.0f, 6.0f), brush);
        }
        DrawToolbarText(dc, brush, dw, r.btn_edit, L"完成", 12.5f, true, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    } else {
        brush->SetColor(t.bg_elevated);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r.btn_edit, 6.0f, 6.0f), brush);
        brush->SetColor(t.border);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r.btn_edit, 6.0f, 6.0f), brush, 1.0f);
        DrawButtonBox(dc, brush, r.btn_edit, hovered_toolbar_btn_ == BTN_EDIT, false, t);
        DrawToolbarText(dc, brush, dw, r.btn_edit, L"编辑", 12.5f, true, t.text_primary);
    }

    // Document title centered in the window horizontally with crisp foreground ink & normal size
    float safe_x0 = r.btn_edit.right + 16.0f;
    float safe_x1 = r.btn_find.left - 16.0f;
    std::wstring disp_title = doc_.doc_title;
    if (edit_mode_ && editor_.IsModified() && !disp_title.empty()) {
        disp_title = L"\u2022 " + disp_title;
    }
    if (safe_x1 > safe_x0 + 48.0f && !disp_title.empty()) {
        float win_cx = (toolbar.left + toolbar.right) * 0.5f;
        float half_sym = std::max(0.0f, std::min(win_cx - safe_x0, safe_x1 - win_cx));
        float sym_w = half_sym * 2.0f;

        float natural_w = 0.0f;
        if (auto measure = MakeUiText(dw, disp_title, 14.0f, true, 4000.0f)) {
            DWRITE_TEXT_METRICS mtm{};
            measure->GetMetrics(&mtm);
            natural_w = mtm.widthIncludingTrailingWhitespace;
        }

        float box_x = safe_x0;
        float box_w = safe_x1 - safe_x0;
        if (natural_w > 0.0f && natural_w <= sym_w) {
            box_x = win_cx - half_sym;
            box_w = sym_w;
        }
        if (auto tl = MakeUiText(dw, disp_title, 14.0f, true, box_w, DWRITE_TEXT_ALIGNMENT_CENTER, true)) {
            DWRITE_TEXT_METRICS tm{};
            tl->GetMetrics(&tm);
            float ty = toolbar.top + ((toolbar.bottom - toolbar.top) - tm.height) * 0.5f;
            brush->SetColor(t.text_primary);
            dc->DrawTextLayout(D2D1::Point2F(box_x, ty), tl.Get(), brush);
        }
    }

    // Vector Search icon
    DrawButtonBox(dc, brush, r.btn_find, hovered_toolbar_btn_ == BTN_FIND, find_visible_, t);
    DrawSearchIcon(dc, brush, round_stroke.Get(), r.btn_find,
                   find_visible_ ? t.accent : t.text_primary);

    // Zoom segmented control (single rounded pill sliced into 3 flat-edged segments)
    D2D1_RECT_F zoom_pill = D2D1::RectF(r.btn_zoom_out.left, r.btn_zoom_out.top, r.btn_zoom_in.right, r.btn_zoom_in.bottom);
    brush->SetColor(t.bg_elevated);
    dc->FillRoundedRectangle(D2D1::RoundedRect(zoom_pill, 6.0f, 6.0f), brush);

    bool hover_out = (hovered_toolbar_btn_ == BTN_ZOOM_OUT);
    bool hover_rst = (hovered_toolbar_btn_ == BTN_ZOOM_RESET);
    bool hover_in = (hovered_toolbar_btn_ == BTN_ZOOM_IN);
    if (hover_out || hover_rst || hover_in) {
        ComPtr<ID2D1RoundedRectangleGeometry> pill_geom;
        if (auto* fac = d2d_.D2DFactory(); fac && SUCCEEDED(fac->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(zoom_pill, 6.0f, 6.0f), pill_geom.Put())) && pill_geom) {
            dc->PushLayer(D2D1::LayerParameters(zoom_pill, pill_geom.Get()), nullptr);
            brush->SetColor(t.bg_hover);
            if (hover_out) dc->FillRectangle(r.btn_zoom_out, brush);
            else if (hover_rst) dc->FillRectangle(r.btn_zoom_reset, brush);
            else if (hover_in) dc->FillRectangle(r.btn_zoom_in, brush);
            dc->PopLayer();
        }
    }

    // Crisp right-angle segment dividers + outer rounded pill outline
    brush->SetColor(t.border);
    dc->FillRectangle(D2D1::RectF(r.btn_zoom_out.right - 0.5f, zoom_pill.top, r.btn_zoom_out.right + 0.5f, zoom_pill.bottom), brush);
    dc->FillRectangle(D2D1::RectF(r.btn_zoom_reset.right - 0.5f, zoom_pill.top, r.btn_zoom_reset.right + 0.5f, zoom_pill.bottom), brush);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(zoom_pill, 6.0f, 6.0f), brush, 1.0f);

    // Vector minus centered in left slice
    {
        float mcx = (r.btn_zoom_out.left + r.btn_zoom_out.right) * 0.5f;
        float mcy = (r.btn_zoom_out.top + r.btn_zoom_out.bottom) * 0.5f;
        brush->SetColor(t.text_primary);
        dc->DrawLine(D2D1::Point2F(mcx - 4.0f, mcy), D2D1::Point2F(mcx + 4.0f, mcy), brush, 1.35f, round_stroke.Get());
    }

    DrawToolbarText(dc, brush, dw, r.btn_zoom_reset, std::to_wstring(config_.zoom_percent) + L"%", 12.0f, true, t.text_primary);

    // Vector plus centered in right slice
    {
        float pcx = (r.btn_zoom_in.left + r.btn_zoom_in.right) * 0.5f;
        float pcy = (r.btn_zoom_in.top + r.btn_zoom_in.bottom) * 0.5f;
        brush->SetColor(t.text_primary);
        dc->DrawLine(D2D1::Point2F(pcx - 4.0f, pcy), D2D1::Point2F(pcx + 4.0f, pcy), brush, 1.35f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(pcx, pcy - 4.0f), D2D1::Point2F(pcx, pcy + 4.0f), brush, 1.35f, round_stroke.Get());
    }

    // Theme button with vector icon + label
    brush->SetColor(t.bg_elevated);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r.btn_theme, 6.0f, 6.0f), brush);
    brush->SetColor(t.border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r.btn_theme, 6.0f, 6.0f), brush, 1.0f);
    DrawButtonBox(dc, brush, r.btn_theme, hovered_toolbar_btn_ == BTN_THEME, false, t, 6.0f);

    float icon_cx = r.btn_theme.left + 17.0f;
    float icon_cy = (r.btn_theme.top + r.btn_theme.bottom) * 0.5f;
    DrawThemeIcon(dc, d2d_.D2DFactory(), brush, round_stroke.Get(), icon_cx, icon_cy, config_.theme, t.text_primary);

    const wchar_t* theme_label = L"亮色";
    if (config_.theme == ThemeMode::Dark) theme_label = L"暗色";
    else if (config_.theme == ThemeMode::Sepia) theme_label = L"护眼";
    D2D1_RECT_F text_r = D2D1::RectF(r.btn_theme.left + 26.0f, r.btn_theme.top, r.btn_theme.right - 6.0f, r.btn_theme.bottom);
    DrawToolbarText(dc, brush, dw, text_r, theme_label, 12.0f, false, t.text_primary);

    // Subtle vertical divider before native caption buttons
    float div_x = r.btn_min.left - 8.0f;
    float div_cy = (toolbar.top + toolbar.bottom) * 0.5f;
    brush->SetColor(t.border);
    dc->FillRectangle(D2D1::RectF(div_x, div_cy - 9.0f, div_x + 1.0f, div_cy + 9.0f), brush);

    // Window caption buttons (Minimize, Maximize/Restore, Close)
    const bool zoomed = IsZoomed(hwnd_) != FALSE;
    if (hovered_toolbar_btn_ == BTN_MIN) {
        brush->SetColor(t.bg_hover);
        dc->FillRectangle(r.btn_min, brush);
    }
    DrawCaptionIcon(dc, brush, round_stroke.Get(), r.btn_min, BTN_MIN, zoomed, t.text_primary);

    if (hovered_toolbar_btn_ == BTN_MAX) {
        brush->SetColor(t.bg_hover);
        dc->FillRectangle(r.btn_max, brush);
    }
    DrawCaptionIcon(dc, brush, round_stroke.Get(), r.btn_max, BTN_MAX, zoomed, t.text_primary);

    bool close_hot = (hovered_toolbar_btn_ == BTN_CLOSE);
    if (close_hot) {
        brush->SetColor(D2D1::ColorF(0xC42B1C));
        dc->FillRectangle(r.btn_close, brush);
    }
    DrawCaptionIcon(dc, brush, round_stroke.Get(), r.btn_close, BTN_CLOSE, zoomed,
                    close_hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : t.text_primary);
}

void MainWindow::RenderSidebar(
    ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& sidebar) {
    auto* dw = d2d_.DWrite();
    brush->SetColor(t.bg_sidebar);
    dc->FillRectangle(sidebar, brush);

    D2D1_RECT_F hdr = D2D1::RectF(sidebar.left, sidebar.top, sidebar.right, sidebar.top + kOutlineHeaderH);
    if (auto tl = MakeUiText(dw, L"大纲目录", 12.5f, true, (hdr.right - hdr.left) - 64.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true)) {
        brush->SetColor(t.text_secondary);
        dc->DrawTextLayout(D2D1::Point2F(hdr.left + 16.0f, hdr.top + 13.0f), tl.Get(), brush);
    }
    if (auto cl = MakeUiText(dw, std::to_wstring(doc_.outline.size()), 11.5f, false, 48.0f, DWRITE_TEXT_ALIGNMENT_TRAILING)) {
        brush->SetColor(t.text_muted);
        dc->DrawTextLayout(D2D1::Point2F(hdr.right - 62.0f, hdr.top + 14.0f), cl.Get(), brush);
    }
    brush->SetColor(t.border);
    dc->FillRectangle(D2D1::RectF(hdr.left + 12.0f, hdr.bottom - 1.0f, hdr.right - 12.0f, hdr.bottom), brush);

    D2D1_RECT_F list_rect = D2D1::RectF(
        sidebar.left, hdr.bottom, sidebar.right, std::max(hdr.bottom + 1.0f, sidebar.bottom - kOutlineBottomPad));
    dc->PushAxisAlignedClip(list_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (doc_.outline.empty()) {
        if (auto el = MakeUiText(dw, L"当前文档没有标题", 12.5f, false, (list_rect.right - list_rect.left) - 32.0f, DWRITE_TEXT_ALIGNMENT_CENTER)) {
            brush->SetColor(t.text_muted);
            dc->DrawTextLayout(D2D1::Point2F(list_rect.left + 16.0f, list_rect.top + 28.0f), el.Get(), brush);
        }
    } else {
        int min_level = 6;
        for (const auto& entry : doc_.outline) min_level = std::min(min_level, entry.level);
        int active_id = layout_.FindActiveHeadingId(target_scroll_y_);
        if (edit_mode_) {
            int top_ln = editor_.GetTopVisibleLine();
            active_id = doc_.outline.front().heading_id;
            for (const auto& entry : doc_.outline) {
                if (entry.source_line <= top_ln + 1) active_id = entry.heading_id;
                else break;
            }
        }

        for (size_t i = 0; i < doc_.outline.size(); ++i) {
            const auto& entry = doc_.outline[i];
            float ry = list_rect.top + 6.0f + static_cast<float>(i) * kOutlineRowH - outline_scroll_y_;
            if (ry + kOutlineRowH < list_rect.top || ry > list_rect.bottom) continue;

            D2D1_RECT_F row_r = D2D1::RectF(list_rect.left + 8.0f, ry + 1.0f, list_rect.right - 8.0f, ry + kOutlineRowH - 1.0f);
            bool is_active = (entry.heading_id == active_id);
            bool is_hover = (static_cast<int>(i) == hovered_outline_idx_);
            if (is_active) {
                brush->SetColor(t.bg_active_item);
                dc->FillRoundedRectangle(D2D1::RoundedRect(row_r, 6.0f, 6.0f), brush);
                brush->SetColor(t.accent);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(row_r.left, row_r.top + 5.0f, row_r.left + 3.0f, row_r.bottom - 5.0f), 1.5f, 1.5f), brush);
            } else if (is_hover) {
                brush->SetColor(t.bg_hover);
                dc->FillRoundedRectangle(D2D1::RoundedRect(row_r, 6.0f, 6.0f), brush);
            }

            float indent = static_cast<float>(std::max(0, entry.level - min_level)) * 13.0f;
            float tx = row_r.left + 11.0f + indent;
            float tw = std::max(24.0f, row_r.right - tx - 8.0f);
            float fsize = (entry.level <= min_level) ? 13.0f : 12.4f;
            bool bold = is_active || (entry.level <= min_level);

            if (auto tl = MakeUiText(dw, entry.title, fsize, bold, tw, DWRITE_TEXT_ALIGNMENT_LEADING, true)) {
                DWRITE_TEXT_METRICS tm{};
                tl->GetMetrics(&tm);
                float ty = row_r.top + ((row_r.bottom - row_r.top) - tm.height) * 0.5f;
                brush->SetColor(is_active ? t.accent : (entry.level <= min_level + 1 ? t.text_primary : t.text_secondary));
                dc->DrawTextLayout(D2D1::Point2F(tx, ty), tl.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
        }
    }
    dc->PopAxisAlignedClip();
}

void MainWindow::RenderFindBar(
    ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& viewport) {
    if (!find_visible_) return;
    auto* dw = d2d_.DWrite();
    D2D1_RECT_F bar = GetFindBarRect(viewport);
    D2D1_RECT_F inp = GetFindInputRect(bar);

    // Outer floating card
    brush->SetColor(t.bg_elevated);
    dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 8.0f, 8.0f), brush);
    brush->SetColor(t.border_strong);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(bar, 8.0f, 8.0f), brush, 1.0f);

    // Inner input field
    brush->SetColor(t.bg_canvas);
    dc->FillRoundedRectangle(D2D1::RoundedRect(inp, 5.0f, 5.0f), brush);
    brush->SetColor(find_focused_ ? t.accent : t.border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(inp, 5.0f, 5.0f), brush, find_focused_ ? 1.4f : 1.0f);

    // Build displayed string = prefix + ime_comp + suffix
    const size_t caret_pos = std::min(find_caret_, find_query_.size());
    const std::wstring& active_comp = find_focused_ ? ime_comp_ : std::wstring();
    std::wstring full_text = find_query_.substr(0, caret_pos) + active_comp + find_query_.substr(caret_pos);

    const float pad_x = 8.0f;
    const float inner_w = std::max(20.0f, (inp.right - inp.left) - 2.0f * pad_x);
    const float text_y = inp.top + 5.0f;

    dc->PushAxisAlignedClip(
        D2D1::RectF(inp.left + 3.0f, inp.top + 2.0f, inp.right - 3.0f, inp.bottom - 2.0f),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (full_text.empty()) {
        if (auto ph = MakeUiText(dw, L"搜索文档...", 12.8f, false, inner_w, DWRITE_TEXT_ALIGNMENT_LEADING, true)) {
            brush->SetColor(t.text_muted);
            dc->DrawTextLayout(D2D1::Point2F(inp.left + pad_x, text_y), ph.Get(), brush);
        }
        if (find_focused_ && find_caret_visible_) {
            brush->SetColor(t.accent);
            dc->DrawLine(
                D2D1::Point2F(inp.left + pad_x, inp.top + 6.0f),
                D2D1::Point2F(inp.left + pad_x, inp.bottom - 6.0f),
                brush, 1.5f);
        }
    } else if (auto tl = MakeUiText(dw, full_text, 12.8f, false, 4000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false)) {
        if (!active_comp.empty()) {
            tl->SetUnderline(TRUE, {static_cast<UINT32>(caret_pos), static_cast<UINT32>(active_comp.size())});
        }
        const UINT32 visual_caret = static_cast<UINT32>(caret_pos + active_comp.size());
        float cx = 0.0f, cy = 0.0f;
        DWRITE_HIT_TEST_METRICS htm{};
        tl->HitTestTextPosition(visual_caret, FALSE, &cx, &cy, &htm);

        // Keep caret smoothly within horizontal input viewport
        if (cx - find_scroll_x_ > inner_w - 2.0f) {
            find_scroll_x_ = cx - (inner_w - 2.0f);
        } else if (cx - find_scroll_x_ < 0.0f) {
            find_scroll_x_ = std::max(0.0f, cx - 12.0f);
        }

        const float draw_x = inp.left + pad_x - find_scroll_x_;

        if (find_sel_all_ && active_comp.empty()) {
            DWRITE_TEXT_METRICS tm{};
            tl->GetMetrics(&tm);
            brush->SetColor(t.selection_bg);
            dc->FillRectangle(
                D2D1::RectF(draw_x, inp.top + 5.0f, draw_x + tm.widthIncludingTrailingWhitespace, inp.bottom - 5.0f),
                brush);
        }

        brush->SetColor(t.text_primary);
        dc->DrawTextLayout(D2D1::Point2F(draw_x, text_y), tl.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

        if (find_focused_ && find_caret_visible_) {
            float caret_screen_x = draw_x + cx;
            brush->SetColor(t.accent);
            dc->DrawLine(
                D2D1::Point2F(caret_screen_x, inp.top + 6.0f),
                D2D1::Point2F(caret_screen_x, inp.bottom - 6.0f),
                brush, 1.5f);
        }
    }
    dc->PopAxisAlignedClip();

    // Match counter
    const size_t total_matches = edit_mode_ ? editor_find_matches_.size() : search_matches_.size();
    std::wstring cnt;
    if (!find_query_.empty()) {
        if (total_matches == 0) {
            cnt = L"0 / 0";
        } else {
            cnt = std::to_wstring(active_match_idx_ + 1) + L" / " + std::to_wstring(total_matches);
        }
    }
    D2D1_RECT_F cnt_r = D2D1::RectF(bar.right - 126.0f, bar.top + 5.0f, bar.right - 76.0f, bar.bottom - 5.0f);
    if (!cnt.empty()) {
        DrawToolbarText(dc, brush, dw, cnt_r, cnt, 11.5f, false,
                        total_matches == 0 ? t.adm_caution_accent : t.text_secondary);
    }

    D2D1_RECT_F prev_r = D2D1::RectF(bar.right - 74.0f, bar.top + 6.0f, bar.right - 52.0f, bar.bottom - 6.0f);
    D2D1_RECT_F next_r = D2D1::RectF(bar.right - 50.0f, bar.top + 6.0f, bar.right - 28.0f, bar.bottom - 6.0f);
    D2D1_RECT_F close_r = D2D1::RectF(bar.right - 26.0f, bar.top + 6.0f, bar.right - 4.0f, bar.bottom - 6.0f);

    DrawButtonBox(dc, brush, prev_r, hovered_find_btn_ == FBTN_PREV, false, t, 4.0f);
    DrawToolbarText(dc, brush, dw, prev_r, L"\u2191", 12.5f, false, t.text_primary);

    DrawButtonBox(dc, brush, next_r, hovered_find_btn_ == FBTN_NEXT, false, t, 4.0f);
    DrawToolbarText(dc, brush, dw, next_r, L"\u2193", 12.5f, false, t.text_primary);

    DrawButtonBox(dc, brush, close_r, hovered_find_btn_ == FBTN_CLOSE, false, t, 4.0f);
    DrawToolbarText(dc, brush, dw, close_r, L"\u2715", 11.5f, false, t.text_secondary);
}

void MainWindow::RenderOverlays(
    ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t, const D2D1_RECT_F& viewport) {
    auto* dw = d2d_.DWrite();

    if (!hovered_link_url_.empty()) {
        float w = std::min(460.0f, (viewport.right - viewport.left) - 40.0f);
        D2D1_RECT_F pill = D2D1::RectF(viewport.left + 12.0f, viewport.bottom - 32.0f, viewport.left + 12.0f + w, viewport.bottom - 8.0f);
        if (auto tl = MakeUiText(dw, hovered_link_url_, 11.5f, false, w - 18.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true)) {
            DWRITE_TEXT_METRICS tm{};
            tl->GetMetrics(&tm);
            pill.right = std::min(pill.right, pill.left + tm.widthIncludingTrailingWhitespace + 20.0f);
            brush->SetColor(t.bg_elevated);
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 6.0f, 6.0f), brush);
            brush->SetColor(t.border);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, 6.0f, 6.0f), brush, 1.0f);
            brush->SetColor(t.text_secondary);
            dc->DrawTextLayout(D2D1::Point2F(pill.left + 9.0f, pill.top + 4.0f), tl.Get(), brush);
        }
    }

    if (!toast_text_.empty() && GetTickCount64() < toast_until_) {
        if (auto tl = MakeUiText(dw, toast_text_, 12.5f, true, 380.0f, DWRITE_TEXT_ALIGNMENT_CENTER, true)) {
            DWRITE_TEXT_METRICS tm{};
            tl->GetMetrics(&tm);
            float pw = std::max(110.0f, tm.widthIncludingTrailingWhitespace + 32.0f);
            tl->SetMaxWidth(pw);
            float cx = (viewport.left + viewport.right) * 0.5f;
            D2D1_RECT_F pill = D2D1::RectF(cx - pw * 0.5f, viewport.bottom - 48.0f, cx + pw * 0.5f, viewport.bottom - 16.0f);
            brush->SetColor(t.bg_elevated);
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 16.0f, 16.0f), brush);
            brush->SetColor(t.accent);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, 16.0f, 16.0f), brush, 1.0f);
            brush->SetColor(t.text_primary);
            dc->DrawTextLayout(D2D1::Point2F(pill.left, pill.top + (32.0f - tm.height) * 0.5f), tl.Get(), brush);
        }
    }
}

void MainWindow::RenderLightbox(ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette&) {
    const auto& imgs = layout_.Images();
    if (lightbox_image_idx_ < 0 || lightbox_image_idx_ >= static_cast<int>(imgs.size())) return;
    const auto& item = imgs[lightbox_image_idx_];
    if (!item.image || item.image->status != LoadedImage::Status::Ready) return;

    brush->SetColor(D2D1::ColorF(0.03f, 0.04f, 0.06f, 0.86f));
    dc->FillRectangle(D2D1::RectF(0, 0, client_w_dip_, client_h_dip_), brush);

    float avail_w = std::max(80.0f, client_w_dip_ * 0.88f);
    float avail_h = std::max(80.0f, client_h_dip_ * 0.82f);
    float nw = std::max(1.0f, item.image->width);
    float nh = std::max(1.0f, item.image->height);
    float scale = std::min(avail_w / nw, avail_h / nh);
    if (!item.image->is_svg) scale = std::min(scale, 2.5f);
    float dw = nw * scale;
    float dh = nh * scale;
    D2D1_RECT_F dest = D2D1::RectF(
        (client_w_dip_ - dw) * 0.5f,
        (client_h_dip_ - dh) * 0.5f - 10.0f,
        (client_w_dip_ + dw) * 0.5f,
        (client_h_dip_ + dh) * 0.5f - 10.0f);

    DrawLoadedImage(dc, *item.image, dest);

    std::wstring hint = item.alt.empty() ? L"按 Esc 或点击任意位置关闭预览" : (item.alt + L"  ·  按 Esc 或点击关闭");
    if (auto tl = MakeUiText(d2d_.DWrite(), hint, 13.0f, false, client_w_dip_ - 40.0f, DWRITE_TEXT_ALIGNMENT_CENTER, true)) {
        brush->SetColor(D2D1::ColorF(0.9f, 0.92f, 0.95f, 0.9f));
        dc->DrawTextLayout(D2D1::Point2F(20.0f, dest.bottom + 14.0f), tl.Get(), brush);
    }
}

void MainWindow::OnSize(UINT width_px, UINT height_px) {
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    if (dpi_ <= 0.0f) dpi_ = 96.0f;
    client_w_dip_ = PxToDip(static_cast<int>(width_px));
    client_h_dip_ = PxToDip(static_cast<int>(height_px));

    d2d_.Resize(width_px, height_px, dpi_);
    RebuildLayout(true);
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDpiChanged(UINT new_dpi, const RECT* suggested_rect) {
    dpi_ = static_cast<float>(new_dpi);
    if (suggested_rect) {
        SetWindowPos(
            hwnd_, nullptr,
            suggested_rect->left, suggested_rect->top,
            suggested_rect->right - suggested_rect->left,
            suggested_rect->bottom - suggested_rect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    UpdateDwmAppearance();
    RebuildLayout(true);
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnMouseMove(float x_dip, float y_dip, WPARAM wp) {
    if (!tracking_mouse_leave_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        if (TrackMouseEvent(&tme)) tracking_mouse_leave_ = true;
    }
    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);

    if (dragging_splitter_) {
        config_.outline_width = std::clamp(x_dip, 180.0f, std::min(440.0f, client_w_dip_ * 0.42f));
        RebuildLayout(true);
        UpdateImePosition();
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (dragging_vscroll_) {
        float vp_h = vp.bottom - vp.top;
        float track_h = vs.bottom - vs.top;
        float total_h = edit_mode_ ? editor_.TotalHeight() : layout_.TotalHeight();
        float max_s = edit_mode_ ? editor_.MaxScrollY() : MaxDocScrollY();
        float thumb_h = std::max(32.0f, track_h * (vp_h / std::max(1.0f, total_h)));
        float denom = std::max(1.0f, track_h - thumb_h);
        float ratio = std::clamp((y_dip - vs.top - vscroll_drag_offset_) / denom, 0.0f, 1.0f);
        if (edit_mode_) {
            editor_.SetScrollY(ratio * max_s);
            UpdateImePosition();
        } else {
            scroll_y_ = target_scroll_y_ = ratio * max_s;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (dragging_hscroll_kind_ != HScrollKind::None && active_hscroll_idx_ >= 0) {
        UpdateHScrollDrag(x_dip);
        return;
    }

    if (autoscroll_active_) {
        autoscroll_cur_x_ = x_dip;
        autoscroll_cur_y_ = y_dip;
        if (std::hypot(x_dip - autoscroll_anchor_x_, y_dip - autoscroll_anchor_y_) > 6.0f) {
            autoscroll_moved_ = true;
        }
        SetCursor(LoadCursorW(nullptr, autoscroll_h_kind_ != HScrollKind::None ? IDC_SIZEWE : IDC_SIZENS));
        return;
    }

    if (edit_mode_ && (wp & MK_LBUTTON) && GetCapture() == hwnd_) {
        if (y_dip < vp.top + 16.0f) {
            editor_.SetScrollY(editor_.ScrollY() - 24.0f);
        } else if (y_dip > vp.bottom - 16.0f) {
            editor_.SetScrollY(editor_.ScrollY() + 24.0f);
        }
        if (editor_.OnMouseMove(x_dip, y_dip)) {
            editor_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return;
    }

    if (dragging_selection_ && (wp & MK_LBUTTON)) {
        if (y_dip < vp.top + 16.0f) {
            ScrollToY(scroll_y_ - 24.0f, false);
        } else if (y_dip > vp.bottom - 16.0f) {
            ScrollToY(scroll_y_ + 24.0f, false);
        }
        int sid = -1;
        uint32_t pos = 0;
        if (layout_.HitTestSelectableNearest(x_dip - vp.left, y_dip - vp.top + scroll_y_, sid, pos)) {
            selection_.end_sel = sid;
            selection_.end_pos = pos;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return;
    }

    if (lightbox_image_idx_ >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }

    int old_tb = hovered_toolbar_btn_;
    int old_ol = hovered_outline_idx_;
    int old_cc = hovered_copy_code_idx_;
    int old_cm = hovered_copy_math_idx_;
    int old_fb = hovered_find_btn_;
    HScrollKind old_hsk = hovered_hscroll_kind_;
    int old_hsi = hovered_hscroll_idx_;
    std::wstring old_link = hovered_link_url_;

    hovered_toolbar_btn_ = -1;
    hovered_outline_idx_ = -1;
    hovered_copy_code_idx_ = -1;
    hovered_copy_math_idx_ = -1;
    hovered_find_btn_ = -1;
    hovered_hscroll_kind_ = HScrollKind::None;
    hovered_hscroll_idx_ = -1;
    hovered_link_url_.clear();

    LPCWSTR cursor = IDC_ARROW;

    if (RectContains(tb, x_dip, y_dip)) {
        ToolbarHitRects tr = ComputeToolbarButtons(tb);
        if (RectContains(tr.btn_outline, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_OUTLINE; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_open, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_OPEN; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_edit, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_EDIT; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_find, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_FIND; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_zoom_out, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_ZOOM_OUT; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_zoom_reset, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_ZOOM_RESET; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_zoom_in, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_ZOOM_IN; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_theme, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_THEME; cursor = IDC_HAND; }
        else if (RectContains(tr.btn_min, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_MIN; cursor = IDC_ARROW; }
        else if (RectContains(tr.btn_max, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_MAX; cursor = IDC_ARROW; }
        else if (RectContains(tr.btn_close, x_dip, y_dip)) { hovered_toolbar_btn_ = BTN_CLOSE; cursor = IDC_ARROW; }
    } else if (sb.right > sb.left && RectContains(sp, x_dip, y_dip)) {
        cursor = IDC_SIZEWE;
    } else if (sb.right > sb.left && RectContains(sb, x_dip, y_dip)) {
        float list_top = sb.top + kOutlineHeaderH;
        float list_bot = sb.bottom - kOutlineBottomPad;
        if (y_dip >= list_top && y_dip <= list_bot && !doc_.outline.empty()) {
            int idx = static_cast<int>((y_dip - (list_top + 6.0f) + outline_scroll_y_) / kOutlineRowH);
            if (idx >= 0 && idx < static_cast<int>(doc_.outline.size())) {
                hovered_outline_idx_ = idx;
                cursor = IDC_HAND;
            }
        }
    } else if (find_visible_ && RectContains(GetFindBarRect(vp), x_dip, y_dip)) {
        D2D1_RECT_F bar = GetFindBarRect(vp);
        D2D1_RECT_F inp = GetFindInputRect(bar);
        D2D1_RECT_F prev_r = D2D1::RectF(bar.right - 74.0f, bar.top + 6.0f, bar.right - 52.0f, bar.bottom - 6.0f);
        D2D1_RECT_F next_r = D2D1::RectF(bar.right - 50.0f, bar.top + 6.0f, bar.right - 28.0f, bar.bottom - 6.0f);
        D2D1_RECT_F close_r = D2D1::RectF(bar.right - 26.0f, bar.top + 6.0f, bar.right - 4.0f, bar.bottom - 6.0f);
        if (RectContains(inp, x_dip, y_dip)) { cursor = IDC_IBEAM; }
        else if (RectContains(prev_r, x_dip, y_dip)) { hovered_find_btn_ = FBTN_PREV; cursor = IDC_HAND; }
        else if (RectContains(next_r, x_dip, y_dip)) { hovered_find_btn_ = FBTN_NEXT; cursor = IDC_HAND; }
        else if (RectContains(close_r, x_dip, y_dip)) { hovered_find_btn_ = FBTN_CLOSE; cursor = IDC_HAND; }
    } else if (RectContains(vp, x_dip, y_dip)) {
        if (edit_mode_) {
            if (editor_.MaxScrollY() > 1.0f && x_dip >= vs.left - 4.0f && x_dip <= vs.right + 4.0f) {
                cursor = IDC_ARROW;
            } else {
                cursor = IDC_IBEAM;
            }
        } else {
            float doc_x = x_dip - vp.left;
            float doc_y = y_dip - vp.top + scroll_y_;

            const auto& maths = layout_.MathBlocks();
            for (size_t i = 0; i < maths.size(); ++i) {
                if (RectContains(maths[i].card_rect, doc_x, doc_y)) {
                    hovered_copy_math_idx_ = static_cast<int>(i);
                    break;
                }
            }

            HitTestResult hr = layout_.HitTest(doc_x, doc_y);
            if (hr.target == HitTestResult::Target::CopyCodeBtn) {
                hovered_copy_code_idx_ = hr.item_index;
                cursor = IDC_HAND;
            } else if (hr.target == HitTestResult::Target::CopyMathBtn) {
                hovered_copy_math_idx_ = hr.item_index;
                cursor = IDC_HAND;
            } else if (hr.target == HitTestResult::Target::CodeHScroll) {
                hovered_hscroll_kind_ = HScrollKind::Code;
                hovered_hscroll_idx_ = hr.item_index;
                cursor = IDC_HAND;
            } else if (hr.target == HitTestResult::Target::TableHScroll) {
                hovered_hscroll_kind_ = HScrollKind::Table;
                hovered_hscroll_idx_ = hr.item_index;
                cursor = IDC_HAND;
            } else if (hr.target == HitTestResult::Target::Link) {
                hovered_link_url_ = hr.url;
                cursor = IDC_HAND;
            } else if (hr.target == HitTestResult::Target::Image) {
                cursor = IDC_HAND;
            } else if (hr.target == HitTestResult::Target::Text) {
                cursor = IDC_IBEAM;
            }
        }
    }

    SetCursor(LoadCursorW(nullptr, cursor));

    if (old_tb != hovered_toolbar_btn_ || old_ol != hovered_outline_idx_ ||
        old_cc != hovered_copy_code_idx_ || old_cm != hovered_copy_math_idx_ ||
        old_fb != hovered_find_btn_ || old_link != hovered_link_url_ ||
        old_hsk != hovered_hscroll_kind_ || old_hsi != hovered_hscroll_idx_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnLButtonDown(float x_dip, float y_dip, WPARAM wp) {
    if (lightbox_image_idx_ >= 0) {
        lightbox_image_idx_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);

    // Toolbar & merged caption clicks
    if (RectContains(tb, x_dip, y_dip)) {
        ToolbarHitRects tr = ComputeToolbarButtons(tb);
        if (RectContains(tr.btn_outline, x_dip, y_dip)) ToggleOutline();
        else if (RectContains(tr.btn_open, x_dip, y_dip)) OpenFileDialog();
        else if (RectContains(tr.btn_edit, x_dip, y_dip)) ToggleEditMode();
        else if (RectContains(tr.btn_find, x_dip, y_dip)) ShowFindBar(!find_visible_);
        else if (RectContains(tr.btn_zoom_out, x_dip, y_dip)) SetZoom(config_.zoom_percent - 10);
        else if (RectContains(tr.btn_zoom_reset, x_dip, y_dip)) SetZoom(100);
        else if (RectContains(tr.btn_zoom_in, x_dip, y_dip)) SetZoom(config_.zoom_percent + 10);
        else if (RectContains(tr.btn_theme, x_dip, y_dip)) CycleTheme();
        else if (RectContains(tr.btn_min, x_dip, y_dip)) ShowWindow(hwnd_, SW_MINIMIZE);
        else if (RectContains(tr.btn_max, x_dip, y_dip)) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        else if (RectContains(tr.btn_close, x_dip, y_dip)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }

    // Find bar clicks (input box & buttons)
    if (find_visible_ && RectContains(GetFindBarRect(vp), x_dip, y_dip)) {
        D2D1_RECT_F bar = GetFindBarRect(vp);
        D2D1_RECT_F inp = GetFindInputRect(bar);
        D2D1_RECT_F prev_r = D2D1::RectF(bar.right - 74.0f, bar.top + 6.0f, bar.right - 52.0f, bar.bottom - 6.0f);
        D2D1_RECT_F next_r = D2D1::RectF(bar.right - 50.0f, bar.top + 6.0f, bar.right - 28.0f, bar.bottom - 6.0f);
        D2D1_RECT_F close_r = D2D1::RectF(bar.right - 26.0f, bar.top + 6.0f, bar.right - 4.0f, bar.bottom - 6.0f);

        if (RectContains(inp, x_dip, y_dip)) {
            find_sel_all_ = false;
            SetFindFocused(true);
            if (!find_query_.empty() && d2d_.DWrite()) {
                if (auto tl = MakeUiText(d2d_.DWrite(), find_query_, 12.8f, false, 4000.0f)) {
                    BOOL trailing = FALSE, inside = FALSE;
                    DWRITE_HIT_TEST_METRICS htm{};
                    float local_x = (x_dip - (inp.left + 8.0f)) + find_scroll_x_;
                    tl->HitTestPoint(local_x, 4.0f, &trailing, &inside, &htm);
                    find_caret_ = std::min<size_t>(find_query_.size(), htm.textPosition + (trailing ? htm.length : 0));
                }
            } else {
                find_caret_ = 0;
            }
            find_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (RectContains(prev_r, x_dip, y_dip)) {
            JumpToMatch(-1);
        } else if (RectContains(next_r, x_dip, y_dip)) {
            JumpToMatch(1);
        } else if (RectContains(close_r, x_dip, y_dip)) {
            ShowFindBar(false);
        }
        return;
    }

    if (find_focused_) {
        SetFindFocused(false);
    }

    // Splitter drag
    if (sb.right > sb.left && RectContains(sp, x_dip, y_dip)) {
        dragging_splitter_ = true;
        SetCapture(hwnd_);
        return;
    }

    // Sidebar outline click
    if (sb.right > sb.left && RectContains(sb, x_dip, y_dip)) {
        float list_top = sb.top + kOutlineHeaderH;
        float list_bot = sb.bottom - kOutlineBottomPad;
        if (y_dip >= list_top && y_dip <= list_bot && !doc_.outline.empty()) {
            int idx = static_cast<int>((y_dip - (list_top + 6.0f) + outline_scroll_y_) / kOutlineRowH);
            if (idx >= 0 && idx < static_cast<int>(doc_.outline.size())) {
                if (edit_mode_) {
                    editor_.SetCaretToLine(doc_.outline[idx].source_line);
                    editor_caret_visible_ = true;
                    UpdateImePosition();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else {
                    float hy = 0.0f;
                    if (layout_.GetHeadingY(doc_.outline[idx].heading_id, hy)) {
                        ScrollToY(hy - 18.0f, true);
                    }
                }
            }
        }
        return;
    }

    // Vertical scrollbar drag
    float max_s = edit_mode_ ? editor_.MaxScrollY() : MaxDocScrollY();
    float total_h = edit_mode_ ? editor_.TotalHeight() : layout_.TotalHeight();
    float cur_sy = edit_mode_ ? editor_.ScrollY() : scroll_y_;
    if (max_s > 1.0f && x_dip >= vs.left - 4.0f && x_dip <= vs.right + 4.0f && y_dip >= vp.top && y_dip <= vp.bottom) {
        float vp_h = vp.bottom - vp.top;
        float track_h = vs.bottom - vs.top;
        float thumb_h = std::max(32.0f, track_h * (vp_h / std::max(1.0f, total_h)));
        float thumb_y = vs.top + (track_h - thumb_h) * (cur_sy / max_s);
        if (y_dip >= thumb_y && y_dip <= thumb_y + thumb_h) {
            vscroll_drag_offset_ = y_dip - thumb_y;
        } else {
            vscroll_drag_offset_ = thumb_h * 0.5f;
            float ratio = std::clamp((y_dip - vs.top - vscroll_drag_offset_) / std::max(1.0f, track_h - thumb_h), 0.0f, 1.0f);
            if (edit_mode_) {
                editor_.SetScrollY(ratio * max_s);
                UpdateImePosition();
            } else {
                scroll_y_ = target_scroll_y_ = ratio * max_s;
            }
        }
        dragging_vscroll_ = true;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (edit_mode_) {
        if (RectContains(vp, x_dip, y_dip)) {
            if (editor_.OnLButtonDown(x_dip, y_dip, (wp & MK_SHIFT) != 0)) {
                editor_caret_visible_ = true;
                UpdateImePosition();
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return;
    }

    // Document hit test
    if (RectContains(vp, x_dip, y_dip)) {
        float doc_x = x_dip - vp.left;
        float doc_y = y_dip - vp.top + scroll_y_;
        HitTestResult hr = layout_.HitTest(doc_x, doc_y);

        if (hr.target == HitTestResult::Target::CopyCodeBtn) {
            const auto& cbs = layout_.CodeBlocks();
            if (hr.item_index >= 0 && hr.item_index < static_cast<int>(cbs.size())) {
                copied_code_idx_ = hr.item_index;
                copied_code_until_ = GetTickCount64() + 1500;
                CopyTextToClipboard(cbs[hr.item_index].code, L"已复制代码块");
            }
            return;
        }
        if (hr.target == HitTestResult::Target::CopyMathBtn) {
            const auto& mbs = layout_.MathBlocks();
            if (hr.item_index >= 0 && hr.item_index < static_cast<int>(mbs.size())) {
                copied_math_idx_ = hr.item_index;
                copied_math_until_ = GetTickCount64() + 1500;
                CopyTextToClipboard(mbs[hr.item_index].tex, L"已复制 LaTeX 公式");
            }
            return;
        }
        if (hr.target == HitTestResult::Target::CodeHScroll || hr.target == HitTestResult::Target::TableHScroll) {
            const bool is_code = (hr.target == HitTestResult::Target::CodeHScroll);
            D2D1_RECT_F track{}, thumb{};
            float cur_sx = 0.0f, max_sx = 0.0f;
            if (is_code && hr.item_index >= 0 && hr.item_index < static_cast<int>(layout_.CodeBlocks().size())) {
                const auto& cb = layout_.CodeBlocks()[hr.item_index];
                track = cb.hscroll_track_rect;
                thumb = cb.hscroll_thumb_rect;
                cur_sx = cb.scroll_x;
                max_sx = cb.max_scroll_x;
            } else if (!is_code && hr.item_index >= 0 && hr.item_index < static_cast<int>(layout_.Tables().size())) {
                const auto& tbl = layout_.Tables()[hr.item_index];
                track = tbl.hscroll_track_rect;
                thumb = tbl.hscroll_thumb_rect;
                cur_sx = tbl.scroll_x;
                max_sx = tbl.max_scroll_x;
            }
            if (max_sx > 1.0f) {
                float track_w = std::max(1.0f, track.right - track.left);
                float thumb_w = thumb.right - thumb.left;
                float denom = std::max(1.0f, track_w - thumb_w);
                float cur_thumb_left = track.left + denom * (cur_sx / max_sx);

                dragging_hscroll_kind_ = is_code ? HScrollKind::Code : HScrollKind::Table;
                active_hscroll_idx_ = hr.item_index;
                if (doc_x >= cur_thumb_left - 2.0f && doc_x <= cur_thumb_left + thumb_w + 2.0f) {
                    // Clicked inside the thumb: keep the exact grab point relative to thumb left
                    hscroll_drag_offset_ = doc_x - cur_thumb_left;
                } else {
                    // Clicked on the track outside thumb: jump thumb centered at click & drag cleanly from center
                    hscroll_drag_offset_ = thumb_w * 0.5f;
                    UpdateHScrollDrag(x_dip);
                }
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (hr.target == HitTestResult::Target::Image) {
            const auto& imgs = layout_.Images();
            if (hr.item_index >= 0 && hr.item_index < static_cast<int>(imgs.size()) &&
                imgs[hr.item_index].image && imgs[hr.item_index].image->status == LoadedImage::Status::Ready) {
                lightbox_image_idx_ = hr.item_index;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }

        int sid = -1;
        uint32_t pos = 0;
        if (layout_.HitTestSelectableNearest(doc_x, doc_y, sid, pos)) {
            if ((wp & MK_SHIFT) && selection_.start_sel >= 0) {
                selection_.end_sel = sid;
                selection_.end_pos = pos;
            } else {
                selection_.start_sel = selection_.end_sel = sid;
                selection_.start_pos = selection_.end_pos = pos;
            }
            dragging_selection_ = true;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            selection_.Clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

void MainWindow::OnLButtonDblClick(float x_dip, float y_dip) {
    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);

    if (find_visible_ && RectContains(GetFindBarRect(vp), x_dip, y_dip)) {
        D2D1_RECT_F inp = GetFindInputRect(GetFindBarRect(vp));
        if (RectContains(inp, x_dip, y_dip)) {
            SetFindFocused(true);
            find_sel_all_ = !find_query_.empty();
            find_caret_ = find_query_.size();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        OnLButtonDown(x_dip, y_dip, MK_LBUTTON);
        return;
    }

    // Forward rapid clicks on toolbar, sidebar, scrollbar, and buttons so rapid toggling never drops clicks.
    float max_vs = edit_mode_ ? editor_.MaxScrollY() : MaxDocScrollY();
    if (!RectContains(vp, x_dip, y_dip) || lightbox_image_idx_ >= 0 ||
        (max_vs > 1.0f && x_dip >= vs.left - 4.0f && x_dip <= vs.right + 4.0f)) {
        OnLButtonDown(x_dip, y_dip, MK_LBUTTON);
        return;
    }

    if (edit_mode_) {
        if (editor_.OnLButtonDblClick(x_dip, y_dip)) {
            editor_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    HitTestResult hr = layout_.HitTest(x_dip - vp.left, y_dip - vp.top + scroll_y_);
    if (hr.target == HitTestResult::Target::CopyCodeBtn ||
        hr.target == HitTestResult::Target::CopyMathBtn ||
        hr.target == HitTestResult::Target::Image) {
        OnLButtonDown(x_dip, y_dip, MK_LBUTTON);
        return;
    }

    if ((hr.target == HitTestResult::Target::Text || hr.target == HitTestResult::Target::Link) && hr.selectable_id >= 0) {
        uint32_t s = 0, e = 0;
        layout_.SelectWordAt(hr.selectable_id, hr.text_pos, s, e);
        selection_.start_sel = selection_.end_sel = hr.selectable_id;
        selection_.start_pos = s;
        selection_.end_pos = e;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnLButtonUp(float x_dip, float y_dip) {
    bool was_selecting = dragging_selection_;
    if (dragging_splitter_) {
        dragging_splitter_ = false;
        SaveAppConfig(config_);
    }
    if (edit_mode_) {
        editor_.OnLButtonUp();
    }
    dragging_selection_ = false;
    dragging_vscroll_ = false;
    dragging_hscroll_kind_ = HScrollKind::None;
    active_hscroll_idx_ = -1;
    if (!autoscroll_active_ && GetCapture() == hwnd_) ReleaseCapture();

    if (edit_mode_) return;

    if (was_selecting && !selection_.HasSelection()) {
        D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
        ComputeRegions(tb, sb, sp, vp, vs);
        if (RectContains(vp, x_dip, y_dip)) {
            HitTestResult hr = layout_.HitTest(x_dip - vp.left, y_dip - vp.top + scroll_y_);
            if (hr.target == HitTestResult::Target::Link && !hr.url.empty()) {
                HandleLinkClick(hr.url);
            }
        }
    }
}

void MainWindow::OnRButtonUp(float, float, POINT screen_pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (edit_mode_) {
        bool has_sel = editor_.HasSelection();
        AppendMenuW(menu, has_sel ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_CUT, L"剪切\tCtrl+X");
        AppendMenuW(menu, has_sel ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_COPY, L"复制\tCtrl+C");
        AppendMenuW(menu, MF_STRING, CMD_PASTE, L"粘贴\tCtrl+V");
        AppendMenuW(menu, MF_STRING, CMD_SELECT_ALL, L"全选\tCtrl+A");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, editor_.CanUndo() ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_UNDO, L"撤销\tCtrl+Z");
        AppendMenuW(menu, editor_.CanRedo() ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_REDO, L"重做\tCtrl+Y");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_SAVE, L"保存文件\tCtrl+S");
        AppendMenuW(menu, MF_STRING, CMD_TOGGLE_EDIT, L"完成并返回阅读\tCtrl+E");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (config_.outline_visible ? MF_CHECKED : 0), CMD_TOGGLE_OUTLINE, L"显示侧栏大纲\tCtrl+B");
        AppendMenuW(menu, MF_STRING, CMD_FIND, L"源码内搜索\tCtrl+F");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, screen_pt.x, screen_pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        return;
    }

    AppendMenuW(menu, selection_.HasSelection() ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_COPY, L"复制\tCtrl+C");
    AppendMenuW(menu, MF_STRING, CMD_SELECT_ALL, L"全选\tCtrl+A");
    AppendMenuW(menu, MF_STRING, CMD_TOGGLE_EDIT, L"编辑源码\tCtrl+E");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FILE, L"打开文件...\tCtrl+O");

    if (!config_.recent_files.empty()) {
        HMENU rec = CreatePopupMenu();
        for (size_t i = 0; i < config_.recent_files.size() && i < 10; ++i) {
            AppendMenuW(rec, MF_STRING, CMD_RECENT_BASE + static_cast<UINT>(i), config_.recent_files[i].c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rec), L"最近打开");
    }

    if (!doc_.file_path.empty()) {
        AppendMenuW(menu, MF_STRING, CMD_RELOAD, L"重新加载\tF5");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (config_.outline_visible ? MF_CHECKED : 0), CMD_TOGGLE_OUTLINE, L"显示侧栏大纲\tCtrl+B");
    AppendMenuW(menu, MF_STRING, CMD_FIND, L"文档内搜索\tCtrl+F");

    HMENU theme_menu = CreatePopupMenu();
    AppendMenuW(theme_menu, MF_STRING | (config_.theme == ThemeMode::Light ? MF_CHECKED : 0), CMD_THEME_LIGHT, L"亮色纸张 (Light)");
    AppendMenuW(theme_menu, MF_STRING | (config_.theme == ThemeMode::Dark ? MF_CHECKED : 0), CMD_THEME_DARK, L"暗夜深空 (Dark)");
    AppendMenuW(theme_menu, MF_STRING | (config_.theme == ThemeMode::Sepia ? MF_CHECKED : 0), CMD_THEME_SEPIA, L"暖色护眼 (Sepia)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme_menu), L"阅读主题");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_ZOOM_IN, L"放大字号\tCtrl++");
    AppendMenuW(menu, MF_STRING, CMD_ZOOM_OUT, L"缩小字号\tCtrl+-");
    AppendMenuW(menu, MF_STRING, CMD_ZOOM_RESET, L"重置字号 (100%)\tCtrl+0");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_WELCOME, L"查看欢迎示例文档");

    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, screen_pt.x, screen_pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::OnMouseWheel(float x_dip, float y_dip, int delta, bool horizontal, WPARAM wp) {
    if (lightbox_image_idx_ >= 0) return;

    if ((wp & MK_CONTROL) != 0) {
        int step = (delta > 0) ? 10 : -10;
        SetZoom(config_.zoom_percent + step);
        return;
    }

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);

    if (sb.right > sb.left && RectContains(sb, x_dip, y_dip)) {
        float step = -static_cast<float>(delta) / 120.0f * (kOutlineRowH * 2.5f);
        outline_scroll_y_ = std::clamp(outline_scroll_y_ + step, 0.0f, MaxOutlineScrollY());
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (edit_mode_) {
        if (!horizontal) {
            float step = -static_cast<float>(delta) / 120.0f * 88.0f;
            editor_.ScrollBy(step);
            if (!anim_timer_active_) {
                anim_timer_active_ = true;
                SetTimer(hwnd_, kTimerAnim, 15, nullptr);
            }
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (horizontal) {
        float doc_x = x_dip - vp.left;
        float doc_y = y_dip - vp.top + scroll_y_;
        float dx = -static_cast<float>(delta) / 120.0f * 64.0f;
        const auto& cbs = layout_.CodeBlocks();
        for (size_t i = 0; i < cbs.size(); ++i) {
            if (RectContains(cbs[i].card_rect, doc_x, doc_y)) {
                if (layout_.ScrollCodeBlockBy(static_cast<int>(i), dx)) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
        }
        const auto& mbs = layout_.MathBlocks();
        for (size_t i = 0; i < mbs.size(); ++i) {
            if (RectContains(mbs[i].card_rect, doc_x, doc_y)) {
                if (layout_.ScrollMathBlockBy(static_cast<int>(i), dx)) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
        }
        const auto& tbls = layout_.Tables();
        for (size_t i = 0; i < tbls.size(); ++i) {
            if (RectContains(tbls[i].card_rect, doc_x, doc_y)) {
                if (layout_.ScrollTableBy(static_cast<int>(i), dx)) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
        }
        return;
    }

    float step = -static_cast<float>(delta) / 120.0f * 88.0f;
    ScrollToY(target_scroll_y_ + step, true);
}

void MainWindow::OnKeyDown(WPARAM vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (vk == VK_ESCAPE) {
        if (autoscroll_active_) {
            StopAutoScroll();
        } else if (lightbox_image_idx_ >= 0) {
            lightbox_image_idx_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (find_visible_) {
            ShowFindBar(false);
        } else if (selection_.HasSelection()) {
            selection_.Clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (ctrl && vk == 'E') {
        ToggleEditMode();
        return;
    }
    if (ctrl && vk == 'S' && edit_mode_) {
        SaveCurrentDocument(true);
        return;
    }

    // Search input editing keys when the search box is focused
    if (find_visible_ && find_focused_) {
        if (vk == VK_RETURN) {
            JumpToMatch(shift ? -1 : 1);
            return;
        }
        if (vk == VK_BACK) {
            if (find_sel_all_) {
                find_query_.clear();
                find_caret_ = 0;
                find_sel_all_ = false;
                RunSearch(find_query_, true);
            } else if (find_caret_ > 0 && !find_query_.empty()) {
                find_query_.erase(find_caret_ - 1, 1);
                --find_caret_;
                RunSearch(find_query_, true);
            }
            find_caret_visible_ = true;
            UpdateImePosition();
            return;
        }
        if (vk == VK_DELETE) {
            if (find_sel_all_) {
                find_query_.clear();
                find_caret_ = 0;
                find_sel_all_ = false;
                RunSearch(find_query_, true);
            } else if (find_caret_ < find_query_.size()) {
                find_query_.erase(find_caret_, 1);
                RunSearch(find_query_, true);
            }
            find_caret_visible_ = true;
            UpdateImePosition();
            return;
        }
        if (vk == VK_LEFT) {
            find_sel_all_ = false;
            if (find_caret_ > 0) --find_caret_;
            find_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_RIGHT) {
            find_sel_all_ = false;
            if (find_caret_ < find_query_.size()) ++find_caret_;
            find_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_HOME) {
            find_sel_all_ = false;
            find_caret_ = 0;
            find_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_END) {
            find_sel_all_ = false;
            find_caret_ = find_query_.size();
            find_caret_visible_ = true;
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (ctrl) {
            if (vk == 'A') {
                find_sel_all_ = !find_query_.empty();
                find_caret_ = find_query_.size();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (vk == 'C') {
                if (!find_query_.empty()) CopyTextToClipboard(find_query_, L"");
                return;
            }
            if (vk == 'X') {
                if (!find_query_.empty()) {
                    CopyTextToClipboard(find_query_, L"");
                    find_query_.clear();
                    find_caret_ = 0;
                    find_sel_all_ = false;
                    RunSearch(find_query_, true);
                }
                return;
            }
            if (vk == 'V') {
                std::wstring pasted = ReadTextFromClipboard();
                for (wchar_t& c : pasted) {
                    if (c == L'\r' || c == L'\n') c = L' ';
                }
                if (!pasted.empty()) {
                    if (find_sel_all_) {
                        find_query_.clear();
                        find_caret_ = 0;
                        find_sel_all_ = false;
                    }
                    find_caret_ = std::min(find_caret_, find_query_.size());
                    find_query_.insert(find_caret_, pasted);
                    find_caret_ += pasted.size();
                    find_caret_visible_ = true;
                    RunSearch(find_query_, true);
                    UpdateImePosition();
                }
                return;
            }
            if (vk == 'F') {
                find_sel_all_ = !find_query_.empty();
                find_caret_ = find_query_.size();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
    }

    if (edit_mode_ && !find_focused_) {
        bool mutated = false;
        bool handled = false;
        if (ctrl) {
            if (vk == 'O') { OpenFileDialog(); return; }
            if (vk == 'F') { ShowFindBar(true); return; }
            if (vk == 'B') { ToggleOutline(); return; }
            if (vk == 'R') {
                if (ConfirmDiscardOrSaveBeforeLeave()) ReloadCurrentFile(true);
                return;
            }
            if (vk == 'C') {
                if (editor_.HasSelection()) {
                    CopyTextToClipboard(editor_.GetSelectedText(), L"已复制到剪贴板");
                }
                return;
            }
            if (vk == 'X') {
                if (editor_.HasSelection()) {
                    CopyTextToClipboard(editor_.GetSelectedText(), L"");
                    editor_.DeleteSelection();
                    editor_.EnsureCaretVisible();
                    mutated = true;
                }
                handled = true;
            } else if (vk == 'V') {
                std::wstring pasted = ReadTextFromClipboard();
                if (!pasted.empty()) {
                    editor_.PasteText(pasted);
                    editor_.EnsureCaretVisible();
                    mutated = true;
                }
                handled = true;
            } else if (vk == 'Z' || vk == 'Y') {
                handled = editor_.OnKeyDown(vk, ctrl, shift, alt);
                mutated = handled;
            } else if (vk == VK_OEM_PLUS || vk == VK_ADD) { SetZoom(config_.zoom_percent + 10); return; }
            else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { SetZoom(config_.zoom_percent - 10); return; }
            else if (vk == '0' || vk == VK_NUMPAD0) { SetZoom(100); return; }
        }
        if (!handled) {
            handled = editor_.OnKeyDown(vk, ctrl, shift, alt);
            if (handled && (vk == VK_BACK || vk == VK_DELETE || vk == VK_RETURN || vk == VK_TAB)) {
                mutated = true;
            }
        }
        if (handled) {
            editor_caret_visible_ = true;
            if (mutated && find_visible_ && !find_query_.empty()) {
                RunSearch(find_query_, false);
            }
            if (mutated) UpdateWindowTitle();
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    if (ctrl) {
        if (vk == 'O') { OpenFileDialog(); return; }
        if (vk == 'F') { ShowFindBar(true); return; }
        if (vk == 'B') { ToggleOutline(); return; }
        if (vk == 'R') { ReloadCurrentFile(true); return; }
        if (vk == 'C') {
            if (selection_.HasSelection()) {
                CopyTextToClipboard(layout_.ExtractSelectedText(selection_), L"已复制到剪贴板");
            }
            return;
        }
        if (vk == 'A') {
            std::wstring all = layout_.ExtractAllText();
            if (!all.empty()) {
                selection_.start_sel = 0;
                selection_.start_pos = 0;
                int last_sid = 0;
                uint32_t last_pos = 0;
                layout_.HitTestSelectableNearest(100000.0f, layout_.TotalHeight() + 100.0f, last_sid, last_pos);
                selection_.end_sel = last_sid;
                selection_.end_pos = last_pos;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { SetZoom(config_.zoom_percent + 10); return; }
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { SetZoom(config_.zoom_percent - 10); return; }
        if (vk == '0' || vk == VK_NUMPAD0) { SetZoom(100); return; }
    }

    if (alt) {
        if (vk == VK_LEFT) { NavigateHistory(-1); return; }
        if (vk == VK_RIGHT) { NavigateHistory(1); return; }
    }

    if (vk == VK_F5) {
        ReloadCurrentFile(true);
        return;
    }

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    float page_h = std::max(120.0f, (vp.bottom - vp.top) * 0.85f);

    switch (vk) {
        case VK_DOWN: ScrollToY(target_scroll_y_ + 56.0f, true); break;
        case VK_UP: ScrollToY(target_scroll_y_ - 56.0f, true); break;
        case VK_NEXT: ScrollToY(target_scroll_y_ + page_h, true); break;
        case VK_PRIOR: ScrollToY(target_scroll_y_ - page_h, true); break;
        case VK_SPACE:
            if (!find_focused_) ScrollToY(target_scroll_y_ + (shift ? -page_h : page_h), true);
            break;
        case VK_HOME: ScrollToY(0.0f, true); break;
        case VK_END: ScrollToY(MaxDocScrollY(), true); break;
        default: break;
    }
}

void MainWindow::OnDropFiles(HDROP hdrop) {
    UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    if (count > 0) {
        UINT len = DragQueryFileW(hdrop, 0, nullptr, 0);
        std::wstring path(len + 1, L'\0');
        DragQueryFileW(hdrop, 0, path.data(), len + 1);
        path.resize(len);
        if (!path.empty() && ConfirmDiscardOrSaveBeforeLeave()) {
            OpenFile(path, true);
        }
    }
    DragFinish(hdrop);
}

void MainWindow::OnTimer(UINT_PTR id) {
    if (id == kTimerAnim) {
        if (edit_mode_) {
            if (!editor_.AnimateScroll()) {
                KillTimer(hwnd_, kTimerAnim);
                anim_timer_active_ = false;
            }
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        float diff = target_scroll_y_ - scroll_y_;
        if (std::abs(diff) < 0.6f) {
            scroll_y_ = target_scroll_y_;
            KillTimer(hwnd_, kTimerAnim);
            anim_timer_active_ = false;
        } else {
            scroll_y_ += diff * 0.30f;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (id == kTimerAutoScroll) {
        if (!autoscroll_active_) {
            KillTimer(hwnd_, kTimerAutoScroll);
            return;
        }
        POINT gpt{};
        if (GetCursorPos(&gpt) &&
            (gpt.x != autoscroll_last_screen_pt_.x || gpt.y != autoscroll_last_screen_pt_.y)) {
            autoscroll_last_screen_pt_ = gpt;
            ScreenToClient(hwnd_, &gpt);
            autoscroll_cur_x_ = PxToDip(gpt.x);
            autoscroll_cur_y_ = PxToDip(gpt.y);
            if (std::hypot(autoscroll_cur_x_ - autoscroll_anchor_x_, autoscroll_cur_y_ - autoscroll_anchor_y_) > 6.0f) {
                autoscroll_moved_ = true;
            }
        }
        float delta = (autoscroll_h_kind_ != HScrollKind::None)
            ? (autoscroll_cur_x_ - autoscroll_anchor_x_)
            : (autoscroll_cur_y_ - autoscroll_anchor_y_);
        constexpr float kDeadzone = 9.0f;
        if (std::abs(delta) > kDeadzone) {
            float excess = std::abs(delta) - kDeadzone;
            float speed = std::clamp(excess * 0.22f + excess * excess * 0.0028f, 0.9f, 44.0f) *
                          (delta > 0.0f ? 1.0f : -1.0f);
            bool changed = false;
            if (autoscroll_h_kind_ == HScrollKind::Table) {
                changed = layout_.ScrollTableBy(autoscroll_h_idx_, speed);
            } else if (autoscroll_h_kind_ == HScrollKind::Code) {
                changed = layout_.ScrollCodeBlockBy(autoscroll_h_idx_, speed);
            } else if (edit_mode_) {
                float old_y = editor_.ScrollY();
                editor_.SetScrollY(old_y + speed);
                changed = (std::abs(editor_.ScrollY() - old_y) > 0.1f);
                if (changed) UpdateImePosition();
            } else {
                float old_y = scroll_y_;
                scroll_y_ = target_scroll_y_ = std::clamp(scroll_y_ + speed, 0.0f, MaxDocScrollY());
                changed = (std::abs(scroll_y_ - old_y) > 0.1f);
            }
            if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (id == kTimerCaret) {
        if (find_visible_ && find_focused_) {
            find_caret_visible_ = !find_caret_visible_;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (edit_mode_ && !find_focused_) {
            editor_caret_visible_ = !editor_caret_visible_;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (id == kTimerWatch) {
        if (watcher_.HasChanged() && !(edit_mode_ && editor_.IsModified())) {
            ReloadCurrentFile(true);
        }
        ULONGLONG now = GetTickCount64();
        bool need_repaint = false;
        if (!toast_text_.empty() && now >= toast_until_) {
            toast_text_.clear();
            need_repaint = true;
        }
        if (copied_code_idx_ >= 0 && now >= copied_code_until_) {
            copied_code_idx_ = -1;
            need_repaint = true;
        }
        if (copied_math_idx_ >= 0 && now >= copied_math_until_) {
            copied_math_idx_ = -1;
            need_repaint = true;
        }
        if (need_repaint) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::ScrollToY(float target_y, bool animate) {
    target_scroll_y_ = std::clamp(target_y, 0.0f, MaxDocScrollY());
    if (!animate) {
        scroll_y_ = target_scroll_y_;
        if (anim_timer_active_) {
            KillTimer(hwnd_, kTimerAnim);
            anim_timer_active_ = false;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!anim_timer_active_) {
        anim_timer_active_ = true;
        SetTimer(hwnd_, kTimerAnim, 15, nullptr);
    }
}

void MainWindow::SetZoom(int new_zoom) {
    int clamped = std::clamp(new_zoom, 50, 250);
    if (clamped == config_.zoom_percent) return;
    config_.zoom_percent = clamped;
    SaveAppConfig(config_);
    RebuildLayout(true);
    ShowToast(L"字号缩放: " + std::to_wstring(clamped) + L"%");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CycleTheme() {
    ThemeMode next = ThemeMode::Light;
    if (config_.theme == ThemeMode::Light) next = ThemeMode::Dark;
    else if (config_.theme == ThemeMode::Dark) next = ThemeMode::Sepia;
    SetTheme(next);
}

void MainWindow::SetTheme(ThemeMode mode) {
    if (config_.theme == mode) return;
    config_.theme = mode;
    SaveAppConfig(config_);
    UpdateDwmAppearance();
    RebuildLayout(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleOutline() {
    config_.outline_visible = !config_.outline_visible;
    SaveAppConfig(config_);
    RebuildLayout(true);
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::HandleLinkClick(const std::wstring& url) {
    if (url.empty()) return;
    if (url.front() == L'#') {
        std::string slug = WideToUtf8(url.substr(1));
        float target_y = 0.0f;
        if (layout_.GetAnchorY(slug, target_y)) {
            ScrollToY(target_y - 18.0f, true);
        }
        return;
    }

    if (IsRemoteUrl(url) || url.rfind(L"mailto:", 0) == 0) {
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    std::wstring target = url;
    std::wstring anchor;
    size_t hash = target.find(L'#');
    if (hash != std::wstring::npos) {
        anchor = target.substr(hash + 1);
        target = target.substr(0, hash);
    }

    std::wstring resolved = ResolveDocumentResourcePath(doc_.base_dir, target);
    if (!resolved.empty() && GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (IsMarkdownFileExt(resolved)) {
            if (!ConfirmDiscardOrSaveBeforeLeave()) return;
            OpenFile(resolved, true);
            if (!anchor.empty()) {
                float ay = 0.0f;
                if (layout_.GetAnchorY(WideToUtf8(anchor), ay)) {
                    ScrollToY(ay - 18.0f, false);
                }
            }
            return;
        }
        ShellExecuteW(hwnd_, L"open", resolved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::OpenFileDialog() {
    if (!ConfirmDiscardOrSaveBeforeLeave()) return;
    ComPtr<IFileOpenDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.Put()))) && dlg) {
        COMDLG_FILTERSPEC filters[] = {
            {L"Markdown 文档 (*.md;*.markdown)", L"*.md;*.markdown;*.mdown;*.mkd"},
            {L"所有文本与文件 (*.*)", L"*.*"}
        };
        dlg->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dlg->SetTitle(L"选择要阅读的 Markdown 文件");
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(item.Put())) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    OpenFile(path, true);
                    CoTaskMemFree(path);
                }
            }
        }
        return;
    }

    wchar_t file_buf[MAX_PATH * 4]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Markdown (*.md;*.markdown)\0*.md;*.markdown;*.mdown\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file_buf));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        OpenFile(file_buf, true);
    }
}

void MainWindow::NavigateHistory(int dir) {
    int next = history_index_ + dir;
    if (next < 0 || next >= static_cast<int>(history_.size())) return;
    if (!ConfirmDiscardOrSaveBeforeLeave()) return;
    history_index_ = next;
    OpenFile(history_[history_index_], false);
}

void MainWindow::CopyTextToClipboard(const std::wstring& text, const std::wstring& toast_msg) {
    if (text.empty()) return;
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hmem) {
        void* dst = GlobalLock(hmem);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(hmem);
            SetClipboardData(CF_UNICODETEXT, hmem);
        } else {
            GlobalFree(hmem);
        }
    }
    CloseClipboard();
    if (!toast_msg.empty()) ShowToast(toast_msg);
}

std::wstring MainWindow::ReadTextFromClipboard() {
    std::wstring res;
    if (!OpenClipboard(hwnd_)) return res;
    HANDLE hdata = GetClipboardData(CF_UNICODETEXT);
    if (hdata) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(hdata));
        if (p) {
            res = p;
            GlobalUnlock(hdata);
        }
    }
    CloseClipboard();
    return res;
}

void MainWindow::ShowToast(const std::wstring& msg) {
    toast_text_ = msg;
    toast_until_ = GetTickCount64() + 1600;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateHScrollDrag(float x_dip) {
    if (dragging_hscroll_kind_ == HScrollKind::None || active_hscroll_idx_ < 0) return;
    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    float doc_x = x_dip - vp.left;

    D2D1_RECT_F track{}, thumb{};
    float cur_sx = 0.0f, max_sx = 0.0f;
    if (dragging_hscroll_kind_ == HScrollKind::Code &&
        active_hscroll_idx_ < static_cast<int>(layout_.CodeBlocks().size())) {
        const auto& cb = layout_.CodeBlocks()[active_hscroll_idx_];
        track = cb.hscroll_track_rect;
        thumb = cb.hscroll_thumb_rect;
        cur_sx = cb.scroll_x;
        max_sx = cb.max_scroll_x;
    } else if (dragging_hscroll_kind_ == HScrollKind::Table &&
               active_hscroll_idx_ < static_cast<int>(layout_.Tables().size())) {
        const auto& tbl = layout_.Tables()[active_hscroll_idx_];
        track = tbl.hscroll_track_rect;
        thumb = tbl.hscroll_thumb_rect;
        cur_sx = tbl.scroll_x;
        max_sx = tbl.max_scroll_x;
    } else {
        return;
    }
    if (max_sx <= 1.0f) return;

    float track_w = std::max(1.0f, track.right - track.left);
    float thumb_w = thumb.right - thumb.left;
    float denom = std::max(1.0f, track_w - thumb_w);
    float desired_thumb_left = doc_x - hscroll_drag_offset_;
    float ratio = std::clamp((desired_thumb_left - track.left) / denom, 0.0f, 1.0f);
    float target_sx = ratio * max_sx;
    if (dragging_hscroll_kind_ == HScrollKind::Code) {
        layout_.ScrollCodeBlockBy(active_hscroll_idx_, target_sx - cur_sx);
    } else {
        layout_.ScrollTableBy(active_hscroll_idx_, target_sx - cur_sx);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnMButtonDown(float x_dip, float y_dip) {
    if (autoscroll_active_) {
        StopAutoScroll();
        return;
    }
    if (lightbox_image_idx_ >= 0) return;

    D2D1_RECT_F tb{}, sb{}, sp{}, vp{}, vs{};
    ComputeRegions(tb, sb, sp, vp, vs);
    if (!RectContains(vp, x_dip, y_dip)) return;

    float doc_x = x_dip - vp.left;
    float doc_y = y_dip - vp.top + scroll_y_;

    autoscroll_h_kind_ = HScrollKind::None;
    autoscroll_h_idx_ = -1;

    if (!edit_mode_) {
        const auto& tbls = layout_.Tables();
        for (size_t i = 0; i < tbls.size(); ++i) {
            if (tbls[i].max_scroll_x > 1.0f && RectContains(tbls[i].card_rect, doc_x, doc_y)) {
                autoscroll_h_kind_ = HScrollKind::Table;
                autoscroll_h_idx_ = static_cast<int>(i);
                break;
            }
        }
        if (autoscroll_h_kind_ == HScrollKind::None) {
            const auto& cbs = layout_.CodeBlocks();
            for (size_t i = 0; i < cbs.size(); ++i) {
                if (cbs[i].max_scroll_x > 1.0f && RectContains(cbs[i].card_rect, doc_x, doc_y)) {
                    autoscroll_h_kind_ = HScrollKind::Code;
                    autoscroll_h_idx_ = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    autoscroll_active_ = true;
    autoscroll_moved_ = false;
    autoscroll_anchor_x_ = autoscroll_cur_x_ = x_dip;
    autoscroll_anchor_y_ = autoscroll_cur_y_ = y_dip;
    GetCursorPos(&autoscroll_last_screen_pt_);
    SetCapture(hwnd_);
    SetTimer(hwnd_, kTimerAutoScroll, 16, nullptr);
    SetCursor(LoadCursorW(nullptr, autoscroll_h_kind_ != HScrollKind::None ? IDC_SIZEWE : IDC_SIZENS));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnMButtonUp(float, float) {
    if (autoscroll_active_ && autoscroll_moved_) {
        StopAutoScroll();
    }
}

void MainWindow::StopAutoScroll() {
    if (!autoscroll_active_) return;
    autoscroll_active_ = false;
    autoscroll_moved_ = false;
    autoscroll_h_kind_ = HScrollKind::None;
    autoscroll_h_idx_ = -1;
    KillTimer(hwnd_, kTimerAutoScroll);
    if (GetCapture() == hwnd_) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RenderAutoScrollIndicator(
    ID2D1DeviceContext5* dc, ID2D1SolidColorBrush* brush, const ThemePalette& t) const {
    if (!autoscroll_active_) return;
    float cx = autoscroll_anchor_x_;
    float cy = autoscroll_anchor_y_;
    D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(cx, cy), 14.0f, 14.0f);

    brush->SetColor(t.bg_elevated);
    dc->FillEllipse(outer, brush);
    brush->SetColor(t.border_strong);
    dc->DrawEllipse(outer, brush, 1.2f);

    // Center pivot dot
    brush->SetColor(t.accent);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.5f, 2.5f), brush);

    auto round_stroke = MakeRoundStrokeStyle(d2d_.D2DFactory());
    brush->SetColor(t.text_primary);
    if (autoscroll_h_kind_ != HScrollKind::None) {
        // Horizontal arrows (<  o  >)
        dc->DrawLine(D2D1::Point2F(cx - 8.5f, cy), D2D1::Point2F(cx - 5.5f, cy - 3.0f), brush, 1.4f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(cx - 8.5f, cy), D2D1::Point2F(cx - 5.5f, cy + 3.0f), brush, 1.4f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(cx + 8.5f, cy), D2D1::Point2F(cx + 5.5f, cy - 3.0f), brush, 1.4f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(cx + 8.5f, cy), D2D1::Point2F(cx + 5.5f, cy + 3.0f), brush, 1.4f, round_stroke.Get());
    } else {
        // Vertical arrows (^  o  v)
        dc->DrawLine(D2D1::Point2F(cx, cy - 8.5f), D2D1::Point2F(cx - 3.0f, cy - 5.5f), brush, 1.4f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(cx, cy - 8.5f), D2D1::Point2F(cx + 3.0f, cy - 5.5f), brush, 1.4f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(cx, cy + 8.5f), D2D1::Point2F(cx - 3.0f, cy + 5.5f), brush, 1.4f, round_stroke.Get());
        dc->DrawLine(D2D1::Point2F(cx, cy + 8.5f), D2D1::Point2F(cx + 3.0f, cy + 5.5f), brush, 1.4f, round_stroke.Get());
    }
}

}  // namespace mr
