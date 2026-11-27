#include "core/markdown_doc.h"
#include "core/sample_doc.h"
#include "core/syntax_hl.h"
#include "core/types.h"

#include <cassert>
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

int main() {
    TestMarkdownOutlineAndSlugs();
    TestInlineAndDisplayMath();
    TestImagesTablesAndAdmonitions();
    TestSyntaxHighlighter();
    TestWelcomeShowcaseDoc();
    std::cout << "All MarkReader unit tests passed.\n";
    return 0;
}
