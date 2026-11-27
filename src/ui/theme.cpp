#include "ui/theme.h"

namespace mr {

namespace {

constexpr D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        a
    };
}

ThemePalette MakeLightPalette() {
    ThemePalette p;
    p.mode = ThemeMode::Light;
    p.is_dark = false;
    p.titlebar_caption_rgb = 0xF8FAFC;
    p.titlebar_text_rgb = 0x1E293B;

    p.bg_canvas = Hex(0xFFFFFF);
    p.bg_toolbar = Hex(0xF8FAFC);
    p.bg_sidebar = Hex(0xF6F8FA);
    p.bg_elevated = Hex(0xFFFFFF);
    p.bg_hover = Hex(0xEAEFF5);
    p.bg_active_item = Hex(0xE8F1FD);
    p.border = Hex(0xE2E8F0);
    p.border_strong = Hex(0xCBD5E1);

    p.text_primary = Hex(0x1F2328);
    p.text_secondary = Hex(0x59636E);
    p.text_muted = Hex(0x8C959F);
    p.accent = Hex(0x0969DA);
    p.accent_soft = Hex(0x0969DA, 0.12f);

    p.link = Hex(0x0969DA);
    p.code_inline_bg = Hex(0xF1F5F9);
    p.code_inline_border = Hex(0xE2E8F0);
    p.code_inline_text = Hex(0xC7254E);

    p.code_block_bg = Hex(0xF6F8FA);
    p.code_block_header_bg = Hex(0xEDF2F7);
    p.code_block_border = Hex(0xE2E8F0);

    p.blockquote_bar = Hex(0xCBD5E1);
    p.blockquote_bg = Hex(0xF8FAFC);
    p.blockquote_text = Hex(0x57606A);

    p.math_block_bg = Hex(0xF8FAFC);
    p.math_block_border = Hex(0xE2E8F0);

    p.table_header_bg = Hex(0xF1F5F9);
    p.table_stripe_bg = Hex(0xF8FAFC);
    p.table_border = Hex(0xDCE3EB);

    p.hr_color = Hex(0xE2E8F0);
    p.highlight_bg = Hex(0xFEF08A, 0.85f);
    p.selection_bg = Hex(0x3B82F6, 0.24f);
    p.search_match_bg = Hex(0xFACC15, 0.42f);
    p.search_active_bg = Hex(0xFB923C, 0.65f);
    p.scrollbar_thumb = Hex(0x94A3B8, 0.42f);
    p.scrollbar_thumb_hover = Hex(0x64748B, 0.68f);

    p.adm_note_accent = Hex(0x0969DA);
    p.adm_note_bg = Hex(0xEFF6FF);
    p.adm_tip_accent = Hex(0x1A7F37);
    p.adm_tip_bg = Hex(0xECFDF5);
    p.adm_important_accent = Hex(0x8250DF);
    p.adm_important_bg = Hex(0xF5F3FF);
    p.adm_warning_accent = Hex(0x9A6700);
    p.adm_warning_bg = Hex(0xFFFBEB);
    p.adm_caution_accent = Hex(0xCF222E);
    p.adm_caution_bg = Hex(0xFEF2F2);

    p.syn_normal = Hex(0x24292F);
    p.syn_keyword = Hex(0xCF222E);
    p.syn_type = Hex(0x953800);
    p.syn_string = Hex(0x0A3069);
    p.syn_comment = Hex(0x6E7781);
    p.syn_number = Hex(0x0550AE);
    p.syn_preproc = Hex(0x8250DF);
    p.syn_function = Hex(0x6639BA);
    p.syn_operator = Hex(0x0550AE);
    return p;
}

ThemePalette MakeDarkPalette() {
    ThemePalette p;
    p.mode = ThemeMode::Dark;
    p.is_dark = true;
    p.titlebar_caption_rgb = 0x161B22;
    p.titlebar_text_rgb = 0xE6EDF3;

    p.bg_canvas = Hex(0x0F1117);
    p.bg_toolbar = Hex(0x161B22);
    p.bg_sidebar = Hex(0x131720);
    p.bg_elevated = Hex(0x1C2128);
    p.bg_hover = Hex(0x262C36);
    p.bg_active_item = Hex(0x1E2D44);
    p.border = Hex(0x272E3B);
    p.border_strong = Hex(0x3B4454);

    p.text_primary = Hex(0xE6EDF3);
    p.text_secondary = Hex(0x9198A1);
    p.text_muted = Hex(0x6E7681);
    p.accent = Hex(0x58A6FF);
    p.accent_soft = Hex(0x58A6FF, 0.15f);

    p.link = Hex(0x58A6FF);
    p.code_inline_bg = Hex(0x21262D);
    p.code_inline_border = Hex(0x30363D);
    p.code_inline_text = Hex(0xFF8B94);

    p.code_block_bg = Hex(0x161B22);
    p.code_block_header_bg = Hex(0x1C222B);
    p.code_block_border = Hex(0x2B323D);

    p.blockquote_bar = Hex(0x3B4454);
    p.blockquote_bg = Hex(0x151A23);
    p.blockquote_text = Hex(0x9DA7B3);

    p.math_block_bg = Hex(0x151A23);
    p.math_block_border = Hex(0x272E3B);

    p.table_header_bg = Hex(0x1A2029);
    p.table_stripe_bg = Hex(0x141820);
    p.table_border = Hex(0x2B323D);

    p.hr_color = Hex(0x2A313C);
    p.highlight_bg = Hex(0xEAB308, 0.34f);
    p.selection_bg = Hex(0x388BFD, 0.32f);
    p.search_match_bg = Hex(0xFACC15, 0.32f);
    p.search_active_bg = Hex(0xF97316, 0.58f);
    p.scrollbar_thumb = Hex(0x6E7681, 0.45f);
    p.scrollbar_thumb_hover = Hex(0x8B949E, 0.72f);

    p.adm_note_accent = Hex(0x58A6FF);
    p.adm_note_bg = Hex(0x132238);
    p.adm_tip_accent = Hex(0x3FB950);
    p.adm_tip_bg = Hex(0x12261C);
    p.adm_important_accent = Hex(0xA371F7);
    p.adm_important_bg = Hex(0x211936);
    p.adm_warning_accent = Hex(0xD29922);
    p.adm_warning_bg = Hex(0x2B2111);
    p.adm_caution_accent = Hex(0xF85149);
    p.adm_caution_bg = Hex(0x2E1619);

    p.syn_normal = Hex(0xE6EDF3);
    p.syn_keyword = Hex(0xFF7B72);
    p.syn_type = Hex(0xFFA657);
    p.syn_string = Hex(0xA5D6FF);
    p.syn_comment = Hex(0x8B949E);
    p.syn_number = Hex(0x79C0FF);
    p.syn_preproc = Hex(0xD2A8FF);
    p.syn_function = Hex(0xD2A8FF);
    p.syn_operator = Hex(0x79C0FF);
    return p;
}

ThemePalette MakeSepiaPalette() {
    ThemePalette p;
    p.mode = ThemeMode::Sepia;
    p.is_dark = false;
    p.titlebar_caption_rgb = 0xF4ECD8;
    p.titlebar_text_rgb = 0x3B2F23;

    p.bg_canvas = Hex(0xFBF5E9);
    p.bg_toolbar = Hex(0xF4ECD8);
    p.bg_sidebar = Hex(0xF2E8D2);
    p.bg_elevated = Hex(0xFBF5E9);
    p.bg_hover = Hex(0xE7DBC1);
    p.bg_active_item = Hex(0xE8D8B8);
    p.border = Hex(0xE0D3B8);
    p.border_strong = Hex(0xCBBAA0);

    p.text_primary = Hex(0x3B2F23);
    p.text_secondary = Hex(0x6E5D4B);
    p.text_muted = Hex(0x968572);
    p.accent = Hex(0x9E5A20);
    p.accent_soft = Hex(0x9E5A20, 0.13f);

    p.link = Hex(0x9E5A20);
    p.code_inline_bg = Hex(0xEFE4CD);
    p.code_inline_border = Hex(0xDFD0B2);
    p.code_inline_text = Hex(0x9A3412);

    p.code_block_bg = Hex(0xF3EAD6);
    p.code_block_header_bg = Hex(0xEADFC7);
    p.code_block_border = Hex(0xDFD0B2);

    p.blockquote_bar = Hex(0xC5B293);
    p.blockquote_bg = Hex(0xF5EEDC);
    p.blockquote_text = Hex(0x665543);

    p.math_block_bg = Hex(0xF5EEDC);
    p.math_block_border = Hex(0xE0D3B8);

    p.table_header_bg = Hex(0xEFE4CD);
    p.table_stripe_bg = Hex(0xF7F0E0);
    p.table_border = Hex(0xDCD0B6);

    p.hr_color = Hex(0xDFD0B2);
    p.highlight_bg = Hex(0xFDE047, 0.65f);
    p.selection_bg = Hex(0xB45309, 0.22f);
    p.search_match_bg = Hex(0xF59E0B, 0.35f);
    p.search_active_bg = Hex(0xEA580C, 0.55f);
    p.scrollbar_thumb = Hex(0xA8967E, 0.45f);
    p.scrollbar_thumb_hover = Hex(0x826E55, 0.72f);

    p.adm_note_accent = Hex(0x2563EB);
    p.adm_note_bg = Hex(0xEDEFF7);
    p.adm_tip_accent = Hex(0x15803D);
    p.adm_tip_bg = Hex(0xEAF2E6);
    p.adm_important_accent = Hex(0x7C3AED);
    p.adm_important_bg = Hex(0xF2ECF7);
    p.adm_warning_accent = Hex(0xB45309);
    p.adm_warning_bg = Hex(0xF6ECD5);
    p.adm_caution_accent = Hex(0xB91C1C);
    p.adm_caution_bg = Hex(0xF7E8E3);

    p.syn_normal = Hex(0x3B2F23);
    p.syn_keyword = Hex(0xB42318);
    p.syn_type = Hex(0x9A4B00);
    p.syn_string = Hex(0x1E4E79);
    p.syn_comment = Hex(0x7E6E5B);
    p.syn_number = Hex(0x0369A1);
    p.syn_preproc = Hex(0x6D28D9);
    p.syn_function = Hex(0x6D28D9);
    p.syn_operator = Hex(0x0369A1);
    return p;
}

}  // namespace

const ThemePalette& GetThemePalette(ThemeMode mode) {
    static const ThemePalette kLight = MakeLightPalette();
    static const ThemePalette kDark = MakeDarkPalette();
    static const ThemePalette kSepia = MakeSepiaPalette();
    switch (mode) {
        case ThemeMode::Dark: return kDark;
        case ThemeMode::Sepia: return kSepia;
        default: return kLight;
    }
}

D2D1_COLOR_F SyntaxTokenColor(const ThemePalette& p, TokenKind kind) {
    switch (kind) {
        case TokenKind::Keyword: return p.syn_keyword;
        case TokenKind::Type: return p.syn_type;
        case TokenKind::String: return p.syn_string;
        case TokenKind::Comment: return p.syn_comment;
        case TokenKind::Number: return p.syn_number;
        case TokenKind::Preprocessor: return p.syn_preproc;
        case TokenKind::Function: return p.syn_function;
        case TokenKind::Operator: return p.syn_operator;
        default: return p.syn_normal;
    }
}

uint32_t ColorToTexArgb(D2D1_COLOR_F c) {
    auto byte_of = [](float v) -> uint32_t {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return (byte_of(c.a) << 24) | (byte_of(c.r) << 16) | (byte_of(c.g) << 8) | byte_of(c.b);
}

}  // namespace mr
