#include "core/markdown_doc.h"
#include "core/types.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <unordered_map>
#include <windows.h>

extern "C" {
#include "md4c.h"
}

namespace mr {

namespace {

// Normalizes \[ ... \] and \( ... \) math delimiters outside code fences and inline code spans,
// and collapses blank lines inside standalone $$ ... $$ blocks so MD4C parses multi-line display math cleanly.
// Simultaneously records out_line_map[prep_line] = original 0-based line index.
static std::string PreprocessMarkdownMathWithLineMap(const std::string& input, std::vector<int>& out_line_map) {
    std::string out;
    out.reserve(input.size() + 64);
    out_line_map.clear();
    out_line_map.push_back(0);

    size_t i = 0;
    const size_t n = input.size();
    int orig_line = 0;
    bool in_fence = false;
    char fence_char = 0;
    size_t fence_len = 0;
    bool in_display_dollar_fence = false;
    bool in_bracket_display_fence = false;

    auto is_line_start = [&](size_t pos) {
        return pos == 0 || input[pos - 1] == '\n';
    };

    auto append_and_track = [&](size_t start, size_t len) {
        out.append(input, start, len);
        for (size_t k = 0; k < len; ++k) {
            if (input[start + k] == '\n') {
                ++orig_line;
                out_line_map.push_back(orig_line);
            }
        }
    };

    while (i < n) {
        if (is_line_start(i)) {
            // Check code fence line (``` or ~~~)
            size_t ws = i;
            while (ws < n && (input[ws] == ' ' || input[ws] == '\t') && (ws - i) < 4) ++ws;
            if (!in_display_dollar_fence && !in_bracket_display_fence &&
                ws < n && (input[ws] == '`' || input[ws] == '~')) {
                char fc = input[ws];
                size_t fl = 0;
                while (ws + fl < n && input[ws + fl] == fc) ++fl;
                if (fl >= 3) {
                    if (!in_fence) {
                        in_fence = true;
                        fence_char = fc;
                        fence_len = fl;
                    } else if (fc == fence_char && fl >= fence_len) {
                        // Ensure rest of line is whitespace
                        size_t k = ws + fl;
                        while (k < n && (input[k] == ' ' || input[k] == '\t' || input[k] == '\r')) ++k;
                        if (k == n || input[k] == '\n') {
                            in_fence = false;
                        }
                    }
                }
            }

            if (in_fence) {
                size_t eol = input.find('\n', i);
                if (eol == std::string::npos) {
                    append_and_track(i, n - i);
                    break;
                }
                append_and_track(i, eol - i + 1);
                i = eol + 1;
                continue;
            }

            // Standalone $$ or \[ or \] line
            size_t line_end = input.find('\n', i);
            size_t line_len = (line_end == std::string::npos) ? (n - i) : (line_end - i);
            std::string line = input.substr(i, line_len);
            std::string trimmed = TrimAscii(line);

            if (!in_bracket_display_fence && trimmed == "$$") {
                in_display_dollar_fence = !in_display_dollar_fence;
                out.append(line);
                if (line_end != std::string::npos) {
                    out.push_back('\n');
                    ++orig_line;
                    out_line_map.push_back(orig_line);
                }
                i = (line_end == std::string::npos) ? n : (line_end + 1);
                continue;
            }
            if (!in_display_dollar_fence && !in_bracket_display_fence && trimmed == "\\[") {
                in_bracket_display_fence = true;
                out.append("$$");
                if (line_end != std::string::npos) {
                    out.push_back('\n');
                    ++orig_line;
                    out_line_map.push_back(orig_line);
                }
                i = (line_end == std::string::npos) ? n : (line_end + 1);
                continue;
            }
            if (in_bracket_display_fence && trimmed == "\\]") {
                in_bracket_display_fence = false;
                out.append("$$");
                if (line_end != std::string::npos) {
                    out.push_back('\n');
                    ++orig_line;
                    out_line_map.push_back(orig_line);
                }
                i = (line_end == std::string::npos) ? n : (line_end + 1);
                continue;
            }
            if ((in_display_dollar_fence || in_bracket_display_fence) && trimmed.empty()) {
                // Skip blank lines inside standalone display math block so MD4C keeps it as one math span
                if (line_end != std::string::npos) {
                    ++orig_line;
                    out_line_map.back() = orig_line;
                }
                i = (line_end == std::string::npos) ? n : (line_end + 1);
                continue;
            }
        }

        if (in_fence || in_display_dollar_fence || in_bracket_display_fence) {
            char ch = input[i++];
            out.push_back(ch);
            if (ch == '\n') {
                ++orig_line;
                out_line_map.push_back(orig_line);
            }
            continue;
        }

        // Skip inline code spans `...`
        if (input[i] == '`') {
            size_t ticks = 0;
            while (i + ticks < n && input[i + ticks] == '`') ++ticks;
            size_t search = i + ticks;
            size_t close_pos = std::string::npos;
            while (search < n) {
                if (input[search] == '`') {
                    size_t cticks = 0;
                    while (search + cticks < n && input[search + cticks] == '`') ++cticks;
                    if (cticks == ticks) {
                        close_pos = search + cticks;
                        break;
                    }
                    search += cticks;
                } else {
                    ++search;
                }
            }
            if (close_pos != std::string::npos) {
                append_and_track(i, close_pos - i);
                i = close_pos;
                continue;
            }
            append_and_track(i, ticks);
            i += ticks;
            continue;
        }

        // Convert inline \( ... \) and single-line \[ ... \] (outside escaped \\)
        if (input[i] == '\\' && i + 1 < n && (i == 0 || input[i - 1] != '\\')) {
            if (input[i + 1] == '(') {
                size_t close = input.find("\\)", i + 2);
                size_t nl = input.find('\n', i + 2);
                if (close != std::string::npos && (nl == std::string::npos || close < nl)) {
                    out.push_back('$');
                    out.append(input, i + 2, close - (i + 2));
                    out.push_back('$');
                    i = close + 2;
                    continue;
                }
            } else if (input[i + 1] == '[') {
                size_t close = input.find("\\]", i + 2);
                size_t nl = input.find('\n', i + 2);
                if (close != std::string::npos && (nl == std::string::npos || close < nl)) {
                    out.append("$$");
                    out.append(input, i + 2, close - (i + 2));
                    out.append("$$");
                    i = close + 2;
                    continue;
                }
            }
        }

        char ch = input[i++];
        out.push_back(ch);
        if (ch == '\n') {
            ++orig_line;
            out_line_map.push_back(orig_line);
        }
    }

    return out;
}

std::string PreprocessMarkdownMath(const std::string& input) {
    std::vector<int> line_map;
    return PreprocessMarkdownMathWithLineMap(input, line_map);
}

std::string AttrToUtf8(const MD_ATTRIBUTE& attr) {
    if (!attr.text || attr.size == 0) return {};
    std::string out;
    if (!attr.substr_types || !attr.substr_offsets) {
        out.assign(attr.text, attr.size);
        return out;
    }
    for (unsigned idx = 0;; ++idx) {
        MD_OFFSET beg = attr.substr_offsets[idx];
        if (beg >= attr.size) break;
        MD_OFFSET end = attr.substr_offsets[idx + 1];
        MD_TEXTTYPE tt = attr.substr_types[idx];
        if (tt == MD_TEXT_ENTITY) {
            out.append(DecodeHtmlEntityUtf8(attr.text + beg, end - beg));
        } else if (tt == MD_TEXT_NULLCHAR) {
            out.append("\xEF\xBF\xBD");
        } else {
            out.append(attr.text + beg, end - beg);
        }
    }
    return out;
}

TextAlign ConvertAlign(MD_ALIGN a) {
    switch (a) {
        case MD_ALIGN_LEFT: return TextAlign::Left;
        case MD_ALIGN_CENTER: return TextAlign::Center;
        case MD_ALIGN_RIGHT: return TextAlign::Right;
        default: return TextAlign::Default;
    }
}

AdmonitionKind ParseAdmonitionKind(const std::string& type_str) {
    std::string t = ToLowerAscii(TrimAscii(type_str));
    if (t == "note") return AdmonitionKind::Note;
    if (t == "tip") return AdmonitionKind::Tip;
    if (t == "important") return AdmonitionKind::Important;
    if (t == "warning") return AdmonitionKind::Warning;
    if (t == "caution") return AdmonitionKind::Caution;
    return AdmonitionKind::None;
}

// Lightweight extractor for HTML <img> tags and <br> in raw HTML blocks/spans.
bool ExtractHtmlAttr(const std::string& tag, const char* attr_name, std::string& out_val) {
    std::string lower = ToLowerAscii(tag);
    std::string key = std::string(attr_name) + "=";
    size_t pos = 0;
    while ((pos = lower.find(key, pos)) != std::string::npos) {
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(lower[pos - 1]))) {
            pos += key.size();
            continue;
        }
        size_t val_start = pos + key.size();
        while (val_start < tag.size() && std::isspace(static_cast<unsigned char>(tag[val_start]))) ++val_start;
        if (val_start >= tag.size()) return false;
        char q = tag[val_start];
        if (q == '"' || q == '\'') {
            size_t end = tag.find(q, val_start + 1);
            if (end == std::string::npos) return false;
            out_val = tag.substr(val_start + 1, end - val_start - 1);
            return true;
        } else {
            size_t end = val_start;
            while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) && tag[end] != '>') ++end;
            out_val = tag.substr(val_start, end - val_start);
            return true;
        }
    }
    return false;
}

void FinalizeParagraphPlainText(ParagraphData& p) {
    p.plain_text.clear();
    for (const auto& run : p.runs) {
        if (run.kind == InlineRun::Kind::LineBreak) {
            p.plain_text.push_back(L'\n');
        } else if (run.kind == InlineRun::Kind::InlineImage) {
            if (!run.text.empty()) p.plain_text.append(run.text);
        } else {
            p.plain_text.append(run.text);
        }
    }
}

void CountStatsInParagraph(const ParagraphData& p, DocumentStats& stats) {
    for (const auto& run : p.runs) {
        if (run.kind == InlineRun::Kind::InlineMath) {
            ++stats.math_count;
            continue;
        }
        if (run.kind == InlineRun::Kind::InlineImage) {
            ++stats.image_count;
            continue;
        }
        bool in_word = false;
        for (wchar_t ch : run.text) {
            if (!std::iswspace(ch)) ++stats.char_count;
            // Count CJK ideographs individually as reading units, and Latin runs as words.
            if (ch >= 0x4E00 && ch <= 0x9FFF) {
                ++stats.word_count;
                in_word = false;
            } else if (std::iswalnum(ch)) {
                if (!in_word) {
                    ++stats.word_count;
                    in_word = true;
                }
            } else {
                in_word = false;
            }
        }
    }
}

void CollectStatsFromBlocks(const std::vector<Block>& blocks, DocumentStats& stats) {
    for (const auto& b : blocks) {
        switch (b.kind) {
            case Block::Kind::Heading:
                ++stats.heading_count;
                CountStatsInParagraph(b.inline_content, stats);
                break;
            case Block::Kind::Paragraph:
                CountStatsInParagraph(b.inline_content, stats);
                break;
            case Block::Kind::CodeBlock:
                ++stats.code_block_count;
                stats.char_count += b.code_text.size();
                break;
            case Block::Kind::DisplayMath:
                ++stats.math_count;
                break;
            case Block::Kind::Image:
                ++stats.image_count;
                break;
            case Block::Kind::BlockQuote:
            case Block::Kind::Admonition:
            case Block::Kind::FootnoteDef:
                CollectStatsFromBlocks(b.children, stats);
                break;
            case Block::Kind::List:
                for (const auto& item : b.list_items) {
                    CollectStatsFromBlocks(item.children, stats);
                }
                break;
            case Block::Kind::Table:
                for (const auto& row : b.table_rows) {
                    for (const auto& cell : row.cells) {
                        CountStatsInParagraph(cell.content, stats);
                    }
                }
                break;
            default:
                break;
        }
    }
}

struct InlineStyleState {
    int bold = 0;
    int italic = 0;
    int strike = 0;
    int underline = 0;
    int highlight = 0;
    int superscript = 0;
    int subscript = 0;
    int code = 0;
    int math_inline = 0;
    int math_display = 0;
    std::vector<std::pair<std::wstring, std::wstring>> link_stack;
    struct ImgCapture {
        std::wstring src;
        std::wstring title;
        std::wstring alt;
    };
    std::vector<ImgCapture> img_stack;
};

struct ContainerFrame {
    enum class Kind {
        Root,
        BlockQuote,
        Admonition,
        List,
        ListItem,
        Table,
        FootnoteDef
    };

    Kind kind = Kind::Root;
    Block block;
    ListItem list_item;
};

struct ParseBuilder {
    MarkdownDocument doc;
    std::vector<ContainerFrame> stack;
    std::unordered_map<std::string, int> slug_counts;

    const char* prep_beg = nullptr;
    size_t prep_size = 0;
    std::vector<size_t> prep_line_starts;
    std::vector<int> prep_to_orig_line;
    int last_orig_line = 0;
    int current_block_line = -1;
    int current_math_line = -1;

    bool in_paragraph_like = false;
    bool in_heading = false;
    bool in_code_block = false;
    bool in_html_block = false;
    bool in_table_cell = false;

    int current_heading_level = 1;
    std::string current_code_lang;
    std::string current_code_buf;
    std::string current_html_buf;

    // A paragraph can split if it contains MD_SPAN_LATEXMATH_DISPLAY or standalone images.
    enum class SegKind { Paragraph, DisplayMath };
    struct Segment {
        SegKind kind = SegKind::Paragraph;
        ParagraphData para;
        std::wstring math_tex;
        int source_line = -1;
    };
    std::vector<Segment> para_segments;
    std::string active_math_buf;
    InlineStyleState style;

    TableRow current_table_row;
    TableCell current_table_cell;
    bool current_table_in_head = false;

    ParseBuilder() {
        ContainerFrame root;
        root.kind = ContainerFrame::Kind::Root;
        stack.push_back(std::move(root));
    }

    int ResolveOrigLine(const char* ptr) const {
        if (!prep_beg || !ptr || ptr < prep_beg || ptr > prep_beg + prep_size || prep_line_starts.empty()) {
            return -1;
        }
        size_t off = static_cast<size_t>(ptr - prep_beg);
        auto it = std::upper_bound(prep_line_starts.begin(), prep_line_starts.end(), off);
        size_t prep_line = (it == prep_line_starts.begin())
            ? 0
            : static_cast<size_t>((it - prep_line_starts.begin()) - 1);
        if (prep_to_orig_line.empty()) return static_cast<int>(prep_line);
        if (prep_line >= prep_to_orig_line.size()) return prep_to_orig_line.back();
        return prep_to_orig_line[prep_line];
    }

    void StampSourceLine(int line) {
        if (line < 0) return;
        last_orig_line = std::max(last_orig_line, line);
        for (auto& f : stack) {
            if (f.kind == ContainerFrame::Kind::ListItem) {
                if (f.list_item.source_line < 0) f.list_item.source_line = line;
            } else if (f.kind != ContainerFrame::Kind::Root) {
                if (f.block.source_line < 0) f.block.source_line = line;
            }
        }
        if ((in_heading || in_code_block || in_html_block) && current_block_line < 0) {
            current_block_line = line;
        }
        if (in_table_cell && current_table_row.source_line < 0) {
            current_table_row.source_line = line;
        }
        if (!in_table_cell) {
            if (style.math_display > 0 && current_math_line < 0) {
                current_math_line = line;
            }
            if (!para_segments.empty() && para_segments.back().kind == SegKind::Paragraph &&
                para_segments.back().source_line < 0) {
                para_segments.back().source_line = line;
            }
        }
    }

    void AppendBlockToParent(Block&& b) {
        ContainerFrame& top = stack.back();
        switch (top.kind) {
            case ContainerFrame::Kind::Root:
                doc.blocks.push_back(std::move(b));
                break;
            case ContainerFrame::Kind::BlockQuote:
            case ContainerFrame::Kind::Admonition:
            case ContainerFrame::Kind::FootnoteDef:
                top.block.children.push_back(std::move(b));
                break;
            case ContainerFrame::Kind::ListItem:
                top.list_item.children.push_back(std::move(b));
                break;
            default:
                doc.blocks.push_back(std::move(b));
                break;
        }
    }

    ParagraphData& CurrentParagraph() {
        if (in_table_cell) return current_table_cell.content;
        if (para_segments.empty() || para_segments.back().kind != SegKind::Paragraph) {
            para_segments.push_back({SegKind::Paragraph, {}, {}, -1});
        }
        return para_segments.back().para;
    }

    void AppendTextToCurrentParagraph(const std::wstring& wtext, InlineRun::Kind kind = InlineRun::Kind::Text) {
        if (wtext.empty() && kind == InlineRun::Kind::Text) return;
        ParagraphData& p = CurrentParagraph();
        InlineRun run;
        run.kind = kind;
        run.text = wtext;
        run.bold = style.bold > 0;
        run.italic = style.italic > 0;
        run.strike = style.strike > 0;
        run.underline = style.underline > 0;
        run.highlight = style.highlight > 0;
        run.superscript = style.superscript > 0;
        run.subscript = style.subscript > 0;
        if (!style.link_stack.empty()) {
            run.is_link = true;
            run.url = style.link_stack.back().first;
            run.title = style.link_stack.back().second;
        }

        // Coalesce adjacent plain/styled runs with identical formatting.
        if (!p.runs.empty() && kind == InlineRun::Kind::Text) {
            InlineRun& prev = p.runs.back();
            if (prev.kind == InlineRun::Kind::Text &&
                prev.bold == run.bold && prev.italic == run.italic &&
                prev.strike == run.strike && prev.underline == run.underline &&
                prev.highlight == run.highlight && prev.superscript == run.superscript &&
                prev.subscript == run.subscript && prev.is_link == run.is_link &&
                prev.url == run.url) {
                prev.text.append(run.text);
                return;
            }
        }
        if (!p.runs.empty() && kind == InlineRun::Kind::InlineCode) {
            InlineRun& prev = p.runs.back();
            if (prev.kind == InlineRun::Kind::InlineCode &&
                prev.bold == run.bold && prev.italic == run.italic &&
                prev.is_link == run.is_link && prev.url == run.url) {
                prev.text.append(run.text);
                return;
            }
        }
        p.runs.push_back(std::move(run));
    }

    void FlushParagraphSegments() {
        for (auto& seg : para_segments) {
            int seg_line = (seg.source_line >= 0) ? seg.source_line : last_orig_line;
            if (seg.kind == SegKind::DisplayMath) {
                std::wstring trimmed = Utf8ToWide(TrimAscii(WideToUtf8(seg.math_tex)));
                if (!trimmed.empty()) {
                    Block mb;
                    mb.kind = Block::Kind::DisplayMath;
                    mb.source_line = seg_line;
                    mb.math_tex = std::move(trimmed);
                    AppendBlockToParent(std::move(mb));
                }
                continue;
            }

            // Trim trailing soft-break space if segment only had whitespace
            bool has_non_ws = false;
            for (const auto& r : seg.para.runs) {
                if (r.kind != InlineRun::Kind::Text) {
                    has_non_ws = true;
                    break;
                }
                for (wchar_t ch : r.text) {
                    if (!std::iswspace(ch)) {
                        has_non_ws = true;
                        break;
                    }
                }
                if (has_non_ws) break;
            }
            if (!has_non_ws) continue;

            // If a paragraph contains only a single image (ignoring whitespace), promote it to a centered Block::Kind::Image.
            std::vector<const InlineRun*> non_ws_runs;
            for (const auto& r : seg.para.runs) {
                if (r.kind == InlineRun::Kind::Text) {
                    bool ws = true;
                    for (wchar_t ch : r.text) {
                        if (!std::iswspace(ch)) { ws = false; break; }
                    }
                    if (ws) continue;
                } else if (r.kind == InlineRun::Kind::LineBreak) {
                    continue;
                }
                non_ws_runs.push_back(&r);
            }
            if (non_ws_runs.size() == 1 && non_ws_runs[0]->kind == InlineRun::Kind::InlineImage) {
                const InlineRun* img = non_ws_runs[0];
                Block ib;
                ib.kind = Block::Kind::Image;
                ib.source_line = seg_line;
                ib.image_src = img->url;
                ib.image_alt = img->text;
                ib.image_title = img->title;
                ib.image_link_href = img->link_href;
                AppendBlockToParent(std::move(ib));
                continue;
            }

            FinalizeParagraphPlainText(seg.para);
            Block pb;
            pb.kind = Block::Kind::Paragraph;
            pb.source_line = seg_line;
            pb.inline_content = std::move(seg.para);
            AppendBlockToParent(std::move(pb));
        }
        para_segments.clear();
    }

    void ProcessRawHtml(const std::string& html, bool is_block) {
        std::string trimmed = TrimAscii(html);
        if (trimmed.empty()) return;
        // Ignore HTML comments
        if (trimmed.rfind("<!--", 0) == 0) return;

        std::string lower = ToLowerAscii(trimmed);
        // Handle <br> / <br/>
        if (lower == "<br>" || lower == "<br/>" || lower == "<br />") {
            if (!is_block) {
                AppendTextToCurrentParagraph(L"\n", InlineRun::Kind::LineBreak);
            }
            return;
        }
        if (lower == "<hr>" || lower == "<hr/>" || lower == "<hr />") {
            if (is_block) {
                Block hb;
                hb.kind = Block::Kind::ThematicBreak;
                AppendBlockToParent(std::move(hb));
            }
            return;
        }
        // Extract <img src="..." alt="..."> tags from HTML snippets commonly used in GitHub READMEs
        size_t pos = 0;
        bool found_img = false;
        while ((pos = lower.find("<img", pos)) != std::string::npos) {
            size_t end = trimmed.find('>', pos);
            if (end == std::string::npos) break;
            std::string tag = trimmed.substr(pos, end - pos + 1);
            std::string src, alt, title;
            if (ExtractHtmlAttr(tag, "src", src) && !src.empty()) {
                ExtractHtmlAttr(tag, "alt", alt);
                ExtractHtmlAttr(tag, "title", title);
                if (is_block) {
                    Block ib;
                    ib.kind = Block::Kind::Image;
                    ib.image_src = Utf8ToWide(src);
                    ib.image_alt = Utf8ToWide(alt);
                    ib.image_title = Utf8ToWide(title);
                    AppendBlockToParent(std::move(ib));
                } else {
                    ParagraphData& p = CurrentParagraph();
                    InlineRun run;
                    run.kind = InlineRun::Kind::InlineImage;
                    run.url = Utf8ToWide(src);
                    run.text = Utf8ToWide(alt);
                    run.title = Utf8ToWide(title);
                    p.runs.push_back(std::move(run));
                }
                found_img = true;
            }
            pos = end + 1;
        }
        if (found_img) return;
        // Silently ignore structural HTML tags (<div>, <p align="center">, </p>, <details>, etc.)
    }
};

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* b = static_cast<ParseBuilder*>(userdata);
    if (type != MD_BLOCK_DOC && !b->in_table_cell && !b->para_segments.empty()) {
        b->FlushParagraphSegments();
    }
    switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_QUOTE: {
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::BlockQuote;
            f.block.kind = Block::Kind::BlockQuote;
            f.block.source_line = -1;
            b->stack.push_back(std::move(f));
            break;
        }
        case MD_BLOCK_ADMONITION: {
            auto* det = static_cast<MD_BLOCK_ADMONITION_DETAIL*>(detail);
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::Admonition;
            f.block.kind = Block::Kind::Admonition;
            f.block.source_line = -1;
            f.block.admonition_kind = det ? ParseAdmonitionKind(AttrToUtf8(det->type)) : AdmonitionKind::Note;
            if (f.block.admonition_kind == AdmonitionKind::None) f.block.admonition_kind = AdmonitionKind::Note;
            b->stack.push_back(std::move(f));
            break;
        }
        case MD_BLOCK_UL: {
            auto* det = static_cast<MD_BLOCK_UL_DETAIL*>(detail);
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::List;
            f.block.kind = Block::Kind::List;
            f.block.source_line = -1;
            f.block.list_ordered = false;
            f.block.list_tight = det ? (det->is_tight != 0) : false;
            b->stack.push_back(std::move(f));
            break;
        }
        case MD_BLOCK_OL: {
            auto* det = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::List;
            f.block.kind = Block::Kind::List;
            f.block.source_line = -1;
            f.block.list_ordered = true;
            f.block.list_start = det ? det->start : 1;
            f.block.list_tight = det ? (det->is_tight != 0) : false;
            b->stack.push_back(std::move(f));
            break;
        }
        case MD_BLOCK_LI: {
            auto* det = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::ListItem;
            f.list_item.source_line = -1;
            if (det && det->is_task) {
                f.list_item.is_task = true;
                f.list_item.task_checked = (det->task_mark == 'x' || det->task_mark == 'X');
            }
            b->stack.push_back(std::move(f));
            break;
        }
        case MD_BLOCK_HR: {
            Block hb;
            hb.kind = Block::Kind::ThematicBreak;
            hb.source_line = b->last_orig_line + 1;
            b->AppendBlockToParent(std::move(hb));
            break;
        }
        case MD_BLOCK_H: {
            auto* det = static_cast<MD_BLOCK_H_DETAIL*>(detail);
            b->in_heading = true;
            b->in_paragraph_like = true;
            b->current_block_line = -1;
            b->current_heading_level = det ? static_cast<int>(det->level) : 1;
            b->para_segments.clear();
            b->style = {};
            break;
        }
        case MD_BLOCK_P: {
            b->in_paragraph_like = true;
            b->para_segments.clear();
            b->style = {};
            break;
        }
        case MD_BLOCK_CODE: {
            auto* det = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            b->in_code_block = true;
            b->current_block_line = -1;
            b->current_code_lang = det ? TrimAscii(AttrToUtf8(det->lang)) : "";
            b->current_code_buf.clear();
            break;
        }
        case MD_BLOCK_HTML: {
            b->in_html_block = true;
            b->current_block_line = -1;
            b->current_html_buf.clear();
            break;
        }
        case MD_BLOCK_TABLE: {
            auto* det = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::Table;
            f.block.kind = Block::Kind::Table;
            f.block.source_line = -1;
            f.block.table_cols = det ? det->col_count : 0;
            b->stack.push_back(std::move(f));
            break;
        }
        case MD_BLOCK_THEAD:
            b->current_table_in_head = true;
            break;
        case MD_BLOCK_TBODY:
            b->current_table_in_head = false;
            break;
        case MD_BLOCK_TR:
            b->current_table_row = {};
            b->current_table_row.is_header = b->current_table_in_head;
            b->current_table_row.source_line = -1;
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            auto* det = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
            b->in_table_cell = true;
            b->current_table_cell = {};
            b->current_table_cell.align = det ? ConvertAlign(det->align) : TextAlign::Default;
            b->style = {};
            break;
        }
        case MD_BLOCK_FOOTNOTE_DEF: {
            auto* det = static_cast<MD_BLOCK_FOOTNOTE_DEF_DETAIL*>(detail);
            ContainerFrame f;
            f.kind = ContainerFrame::Kind::FootnoteDef;
            f.block.kind = Block::Kind::FootnoteDef;
            f.block.source_line = -1;
            f.block.footnote_id = det ? det->id : 0;
            f.block.footnote_label = det ? Utf8ToWide(AttrToUtf8(det->label)) : L"";
            b->stack.push_back(std::move(f));
            break;
        }
        default:
            break;
    }
    return 0;
}

int OnLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto* b = static_cast<ParseBuilder*>(userdata);
    switch (type) {
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_ADMONITION:
        case MD_BLOCK_FOOTNOTE_DEF: {
            if (b->stack.size() > 1) {
                Block blk = std::move(b->stack.back().block);
                if (blk.source_line < 0) blk.source_line = b->last_orig_line;
                b->stack.pop_back();
                b->AppendBlockToParent(std::move(blk));
            }
            break;
        }
        case MD_BLOCK_LI: {
            b->FlushParagraphSegments();
            if (b->stack.size() > 1) {
                ListItem item = std::move(b->stack.back().list_item);
                if (item.source_line < 0) item.source_line = b->last_orig_line;
                b->stack.pop_back();
                if (!b->stack.empty() && b->stack.back().kind == ContainerFrame::Kind::List) {
                    b->stack.back().block.list_items.push_back(std::move(item));
                }
            }
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
        case MD_BLOCK_TABLE: {
            if (b->stack.size() > 1) {
                Block blk = std::move(b->stack.back().block);
                if (blk.source_line < 0) blk.source_line = b->last_orig_line;
                b->stack.pop_back();
                b->AppendBlockToParent(std::move(blk));
            }
            break;
        }
        case MD_BLOCK_H: {
            ParagraphData para;
            if (!b->para_segments.empty() && b->para_segments[0].kind == ParseBuilder::SegKind::Paragraph) {
                para = std::move(b->para_segments[0].para);
            }
            b->para_segments.clear();
            FinalizeParagraphPlainText(para);

            std::string raw_slug = SlugifyHeading(WideToUtf8(para.plain_text));
            if (raw_slug.empty()) raw_slug = "section";
            int& count = b->slug_counts[raw_slug];
            std::string unique_slug = (count == 0) ? raw_slug : (raw_slug + "-" + std::to_string(count));
            ++count;

            int src_line = (b->current_block_line >= 0) ? b->current_block_line : b->last_orig_line;
            int hid = static_cast<int>(b->doc.outline.size());
            b->doc.outline.push_back({b->current_heading_level, hid, para.plain_text, unique_slug, src_line});
            if (b->doc.doc_title.empty() && b->current_heading_level == 1) {
                b->doc.doc_title = para.plain_text;
            }

            Block hb;
            hb.kind = Block::Kind::Heading;
            hb.source_line = src_line;
            hb.heading_level = b->current_heading_level;
            hb.heading_id = hid;
            hb.slug = unique_slug;
            hb.inline_content = std::move(para);
            b->AppendBlockToParent(std::move(hb));

            b->in_heading = false;
            b->in_paragraph_like = false;
            break;
        }
        case MD_BLOCK_P: {
            b->FlushParagraphSegments();
            b->in_paragraph_like = false;
            break;
        }
        case MD_BLOCK_CODE: {
            // Trim single trailing newline from code block
            while (!b->current_code_buf.empty() &&
                   (b->current_code_buf.back() == '\n' || b->current_code_buf.back() == '\r')) {
                b->current_code_buf.pop_back();
            }
            int code_line = (b->current_block_line >= 0) ? b->current_block_line : b->last_orig_line;
            if (!b->current_code_lang.empty() && code_line > 0) {
                --code_line;  // Opening ``` fence sits one line above first body token
            }
            std::string lang_lower = ToLowerAscii(b->current_code_lang);
            if (lang_lower == "math" || lang_lower == "katex") {
                std::wstring tex = Utf8ToWide(TrimAscii(b->current_code_buf));
                if (!tex.empty()) {
                    Block mb;
                    mb.kind = Block::Kind::DisplayMath;
                    mb.source_line = code_line;
                    mb.math_tex = std::move(tex);
                    b->AppendBlockToParent(std::move(mb));
                }
            } else {
                Block cb;
                cb.kind = Block::Kind::CodeBlock;
                cb.source_line = code_line;
                cb.code_lang = b->current_code_lang;
                cb.code_text = Utf8ToWide(b->current_code_buf);
                cb.code_tokens = HighlightCode(cb.code_text, cb.code_lang);
                b->AppendBlockToParent(std::move(cb));
            }
            b->current_code_buf.clear();
            b->in_code_block = false;
            break;
        }
        case MD_BLOCK_HTML: {
            b->ProcessRawHtml(b->current_html_buf, true);
            b->current_html_buf.clear();
            b->in_html_block = false;
            break;
        }
        case MD_BLOCK_TR: {
            if (b->current_table_row.source_line < 0) {
                b->current_table_row.source_line = b->last_orig_line;
            }
            if (!b->stack.empty() && b->stack.back().kind == ContainerFrame::Kind::Table) {
                b->stack.back().block.table_rows.push_back(std::move(b->current_table_row));
            }
            b->current_table_row = {};
            break;
        }
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            FinalizeParagraphPlainText(b->current_table_cell.content);
            b->current_table_row.cells.push_back(std::move(b->current_table_cell));
            b->current_table_cell = {};
            b->in_table_cell = false;
            break;
        }
        default:
            break;
    }
    return 0;
}

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* b = static_cast<ParseBuilder*>(userdata);
    switch (type) {
        case MD_SPAN_STRONG: b->style.bold++; break;
        case MD_SPAN_EM: b->style.italic++; break;
        case MD_SPAN_DEL: b->style.strike++; break;
        case MD_SPAN_U: b->style.underline++; break;
        case MD_SPAN_MARK: b->style.highlight++; break;
        case MD_SPAN_SUPERSCRIPT: b->style.superscript++; break;
        case MD_SPAN_SUBSCRIPT: b->style.subscript++; break;
        case MD_SPAN_CODE: b->style.code++; break;
        case MD_SPAN_A: {
            auto* det = static_cast<MD_SPAN_A_DETAIL*>(detail);
            std::wstring href = det ? Utf8ToWide(AttrToUtf8(det->href)) : L"";
            std::wstring title = det ? Utf8ToWide(AttrToUtf8(det->title)) : L"";
            b->style.link_stack.emplace_back(std::move(href), std::move(title));
            break;
        }
        case MD_SPAN_IMG: {
            auto* det = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
            InlineStyleState::ImgCapture cap;
            cap.src = det ? Utf8ToWide(AttrToUtf8(det->src)) : L"";
            cap.title = det ? Utf8ToWide(AttrToUtf8(det->title)) : L"";
            b->style.img_stack.push_back(std::move(cap));
            break;
        }
        case MD_SPAN_LATEXMATH:
            b->style.math_inline++;
            b->active_math_buf.clear();
            break;
        case MD_SPAN_LATEXMATH_DISPLAY:
            b->style.math_display++;
            b->current_math_line = -1;
            b->active_math_buf.clear();
            break;
        case MD_SPAN_FOOTNOTE_REF: {
            auto* det = static_cast<MD_SPAN_FOOTNOTE_REF_DETAIL*>(detail);
            if (det) {
                ParagraphData& p = b->CurrentParagraph();
                InlineRun run;
                run.kind = InlineRun::Kind::FootnoteRef;
                run.text = L"[" + std::to_wstring(det->id) + L"]";
                run.url = L"#fn-" + std::to_wstring(det->id);
                run.superscript = true;
                run.is_link = true;
                p.runs.push_back(std::move(run));
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

int OnLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* b = static_cast<ParseBuilder*>(userdata);
    switch (type) {
        case MD_SPAN_STRONG: b->style.bold = std::max(0, b->style.bold - 1); break;
        case MD_SPAN_EM: b->style.italic = std::max(0, b->style.italic - 1); break;
        case MD_SPAN_DEL: b->style.strike = std::max(0, b->style.strike - 1); break;
        case MD_SPAN_U: b->style.underline = std::max(0, b->style.underline - 1); break;
        case MD_SPAN_MARK: b->style.highlight = std::max(0, b->style.highlight - 1); break;
        case MD_SPAN_SUPERSCRIPT: b->style.superscript = std::max(0, b->style.superscript - 1); break;
        case MD_SPAN_SUBSCRIPT: b->style.subscript = std::max(0, b->style.subscript - 1); break;
        case MD_SPAN_CODE: b->style.code = std::max(0, b->style.code - 1); break;
        case MD_SPAN_A:
            if (!b->style.link_stack.empty()) {
                // If link wrapped a single standalone image in a paragraph, propagate href to InlineImage/Block
                std::wstring href = b->style.link_stack.back().first;
                b->style.link_stack.pop_back();
                if (!b->in_table_cell && !b->para_segments.empty() &&
                    b->para_segments.back().kind == ParseBuilder::SegKind::Paragraph) {
                    auto& runs = b->para_segments.back().para.runs;
                    if (!runs.empty() && runs.back().kind == InlineRun::Kind::InlineImage && runs.back().url != href) {
                        runs.back().is_link = true;
                        // Keep image src in url, link target in title? No, keep image src in url, store link in runs.back().title only if empty?
                        // Actually let's keep InlineImage clickable by storing link in runs.back().title if we distinguish or add field? Wait: InlineImage has url=src; let's preserve link_href!
                    }
                }
            }
            break;
        case MD_SPAN_IMG:
            if (!b->style.img_stack.empty()) {
                auto cap = std::move(b->style.img_stack.back());
                b->style.img_stack.pop_back();
                ParagraphData& p = b->CurrentParagraph();
                InlineRun run;
                run.kind = InlineRun::Kind::InlineImage;
                run.url = std::move(cap.src);
                run.text = std::move(cap.alt);
                run.title = std::move(cap.title);
                if (!b->style.link_stack.empty()) {
                    run.is_link = true;
                    run.link_href = b->style.link_stack.back().first;
                }
                p.runs.push_back(std::move(run));
            }
            break;
        case MD_SPAN_LATEXMATH: {
            b->style.math_inline = std::max(0, b->style.math_inline - 1);
            std::wstring tex = Utf8ToWide(TrimAscii(b->active_math_buf));
            b->active_math_buf.clear();
            if (!tex.empty()) {
                b->AppendTextToCurrentParagraph(tex, InlineRun::Kind::InlineMath);
            }
            break;
        }
        case MD_SPAN_LATEXMATH_DISPLAY: {
            b->style.math_display = std::max(0, b->style.math_display - 1);
            std::wstring tex = Utf8ToWide(TrimAscii(b->active_math_buf));
            b->active_math_buf.clear();
            if (!tex.empty()) {
                if (b->in_table_cell || b->in_heading) {
                    // Inside table cells or headings, render inline so table/heading geometry stays cohesive.
                    b->AppendTextToCurrentParagraph(tex, InlineRun::Kind::InlineMath);
                } else {
                    int mline = (b->current_math_line >= 0) ? b->current_math_line : b->last_orig_line;
                    b->para_segments.push_back({ParseBuilder::SegKind::DisplayMath, {}, std::move(tex), mline});
                }
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* b = static_cast<ParseBuilder*>(userdata);
    int orig_line = b->ResolveOrigLine(text);
    if (orig_line >= 0) {
        b->StampSourceLine(orig_line);
    }
    if (b->in_code_block) {
        b->current_code_buf.append(text, size);
        return 0;
    }
    if (b->in_html_block) {
        b->current_html_buf.append(text, size);
        return 0;
    }
    if (b->style.math_inline > 0 || b->style.math_display > 0) {
        if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR) {
            b->active_math_buf.push_back(' ');
        } else {
            b->active_math_buf.append(text, size);
        }
        return 0;
    }
    if (!b->style.img_stack.empty()) {
        b->style.img_stack.back().alt.append(Utf8ToWide(text, size));
        return 0;
    }

    switch (type) {
        case MD_TEXT_NORMAL:
            b->AppendTextToCurrentParagraph(Utf8ToWide(text, size),
                b->style.code > 0 ? InlineRun::Kind::InlineCode : InlineRun::Kind::Text);
            break;
        case MD_TEXT_CODE:
            b->AppendTextToCurrentParagraph(Utf8ToWide(text, size), InlineRun::Kind::InlineCode);
            break;
        case MD_TEXT_ENTITY: {
            std::string dec = DecodeHtmlEntityUtf8(text, size);
            b->AppendTextToCurrentParagraph(Utf8ToWide(dec),
                b->style.code > 0 ? InlineRun::Kind::InlineCode : InlineRun::Kind::Text);
            break;
        }
        case MD_TEXT_NULLCHAR:
            b->AppendTextToCurrentParagraph(L"\uFFFD", InlineRun::Kind::Text);
            break;
        case MD_TEXT_BR:
            b->AppendTextToCurrentParagraph(L"\n", InlineRun::Kind::LineBreak);
            break;
        case MD_TEXT_SOFTBR:
            b->AppendTextToCurrentParagraph(L" ", InlineRun::Kind::Text);
            break;
        case MD_TEXT_HTML:
            b->ProcessRawHtml(std::string(text, size), false);
            break;
        default:
            break;
    }
    return 0;
}

}  // namespace

MarkdownDocument ParseMarkdownDocument(const std::string& utf8_markdown, const std::wstring& file_path) {
    ParseBuilder builder;
    builder.doc.file_path = file_path;
    if (!file_path.empty()) {
        size_t sep = file_path.find_last_of(L"\\/");
        builder.doc.base_dir = (sep == std::wstring::npos) ? L"." : file_path.substr(0, sep);
        if (sep != std::wstring::npos) {
            builder.doc.doc_title = file_path.substr(sep + 1);
        } else {
            builder.doc.doc_title = file_path;
        }
    } else {
        builder.doc.doc_title = L"MarkReader";
    }

    std::string preprocessed = PreprocessMarkdownMathWithLineMap(utf8_markdown, builder.prep_to_orig_line);
    builder.prep_beg = preprocessed.data();
    builder.prep_size = preprocessed.size();
    builder.prep_line_starts.clear();
    builder.prep_line_starts.push_back(0);
    for (size_t i = 0; i < preprocessed.size(); ++i) {
        if (preprocessed[i] == '\n') {
            builder.prep_line_starts.push_back(i + 1);
        }
    }

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES |
                   MD_FLAG_STRIKETHROUGH |
                   MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS |
                   MD_FLAG_LATEXMATHSPANS |
                   MD_FLAG_ADMONITIONS |
                   MD_FLAG_FOOTNOTES |
                   MD_FLAG_HIGHLIGHT |
                   MD_FLAG_SUPERSCRIPTS;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    md_parse(preprocessed.data(), static_cast<MD_SIZE>(preprocessed.size()), &parser, &builder);

    CollectStatsFromBlocks(builder.doc.blocks, builder.doc.stats);
    // ~240 words / CJK characters per minute reading speed
    builder.doc.stats.reading_minutes = std::max(1, static_cast<int>((builder.doc.stats.word_count + 239) / 240));
    return std::move(builder.doc);
}

std::wstring ResolveDocumentResourcePath(const std::wstring& base_dir, const std::wstring& raw_src) {
    if (raw_src.empty()) return {};
    if (IsRemoteUrl(raw_src)) return raw_src;

    std::wstring src = raw_src;
    // Strip optional surrounding angle brackets
    if (src.size() >= 2 && src.front() == L'<' && src.back() == L'>') {
        src = src.substr(1, src.size() - 2);
    }
    // Strip file:/// prefix
    if (src.rfind(L"file:///", 0) == 0) {
        src = src.substr(8);
    } else if (src.rfind(L"file://", 0) == 0) {
        src = src.substr(7);
    }

    // Strip query/fragment on local file paths if present
    std::wstring decoded = UrlDecodeWide(src);
    for (wchar_t& ch : decoded) {
        if (ch == L'/') ch = L'\\';
    }
    if (IsWindowsAbsolutePath(decoded)) {
        return decoded;
    }
    if (base_dir.empty()) {
        return decoded;
    }
    std::wstring joined = base_dir;
    if (joined.back() != L'\\' && joined.back() != L'/') joined.push_back(L'\\');
    joined.append(decoded);

    wchar_t full[MAX_PATH * 4]{};
    DWORD len = GetFullPathNameW(joined.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (len > 0 && len < std::size(full)) {
        return std::wstring(full, len);
    }
    return joined;
}

}  // namespace mr
