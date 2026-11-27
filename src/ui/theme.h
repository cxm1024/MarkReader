#pragma once

#include "core/syntax_hl.h"
#include "core/types.h"

#include <d2d1_3.h>

namespace mr {

struct ThemePalette {
    ThemeMode mode = ThemeMode::Light;

    // Chrome & surfaces
    D2D1_COLOR_F bg_canvas{};
    D2D1_COLOR_F bg_toolbar{};
    D2D1_COLOR_F bg_sidebar{};
    D2D1_COLOR_F bg_elevated{};
    D2D1_COLOR_F bg_hover{};
    D2D1_COLOR_F bg_active_item{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F border_strong{};

    // Text hierarchy
    D2D1_COLOR_F text_primary{};
    D2D1_COLOR_F text_secondary{};
    D2D1_COLOR_F text_muted{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accent_soft{};

    // Markdown elements
    D2D1_COLOR_F link{};
    D2D1_COLOR_F code_inline_bg{};
    D2D1_COLOR_F code_inline_border{};
    D2D1_COLOR_F code_inline_text{};

    D2D1_COLOR_F code_block_bg{};
    D2D1_COLOR_F code_block_header_bg{};
    D2D1_COLOR_F code_block_border{};

    D2D1_COLOR_F blockquote_bar{};
    D2D1_COLOR_F blockquote_bg{};
    D2D1_COLOR_F blockquote_text{};

    D2D1_COLOR_F math_block_bg{};
    D2D1_COLOR_F math_block_border{};

    D2D1_COLOR_F table_header_bg{};
    D2D1_COLOR_F table_stripe_bg{};
    D2D1_COLOR_F table_border{};

    D2D1_COLOR_F hr_color{};
    D2D1_COLOR_F highlight_bg{};
    D2D1_COLOR_F selection_bg{};
    D2D1_COLOR_F search_match_bg{};
    D2D1_COLOR_F search_active_bg{};
    D2D1_COLOR_F scrollbar_thumb{};
    D2D1_COLOR_F scrollbar_thumb_hover{};

    // Admonitions: Note, Tip, Important, Warning, Caution
    D2D1_COLOR_F adm_note_accent{};
    D2D1_COLOR_F adm_note_bg{};
    D2D1_COLOR_F adm_tip_accent{};
    D2D1_COLOR_F adm_tip_bg{};
    D2D1_COLOR_F adm_important_accent{};
    D2D1_COLOR_F adm_important_bg{};
    D2D1_COLOR_F adm_warning_accent{};
    D2D1_COLOR_F adm_warning_bg{};
    D2D1_COLOR_F adm_caution_accent{};
    D2D1_COLOR_F adm_caution_bg{};

    // Syntax highlighting
    D2D1_COLOR_F syn_normal{};
    D2D1_COLOR_F syn_keyword{};
    D2D1_COLOR_F syn_type{};
    D2D1_COLOR_F syn_string{};
    D2D1_COLOR_F syn_comment{};
    D2D1_COLOR_F syn_number{};
    D2D1_COLOR_F syn_preproc{};
    D2D1_COLOR_F syn_function{};
    D2D1_COLOR_F syn_operator{};

    // DWM Caption
    uint32_t titlebar_caption_rgb = 0xFFFFFF;
    uint32_t titlebar_text_rgb = 0x1F2328;
    bool is_dark = false;
};

const ThemePalette& GetThemePalette(ThemeMode mode);
D2D1_COLOR_F SyntaxTokenColor(const ThemePalette& p, TokenKind kind);
uint32_t ColorToTexArgb(D2D1_COLOR_F c);

}  // namespace mr
