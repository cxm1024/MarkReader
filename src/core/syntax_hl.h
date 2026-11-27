#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mr {

enum class TokenKind : uint8_t {
    Normal = 0,
    Keyword,
    Type,
    String,
    Comment,
    Number,
    Preprocessor,
    Function,
    Operator
};

struct SyntaxToken {
    uint32_t start = 0;   // UTF-16 code-unit offset
    uint32_t length = 0;  // UTF-16 code-unit length
    TokenKind kind = TokenKind::Normal;
};

// Tokenizes UTF-16 code text according to the normalized code fence language.
std::vector<SyntaxToken> HighlightCode(const std::wstring& code, const std::string& lang);

struct MarkdownSpanStyle {
    uint32_t start = 0;   // UTF-16 code-unit offset within line
    uint32_t length = 0;  // UTF-16 code-unit length
    TokenKind kind = TokenKind::Normal;
    bool bold = false;
    bool italic = false;
};

struct MarkdownFenceState {
    bool in_code_fence = false;
    wchar_t fence_char = 0;
    size_t fence_len = 0;
    std::string fence_lang;
    bool in_math_fence = false;
};

// Tokenizes a single Markdown source line and advances multi-line fence state.
std::vector<MarkdownSpanStyle> HighlightMarkdownLine(const std::wstring& line, MarkdownFenceState& state);

}  // namespace mr
