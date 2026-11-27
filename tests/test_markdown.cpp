#include "core/markdown_doc.h"
#include "core/sample_doc.h"
#include "core/syntax_hl.h"
#include "core/types.h"
#include "ui/source_editor.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace mr;

static void TestMarkdownOutlineAndSlugs() {
    const std::string md = R"(
# Title One
## Sub Section
### Sub Section
## 中文标题 测试
)";
    auto doc = ParseMarkdownDocument(md, L"D:\\docs\\test.md");
    assert(doc.outline.size() == 4);
    assert(doc.outline[0].level == 1 && doc.outline[0].slug == "title-one");
    assert(doc.outline[1].level == 2 && doc.outline[1].slug == "sub-section");
    assert(doc.outline[2].level == 3 && doc.outline[2].slug == "sub-section-1");
    assert(doc.outline[3].level == 2 && doc.outline[3].title == L"中文标题 测试");
}

static void TestInlineAndDisplayMath() {
    const std::string md = R"(
Inline $E = mc^2$ and \(a^2 + b^2 = c^2\) inside text.

$$
\int_0^\infty e^{-x^2} dx = \frac{\sqrt{\pi}}{2}
$$

\[
A = \begin{pmatrix} 1 & 0 \\ 0 & 1 \end{pmatrix}
\]

```math
\sum_{n=1}^{\infty} \frac{1}{n^2} = \frac{\pi^2}{6}
```

```cpp
// Keep \(not math\) inside code block untouched
int x = 1;
```
)";
    auto doc = ParseMarkdownDocument(md);
    int inline_math = 0;
    int display_math = 0;
    int code_blocks = 0;
    for (const auto& b : doc.blocks) {
        if (b.kind == Block::Kind::Paragraph) {
            for (const auto& r : b.inline_content.runs) {
                if (r.kind == InlineRun::Kind::InlineMath) ++inline_math;
            }
        } else if (b.kind == Block::Kind::DisplayMath) {
            ++display_math;
        } else if (b.kind == Block::Kind::CodeBlock) {
            ++code_blocks;
            assert(b.code_text.find(L"\\(not math\\)") != std::wstring::npos);
        }
    }
    assert(inline_math == 2);
    assert(display_math == 3);
    assert(code_blocks == 1);

    // Adjacent inline math delimiters ($ touching CJK/ASCII without surrounding spaces)
    const std::string md_adj = R"(
第$k$个样本与$N=150$个点，压缩比144D$\to$60D且a$x$b紧邻，另外`$keep_code$`、\$escaped\$和 $5 and $10 不应变成公式。
)";
    auto doc_adj = ParseMarkdownDocument(md_adj);
    int adj_math = 0;
    bool saw_literal_money = false;
    for (const auto& b : doc_adj.blocks) {
        if (b.kind == Block::Kind::Paragraph) {
            for (const auto& r : b.inline_content.runs) {
                if (r.kind == InlineRun::Kind::InlineMath) ++adj_math;
                if (r.kind == InlineRun::Kind::Text && r.text.find(L"$5 and $10") != std::wstring::npos) {
                    saw_literal_money = true;
                }
            }
        }
    }
    assert(adj_math == 4);
    assert(saw_literal_money);
}

static void TestImagesTablesAndAdmonitions() {
    const std::string md = R"(
![diagram](images/arch%20v1.svg "Architecture")

[![badge](badge.png)](https://example.com)

> [!TIP]
> Useful tip with `inline_code`.

| Name | Score |
| :--- | ---: |
| Alice | 100 |
)";
    auto doc = ParseMarkdownDocument(md, L"D:\\notes\\readme.md");
    bool saw_block_img = false;
    bool saw_linked_img = false;
    bool saw_admonition = false;
    bool saw_table = false;

    for (const auto& b : doc.blocks) {
        if (b.kind == Block::Kind::Image) {
            if (b.image_src == L"images/arch%20v1.svg") {
                saw_block_img = true;
                auto resolved = ResolveDocumentResourcePath(doc.base_dir, b.image_src);
                assert(resolved == L"D:\\notes\\images\\arch v1.svg");
            }
            if (b.image_src == L"badge.png" && b.image_link_href == L"https://example.com") {
                saw_linked_img = true;
            }
        } else if (b.kind == Block::Kind::Admonition) {
            saw_admonition = (b.admonition_kind == AdmonitionKind::Tip);
        } else if (b.kind == Block::Kind::Table) {
            saw_table = (b.table_cols == 2 && b.table_rows.size() == 2);
        }
    }
    assert(saw_block_img);
    assert(saw_linked_img);
    assert(saw_admonition);
    assert(saw_table);
}

static void TestSyntaxHighlighter() {
    auto tokens = HighlightCode(L"int main() { return 42; // hi\n}", "cpp");
    bool has_kw = false, has_num = false, has_cmt = false;
    for (const auto& t : tokens) {
        if (t.kind == TokenKind::Keyword) has_kw = true;
        if (t.kind == TokenKind::Number) has_num = true;
        if (t.kind == TokenKind::Comment) has_cmt = true;
    }
    assert(has_kw && has_num && has_cmt);
}

static void TestWelcomeShowcaseDoc() {
    auto doc = ParseMarkdownDocument(GetWelcomeMarkdownUtf8());
    assert(!doc.outline.empty());
    assert(doc.stats.math_count >= 5);
    assert(doc.stats.code_block_count >= 2);
    assert(doc.stats.word_count > 50);

    // Ensure tight list items (including task lists) retain non-empty paragraph children.
    int list_blocks = 0;
    int non_empty_items = 0;
    int task_items = 0;
    for (const auto& b : doc.blocks) {
        if (b.kind == Block::Kind::List) {
            ++list_blocks;
            for (const auto& li : b.list_items) {
                assert(!li.children.empty());
                ++non_empty_items;
                if (li.is_task) ++task_items;
            }
        }
    }
    assert(list_blocks >= 2);
    assert(non_empty_items >= 7);
    assert(task_items >= 5);
}

static void TestSourceLinesAndSaveRoundtrip() {
    // Line 0: blank
    // Line 1: # First Heading
    // Line 2: $$
    // Line 3: a = 1
    // Line 4: (blank inside $$ collapsed by preprocessor)
    // Line 5: b = 2
    // Line 6: $$
    // Line 7: ## Second Heading
    // Line 8: | C1 | C2 |
    // Line 9: | --- | --- |
    // Line 10: | v1 | v2 |
    const std::string md =
        "\n"
        "# First Heading\n"
        "$$\n"
        "a = 1\n"
        "\n"
        "b = 2\n"
        "$$\n"
        "## Second Heading\n"
        "| C1 | C2 |\n"
        "| --- | --- |\n"
        "| v1 | v2 |\n";
    auto doc = ParseMarkdownDocument(md, L"anchor_test.md");
    assert(doc.outline.size() == 2);
    assert(doc.outline[0].source_line == 1);
    assert(doc.outline[1].source_line == 7);

    bool saw_table = false;
    for (const auto& b : doc.blocks) {
        if (b.kind == Block::Kind::Table) {
            saw_table = true;
            assert(b.table_rows.size() == 2);
            assert(b.table_rows[0].source_line == 8);
            assert(b.table_rows[1].source_line == 10);
        }
    }
    assert(saw_table);

    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "markreader_save_test.md";
    std::wstring err;
    const std::string payload = "# 测试保存\r\n包含中文与 $E=mc^2$ 公式。\r\n";
    assert(SaveTextFileUtf8(tmp.wstring(), payload, err));
    std::string loaded;
    assert(LoadTextFileUtf8(tmp.wstring(), loaded, err));
    assert(loaded == payload);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

static void TestMarkdownSourceHighlighter() {
    MarkdownFenceState st;
    auto h = HighlightMarkdownLine(L"# 标题 [链接](https://example.com) $x^2$", st);
    bool has_preproc = false, has_bold = false, has_string = false;
    for (const auto& sp : h) {
        if (sp.kind == TokenKind::Preprocessor) has_preproc = true;
        if (sp.bold) has_bold = true;
        if (sp.kind == TokenKind::String) has_string = true;
    }
    assert(has_preproc && has_bold && has_string);

    auto f0 = HighlightMarkdownLine(L"```cpp", st);
    (void)f0;
    assert(st.in_code_fence && st.fence_lang == "cpp");
    auto c1 = HighlightMarkdownLine(L"int val = 42;", st);
    bool saw_kw = false, saw_num = false;
    for (const auto& sp : c1) {
        if (sp.kind == TokenKind::Keyword) saw_kw = true;
        if (sp.kind == TokenKind::Number) saw_num = true;
    }
    assert(saw_kw && saw_num);
    HighlightMarkdownLine(L"```", st);
    assert(!st.in_code_fence);
}

static void TestSourceEditorUndoRedoAndFind() {
    SourceEditor ed;
    // 1. Coalesced typing + space boundary Undo/Redo & IsModified tracking (LF preserved)
    ed.SetDocument("alpha\nbeta\ngamma");
    assert(!ed.IsModified());
    assert(ed.OnChar(L'X'));
    assert(ed.OnChar(L'Y'));
    assert(ed.OnChar(L' '));
    assert(ed.IsModified());
    assert(ed.GetUtf8() == "XY alpha\nbeta\ngamma");

    assert(ed.Undo());
    assert(ed.GetUtf8() == "XYalpha\nbeta\ngamma");
    assert(ed.Undo());
    assert(ed.GetUtf8() == "alpha\nbeta\ngamma");
    assert(!ed.IsModified());

    // Regression test: Redo must restore the new text rather than deleting the line
    assert(ed.Redo());
    assert(ed.GetUtf8() == "XYalpha\nbeta\ngamma");
    assert(ed.Redo());
    assert(ed.GetUtf8() == "XY alpha\nbeta\ngamma");
    assert(ed.IsModified());

    // 2. Multi-line selection + replace + Undo/Redo + Find + CRLF preservation
    SourceEditor crlf_ed;
    crlf_ed.SetDocument("hello world\r\nsecond line\r\n");
    crlf_ed.SelectAll();
    crlf_ed.PasteText(L"ONE\nTWO\nTHREE");
    assert(crlf_ed.GetUtf8() == "ONE\r\nTWO\r\nTHREE");
    assert(crlf_ed.Undo());
    assert(crlf_ed.GetUtf8() == "hello world\r\nsecond line\r\n");
    assert(!crlf_ed.IsModified());
    assert(crlf_ed.Redo());
    assert(crlf_ed.GetUtf8() == "ONE\r\nTWO\r\nTHREE");

    auto matches = crlf_ed.FindAllMatches(L"two");
    assert(matches.size() == 1);
    assert(matches[0].line == 1 && matches[0].start == 0 && matches[0].length == 3);
}

int main() {
    TestMarkdownOutlineAndSlugs();
    TestInlineAndDisplayMath();
    TestImagesTablesAndAdmonitions();
    TestSyntaxHighlighter();
    TestWelcomeShowcaseDoc();
    TestSourceLinesAndSaveRoundtrip();
    TestMarkdownSourceHighlighter();
    TestSourceEditorUndoRedoAndFind();
    std::cout << "All MarkReader unit tests passed.\n";
    return 0;
}
