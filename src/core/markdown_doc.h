#pragma once

#include "core/syntax_hl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mr {

enum class TextAlign : uint8_t {
    Default = 0,
    Left,
    Center,
    Right
};

enum class AdmonitionKind : uint8_t {
    None = 0,
    Note,
    Tip,
    Important,
    Warning,
    Caution
};

struct InlineRun {
    enum class Kind : uint8_t {
        Text = 0,
        LineBreak,
        InlineCode,
        InlineMath,
        InlineImage,
        FootnoteRef
    };

    Kind kind = Kind::Text;
    std::wstring text;
    std::wstring url;
    std::wstring title;
    std::wstring link_href;

    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool underline = false;
    bool highlight = false;
    bool superscript = false;
    bool subscript = false;
    bool is_link = false;
};

struct ParagraphData {
    std::vector<InlineRun> runs;
    std::wstring plain_text;  // Concatenated searchable/copyable text
};

struct TableCell {
    TextAlign align = TextAlign::Default;
    ParagraphData content;
};

struct TableRow {
    bool is_header = false;
    int source_line = 0;
    std::vector<TableCell> cells;
};

struct Block;

struct ListItem {
    bool is_task = false;
    bool task_checked = false;
    int source_line = 0;
    std::vector<Block> children;
};

struct Block {
    enum class Kind : uint8_t {
        Heading = 0,
        Paragraph,
        CodeBlock,
        DisplayMath,
        Image,
        ThematicBreak,
        BlockQuote,
        Admonition,
        List,
        Table,
        FootnoteDef
    };

    Kind kind = Kind::Paragraph;
    int source_line = 0;

    // Heading
    int heading_level = 1;
    int heading_id = -1;
    std::string slug;

    // Paragraph / Heading inline content
    ParagraphData inline_content;

    // CodeBlock
    std::string code_lang;
    std::wstring code_text;
    std::vector<SyntaxToken> code_tokens;

    // DisplayMath
    std::wstring math_tex;

    // Image
    std::wstring image_src;
    std::wstring image_alt;
    std::wstring image_title;
    std::wstring image_link_href;

    // BlockQuote / Admonition / FootnoteDef
    AdmonitionKind admonition_kind = AdmonitionKind::None;
    std::wstring footnote_label;
    uint32_t footnote_id = 0;
    std::vector<Block> children;

    // List
    bool list_ordered = false;
    uint32_t list_start = 1;
    bool list_tight = false;
    std::vector<ListItem> list_items;

    // Table
    uint32_t table_cols = 0;
    std::vector<TableRow> table_rows;
};

struct OutlineEntry {
    int level = 1;
    int heading_id = 0;
    std::wstring title;
    std::string slug;
    int source_line = 0;
};

struct DocumentStats {
    size_t word_count = 0;
    size_t char_count = 0;
    size_t heading_count = 0;
    size_t math_count = 0;
    size_t code_block_count = 0;
    size_t image_count = 0;
    int reading_minutes = 1;
};

struct MarkdownDocument {
    std::wstring file_path;
    std::wstring base_dir;
    std::wstring doc_title;
    std::vector<Block> blocks;
    std::vector<OutlineEntry> outline;
    DocumentStats stats;
};

// Parses UTF-8 Markdown into a structured AST ready for layout and rendering.
MarkdownDocument ParseMarkdownDocument(const std::string& utf8_markdown, const std::wstring& file_path = L"");

// Resolves a relative or URI image/link path against base_dir.
std::wstring ResolveDocumentResourcePath(const std::wstring& base_dir, const std::wstring& raw_src);

}  // namespace mr
