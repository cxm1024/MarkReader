#include "core/syntax_hl.h"
#include "core/types.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace mr {

namespace {

enum class LangFamily {
    Plain,
    CLike,
    Python,
    JsTs,
    Rust,
    Go,
    Shell,
    Sql,
    Json,
    YamlToml,
    Markup,
    Css,
    TeX
};

LangFamily DetectLanguage(const std::string& raw_lang) {
    std::string l = ToLowerAscii(TrimAscii(raw_lang));
    if (l.empty()) return LangFamily::Plain;
    if (l == "c" || l == "cpp" || l == "c++" || l == "cc" || l == "cxx" || l == "h" || l == "hpp" ||
        l == "cs" || l == "csharp" || l == "c#" || l == "java" || l == "kt" || l == "kotlin" ||
        l == "swift" || l == "dart" || l == "zig" || l == "objective-c" || l == "objc") {
        return LangFamily::CLike;
    }
    if (l == "py" || l == "python" || l == "python3") return LangFamily::Python;
    if (l == "js" || l == "javascript" || l == "jsx" || l == "ts" || l == "typescript" || l == "tsx" ||
        l == "mjs" || l == "cjs") {
        return LangFamily::JsTs;
    }
    if (l == "rs" || l == "rust") return LangFamily::Rust;
    if (l == "go" || l == "golang") return LangFamily::Go;
    if (l == "sh" || l == "bash" || l == "zsh" || l == "shell" || l == "console" ||
        l == "ps1" || l == "powershell" || l == "pwsh" || l == "bat" || l == "cmd") {
        return LangFamily::Shell;
    }
    if (l == "sql" || l == "mysql" || l == "postgres" || l == "postgresql" || l == "sqlite") {
        return LangFamily::Sql;
    }
    if (l == "json" || l == "jsonc" || l == "json5") return LangFamily::Json;
    if (l == "yaml" || l == "yml" || l == "toml" || l == "ini" || l == "conf" || l == "cmake") {
        return LangFamily::YamlToml;
    }
    if (l == "html" || l == "htm" || l == "xml" || l == "svg" || l == "xhtml") {
        return LangFamily::Markup;
    }
    if (l == "css" || l == "scss" || l == "less") return LangFamily::Css;
    if (l == "tex" || l == "latex") return LangFamily::TeX;
    return LangFamily::Plain;
}

const std::unordered_set<std::wstring>& KeywordsFor(LangFamily fam) {
    static const std::unordered_set<std::wstring> kEmpty;
    static const std::unordered_set<std::wstring> kCLike = {
        L"alignas", L"alignof", L"and", L"asm", L"auto", L"break", L"case", L"catch", L"class",
        L"concept", L"const", L"consteval", L"constexpr", L"constinit", L"continue", L"co_await",
        L"co_return", L"co_yield", L"default", L"delete", L"do", L"else", L"enum", L"explicit",
        L"export", L"extern", L"false", L"final", L"finally", L"for", L"friend", L"goto", L"if",
        L"implements", L"import", L"inline", L"interface", L"module", L"mutable", L"namespace",
        L"new", L"noexcept", L"nullptr", L"null", L"operator", L"or", L"override", L"package",
        L"private", L"protected", L"public", L"requires", L"return", L"sizeof", L"static",
        L"static_assert", L"static_cast", L"dynamic_cast", L"reinterpret_cast", L"const_cast",
        L"struct", L"super", L"switch", L"template", L"this", L"throw", L"throws", L"true", L"try",
        L"typedef", L"typename", L"union", L"using", L"virtual", L"volatile", L"while",
        L"async", L"await", L"var", L"val", L"fun", L"sealed", L"record", L"out", L"ref"
    };
    static const std::unordered_set<std::wstring> kPython = {
        L"False", L"None", L"True", L"and", L"as", L"assert", L"async", L"await", L"break",
        L"class", L"continue", L"def", L"del", L"elif", L"else", L"except", L"finally", L"for",
        L"from", L"global", L"if", L"import", L"in", L"is", L"lambda", L"match", L"case",
        L"nonlocal", L"not", L"or", L"pass", L"raise", L"return", L"try", L"while", L"with", L"yield",
        L"self", L"cls"
    };
    static const std::unordered_set<std::wstring> kJsTs = {
        L"abstract", L"as", L"async", L"await", L"break", L"case", L"catch", L"class", L"const",
        L"continue", L"debugger", L"declare", L"default", L"delete", L"do", L"else", L"enum",
        L"export", L"extends", L"false", L"finally", L"for", L"from", L"function", L"get", L"if",
        L"implements", L"import", L"in", L"instanceof", L"interface", L"let", L"new", L"null",
        L"of", L"package", L"private", L"protected", L"public", L"readonly", L"return", L"set",
        L"static", L"super", L"switch", L"this", L"throw", L"true", L"try", L"type", L"typeof",
        L"undefined", L"var", L"void", L"while", L"with", L"yield", L"namespace", L"keyof", L"infer"
    };
    static const std::unordered_set<std::wstring> kRust = {
        L"as", L"async", L"await", L"break", L"const", L"continue", L"crate", L"dyn", L"else",
        L"enum", L"extern", L"false", L"fn", L"for", L"if", L"impl", L"in", L"let", L"loop",
        L"match", L"mod", L"move", L"mut", L"pub", L"ref", L"return", L"self", L"Self", L"static",
        L"struct", L"super", L"trait", L"true", L"type", L"unsafe", L"use", L"where", L"while"
    };
    static const std::unordered_set<std::wstring> kGo = {
        L"break", L"case", L"chan", L"const", L"continue", L"default", L"defer", L"else",
        L"fallthrough", L"false", L"for", L"func", L"go", L"goto", L"if", L"import", L"interface",
        L"map", L"nil", L"package", L"range", L"return", L"select", L"struct", L"switch", L"true",
        L"type", L"var"
    };
    static const std::unordered_set<std::wstring> kShell = {
        L"if", L"then", L"else", L"elif", L"fi", L"case", L"esac", L"for", L"while", L"until",
        L"do", L"done", L"in", L"function", L"select", L"return", L"exit", L"local", L"export",
        L"readonly", L"declare", L"unset", L"true", L"false", L"param", L"foreach", L"switch",
        L"try", L"catch", L"finally", L"throw", L"echo", L"cd", L"set"
    };
    static const std::unordered_set<std::wstring> kSql = {
        L"SELECT", L"FROM", L"WHERE", L"INSERT", L"INTO", L"VALUES", L"UPDATE", L"SET", L"DELETE",
        L"CREATE", L"TABLE", L"DROP", L"ALTER", L"ADD", L"INDEX", L"JOIN", L"INNER", L"LEFT",
        L"RIGHT", L"OUTER", L"ON", L"AS", L"AND", L"OR", L"NOT", L"NULL", L"IS", L"IN", L"EXISTS",
        L"GROUP", L"BY", L"ORDER", L"ASC", L"DESC", L"HAVING", L"LIMIT", L"OFFSET", L"UNION",
        L"ALL", L"DISTINCT", L"PRIMARY", L"KEY", L"FOREIGN", L"REFERENCES", L"DEFAULT", L"CASE",
        L"WHEN", L"THEN", L"ELSE", L"END", L"BEGIN", L"COMMIT", L"ROLLBACK", L"WITH",
        L"select", L"from", L"where", L"insert", L"into", L"values", L"update", L"set", L"delete",
        L"create", L"table", L"drop", L"alter", L"add", L"index", L"join", L"inner", L"left",
        L"right", L"outer", L"on", L"as", L"and", L"or", L"not", L"null", L"is", L"in", L"exists",
        L"group", L"by", L"order", L"asc", L"desc", L"having", L"limit", L"offset", L"union",
        L"all", L"distinct", L"primary", L"key", L"foreign", L"references", L"default", L"case",
        L"when", L"then", L"else", L"end", L"begin", L"commit", L"rollback", L"with"
    };
    static const std::unordered_set<std::wstring> kJsonYaml = {
        L"true", L"false", L"null", L"True", L"False", L"None", L"yes", L"no", L"on", L"off"
    };

    switch (fam) {
        case LangFamily::CLike: return kCLike;
        case LangFamily::Python: return kPython;
        case LangFamily::JsTs: return kJsTs;
        case LangFamily::Rust: return kRust;
        case LangFamily::Go: return kGo;
        case LangFamily::Shell: return kShell;
        case LangFamily::Sql: return kSql;
        case LangFamily::Json:
        case LangFamily::YamlToml: return kJsonYaml;
        default: return kEmpty;
    }
}

const std::unordered_set<std::wstring>& TypesFor(LangFamily fam) {
    static const std::unordered_set<std::wstring> kEmpty;
    static const std::unordered_set<std::wstring> kTypes = {
        L"void", L"bool", L"boolean", L"char", L"wchar_t", L"char8_t", L"char16_t", L"char32_t",
        L"short", L"int", L"long", L"float", L"double", L"signed", L"unsigned", L"size_t",
        L"int8_t", L"int16_t", L"int32_t", L"int64_t", L"uint8_t", L"uint16_t", L"uint32_t", L"uint64_t",
        L"i8", L"i16", L"i32", L"i64", L"isize", L"u8", L"u16", L"u32", L"u64", L"usize",
        L"f32", L"f64", L"str", L"string", L"String", L"Vec", L"Option", L"Result", L"Box",
        L"std", L"vector", L"wstring", L"shared_ptr", L"unique_ptr", L"map", L"unordered_map",
        L"number", L"any", L"unknown", L"never", L"object", L"Promise", L"Array",
        L"list", L"dict", L"tuple", L"set", L"bytes", L"byte", L"rune", L"error",
        L"INT", L"INTEGER", L"TEXT", L"VARCHAR", L"BOOLEAN", L"TIMESTAMP", L"FLOAT"
    };
    if (fam == LangFamily::CLike || fam == LangFamily::Rust || fam == LangFamily::Go ||
        fam == LangFamily::JsTs || fam == LangFamily::Python || fam == LangFamily::Sql) {
        return kTypes;
    }
    return kEmpty;
}

inline bool IsIdentStart(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'$';
}

inline bool IsIdentCont(wchar_t c) {
    return IsIdentStart(c) || (c >= L'0' && c <= L'9');
}

void PushToken(std::vector<SyntaxToken>& out, uint32_t start, uint32_t len, TokenKind kind) {
    if (len == 0 || kind == TokenKind::Normal) return;
    out.push_back({start, len, kind});
}

}  // namespace

std::vector<SyntaxToken> HighlightCode(const std::wstring& code, const std::string& lang) {
    std::vector<SyntaxToken> tokens;
    LangFamily fam = DetectLanguage(lang);
    if (fam == LangFamily::Plain || code.empty()) return tokens;

    const size_t n = code.size();
    const auto& keywords = KeywordsFor(fam);
    const auto& types = TypesFor(fam);

    if (fam == LangFamily::Markup) {
        size_t i = 0;
        while (i < n) {
            if (i + 3 < n && code[i] == L'<' && code[i + 1] == L'!' && code[i + 2] == L'-' && code[i + 3] == L'-') {
                size_t end = code.find(L"-->", i + 4);
                size_t len = (end == std::wstring::npos) ? (n - i) : (end + 3 - i);
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(len), TokenKind::Comment);
                i += len;
            } else if (code[i] == L'<') {
                size_t j = i + 1;
                if (j < n && (code[j] == L'/' || code[j] == L'!' || code[j] == L'?')) ++j;
                size_t tag_start = j;
                while (j < n && (IsIdentCont(code[j]) || code[j] == L'-' || code[j] == L':')) ++j;
                if (j > tag_start) {
                    PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Keyword);
                }
                while (j < n && code[j] != L'>') {
                    if (code[j] == L'"' || code[j] == L'\'') {
                        wchar_t q = code[j];
                        size_t s = j++;
                        while (j < n && code[j] != q) ++j;
                        if (j < n) ++j;
                        PushToken(tokens, static_cast<uint32_t>(s), static_cast<uint32_t>(j - s), TokenKind::String);
                    } else if (IsIdentStart(code[j])) {
                        size_t a = j;
                        while (j < n && (IsIdentCont(code[j]) || code[j] == L'-' || code[j] == L':')) ++j;
                        PushToken(tokens, static_cast<uint32_t>(a), static_cast<uint32_t>(j - a), TokenKind::Type);
                    } else {
                        ++j;
                    }
                }
                if (j < n && code[j] == L'>') {
                    PushToken(tokens, static_cast<uint32_t>(j), 1, TokenKind::Keyword);
                    ++j;
                }
                i = j;
            } else {
                ++i;
            }
        }
        return tokens;
    }

    if (fam == LangFamily::TeX) {
        size_t i = 0;
        while (i < n) {
            if (code[i] == L'%') {
                size_t j = i;
                while (j < n && code[j] != L'\n') ++j;
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Comment);
                i = j;
            } else if (code[i] == L'\\') {
                size_t j = i + 1;
                if (j < n && std::iswalpha(code[j])) {
                    while (j < n && std::iswalpha(code[j])) ++j;
                } else if (j < n) {
                    ++j;
                }
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Keyword);
                i = j;
            } else if (code[i] == L'{' || code[i] == L'}' || code[i] == L'$' || code[i] == L'&') {
                PushToken(tokens, static_cast<uint32_t>(i), 1, TokenKind::Operator);
                ++i;
            } else {
                ++i;
            }
        }
        return tokens;
    }

    size_t i = 0;
    while (i < n) {
        wchar_t c = code[i];

        // Comments
        if ((fam == LangFamily::CLike || fam == LangFamily::JsTs || fam == LangFamily::Rust ||
             fam == LangFamily::Go || fam == LangFamily::Json || fam == LangFamily::Css) &&
            c == L'/' && i + 1 < n) {
            if (code[i + 1] == L'/' && fam != LangFamily::Css) {
                size_t j = i + 2;
                while (j < n && code[j] != L'\n') ++j;
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Comment);
                i = j;
                continue;
            }
            if (code[i + 1] == L'*') {
                size_t end = code.find(L"*/", i + 2);
                size_t len = (end == std::wstring::npos) ? (n - i) : (end + 2 - i);
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(len), TokenKind::Comment);
                i += len;
                continue;
            }
        }

        if ((fam == LangFamily::Python || fam == LangFamily::Shell || fam == LangFamily::YamlToml) && c == L'#') {
            size_t j = i + 1;
            while (j < n && code[j] != L'\n') ++j;
            PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Comment);
            i = j;
            continue;
        }

        if (fam == LangFamily::Sql && c == L'-' && i + 1 < n && code[i + 1] == L'-') {
            size_t j = i + 2;
            while (j < n && code[j] != L'\n') ++j;
            PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Comment);
            i = j;
            continue;
        }

        // Preprocessor / decorators / Rust attributes
        if (fam == LangFamily::CLike && c == L'#' && (i == 0 || code[i - 1] == L'\n' || code[i - 1] == L' ' || code[i - 1] == L'\t')) {
            size_t j = i + 1;
            while (j < n && code[j] != L'\n') ++j;
            PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Preprocessor);
            i = j;
            continue;
        }
        if ((fam == LangFamily::Python || fam == LangFamily::JsTs || fam == LangFamily::CLike) && c == L'@') {
            size_t j = i + 1;
            while (j < n && (IsIdentCont(code[j]) || code[j] == L'.')) ++j;
            if (j > i + 1) {
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Preprocessor);
                i = j;
                continue;
            }
        }

        // Strings (triple-quoted Python or single/double/backtick)
        if (fam == LangFamily::Python && (c == L'"' || c == L'\'') && i + 2 < n && code[i + 1] == c && code[i + 2] == c) {
            size_t j = i + 3;
            while (j + 2 < n) {
                if (code[j] == L'\\') { j += 2; continue; }
                if (code[j] == c && code[j + 1] == c && code[j + 2] == c) { j += 3; break; }
                ++j;
            }
            if (j + 2 >= n && !(j <= n && j >= i + 6 && code[j - 1] == c)) j = n;
            PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::String);
            i = j;
            continue;
        }

        if (c == L'"' || (c == L'\'' && fam != LangFamily::Rust) || (c == L'`' && (fam == LangFamily::JsTs || fam == LangFamily::Go || fam == LangFamily::Sql))) {
            wchar_t q = c;
            size_t j = i + 1;
            while (j < n) {
                if (code[j] == L'\\' && fam != LangFamily::Sql) {
                    j += (j + 1 < n) ? 2 : 1;
                    continue;
                }
                if (code[j] == q) {
                    ++j;
                    break;
                }
                if (code[j] == L'\n' && q != L'`') break;
                ++j;
            }
            // In JSON, keys before ':' get Type color for crisp readability.
            TokenKind sk = TokenKind::String;
            if (fam == LangFamily::Json) {
                size_t k = j;
                while (k < n && (code[k] == L' ' || code[k] == L'\t')) ++k;
                if (k < n && code[k] == L':') sk = TokenKind::Type;
            }
            PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), sk);
            i = j;
            continue;
        }

        // Rust char literal vs lifetime
        if (fam == LangFamily::Rust && c == L'\'') {
            size_t j = i + 1;
            if (j < n && code[j] == L'\\') {
                j += 2;
                while (j < n && code[j] != L'\'' && code[j] != L'\n') ++j;
                if (j < n && code[j] == L'\'') ++j;
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::String);
                i = j;
                continue;
            } else if (j + 1 < n && code[j + 1] == L'\'') {
                PushToken(tokens, static_cast<uint32_t>(i), 3, TokenKind::String);
                i += 3;
                continue;
            }
        }

        // Numbers
        if ((c >= L'0' && c <= L'9') && (i == 0 || !IsIdentCont(code[i - 1]))) {
            size_t j = i + 1;
            while (j < n && (std::iswalnum(code[j]) || code[j] == L'.' || code[j] == L'_' || code[j] == L'\'')) {
                ++j;
            }
            PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Number);
            i = j;
            continue;
        }

        // Identifiers, keywords, types, functions, YAML keys
        if (IsIdentStart(c)) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(code[j])) ++j;
            std::wstring word = code.substr(i, j - i);

            if (fam == LangFamily::YamlToml) {
                size_t k = j;
                while (k < n && (code[k] == L' ' || code[k] == L'\t')) ++k;
                if (k < n && (code[k] == L':' || code[k] == L'=')) {
                    PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Type);
                    i = j;
                    continue;
                }
            }

            if (keywords.count(word)) {
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Keyword);
            } else if (types.count(word)) {
                PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Type);
            } else {
                size_t k = j;
                while (k < n && (code[k] == L' ' || code[k] == L'\t')) ++k;
                if (k < n && (code[k] == L'(' || (fam == LangFamily::Rust && code[k] == L'!'))) {
                    PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Function);
                } else if (word.size() >= 2 && word[0] >= L'A' && word[0] <= L'Z' &&
                           std::any_of(word.begin() + 1, word.end(), [](wchar_t ch) { return ch >= L'a' && ch <= L'z'; })) {
                    PushToken(tokens, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i), TokenKind::Type);
                }
            }
            i = j;
            continue;
        }

        // Operators
        if (c == L'+' || c == L'-' || c == L'*' || c == L'/' || c == L'=' || c == L'!' ||
            c == L'<' || c == L'>' || c == L'&' || c == L'|' || c == L'^' || c == L'%' || c == L'~') {
            PushToken(tokens, static_cast<uint32_t>(i), 1, TokenKind::Operator);
        }
        ++i;
    }

    return tokens;
}

}  // namespace mr
