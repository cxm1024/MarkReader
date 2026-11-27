#include "core/types.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

extern "C" {
#include "entity.h"
}

namespace mr {

std::wstring Utf8ToWide(const std::string& utf8) {
    return Utf8ToWide(utf8.data(), utf8.size());
}

std::wstring Utf8ToWide(const char* data, size_t len) {
    if (!data || len == 0) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), out.data(), needed);
    return out;
}

std::string WideToUtf8(const std::wstring& wide) {
    return WideToUtf8(wide.data(), wide.size());
}

std::string WideToUtf8(const wchar_t* data, size_t len) {
    if (!data || len == 0) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(len), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string TrimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

static int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
    if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
    return -1;
}

std::wstring UrlDecodeWide(const std::wstring& s) {
    std::string utf8 = WideToUtf8(s);
    std::string decoded;
    decoded.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '%' && i + 2 < utf8.size()) {
            int hi = HexVal(static_cast<wchar_t>(static_cast<unsigned char>(utf8[i + 1])));
            int lo = HexVal(static_cast<wchar_t>(static_cast<unsigned char>(utf8[i + 2])));
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(utf8[i]);
    }
    return Utf8ToWide(decoded);
}

static void AppendCodepointUtf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    }
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string DecodeHtmlEntityUtf8(const char* entity, size_t len) {
    if (!entity || len < 3 || entity[0] != '&' || entity[len - 1] != ';') {
        return entity ? std::string(entity, len) : std::string{};
    }
    if (entity[1] == '#') {
        uint32_t cp = 0;
        if (len >= 4 && (entity[2] == 'x' || entity[2] == 'X')) {
            for (size_t i = 3; i < len - 1; ++i) {
                int v = HexVal(static_cast<wchar_t>(static_cast<unsigned char>(entity[i])));
                if (v < 0) return std::string(entity, len);
                cp = (cp << 4) | static_cast<uint32_t>(v);
                if (cp > 0x10FFFF) { cp = 0xFFFD; break; }
            }
        } else {
            for (size_t i = 2; i < len - 1; ++i) {
                char ch = entity[i];
                if (ch < '0' || ch > '9') return std::string(entity, len);
                cp = cp * 10 + static_cast<uint32_t>(ch - '0');
                if (cp > 0x10FFFF) { cp = 0xFFFD; break; }
            }
        }
        std::string out;
        AppendCodepointUtf8(out, cp);
        return out;
    }

    const ENTITY* ent = entity_lookup(entity, len);
    if (ent) {
        std::string out;
        if (ent->codepoints[0] != 0) AppendCodepointUtf8(out, ent->codepoints[0]);
        if (ent->codepoints[1] != 0) AppendCodepointUtf8(out, ent->codepoints[1]);
        return out;
    }
    return std::string(entity, len);
}

std::string SlugifyHeading(const std::string& text) {
    std::string out;
    bool prev_dash = false;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) {
                out.push_back(static_cast<char>(std::tolower(c)));
                prev_dash = false;
            } else if (c == ' ' || c == '-' || c == '_') {
                if (!out.empty() && !prev_dash && c != '_') {
                    out.push_back('-');
                    prev_dash = true;
                } else if (c == '_') {
                    out.push_back('_');
                    prev_dash = false;
                }
            }
            ++i;
        } else {
            // Preserve UTF-8 multi-byte characters (such as CJK) for heading anchor links.
            size_t char_len = 1;
            if ((c & 0xE0) == 0xC0) char_len = 2;
            else if ((c & 0xF0) == 0xE0) char_len = 3;
            else if ((c & 0xF8) == 0xF0) char_len = 4;
            if (i + char_len <= text.size()) {
                out.append(text, i, char_len);
                prev_dash = false;
            }
            i += char_len;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

bool IsRemoteUrl(const std::wstring& url) {
    auto starts_ci = [&](const wchar_t* prefix, size_t n) {
        if (url.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            wchar_t a = url[i];
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
            if (a != prefix[i]) return false;
        }
        return true;
    };
    return starts_ci(L"http://", 7) || starts_ci(L"https://", 8);
}

bool IsWindowsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 3 &&
        ((path[0] >= L'a' && path[0] <= L'z') || (path[0] >= L'A' && path[0] <= L'Z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    return false;
}

std::wstring GetExecutableDir() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring full(buf, len);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : full.substr(0, pos);
}

bool LoadTextFileUtf8(const std::wstring& path, std::string& out_utf8, std::wstring& out_error) {
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        out_error = L"无法打开文件: " + path;
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 0 || fileSize.QuadPart > 128LL * 1024 * 1024) {
        CloseHandle(hFile);
        out_error = L"文件大小无效或超过 128 MB 限制";
        return false;
    }

    std::string raw(static_cast<size_t>(fileSize.QuadPart), '\0');
    size_t total_read = 0;
    while (total_read < raw.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(raw.size() - total_read, 1u << 20));
        DWORD read_bytes = 0;
        if (!ReadFile(hFile, raw.data() + total_read, chunk, &read_bytes, nullptr)) {
            CloseHandle(hFile);
            out_error = L"读取文件失败";
            return false;
        }
        if (read_bytes == 0) break;
        total_read += read_bytes;
    }
    raw.resize(total_read);
    CloseHandle(hFile);

    // Handle UTF-8 BOM, UTF-16 LE BOM, UTF-16 BE BOM, or fallback ANSI/GBK -> UTF-8.
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        out_utf8 = raw.substr(3);
        return true;
    }

    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0xFF &&
        static_cast<unsigned char>(raw[1]) == 0xFE) {
        size_t wchar_cnt = (raw.size() - 2) / 2;
        std::wstring ws(wchar_cnt, L'\0');
        for (size_t i = 0; i < wchar_cnt; ++i) {
            uint16_t lo = static_cast<unsigned char>(raw[2 + i * 2]);
            uint16_t hi = static_cast<unsigned char>(raw[2 + i * 2 + 1]);
            ws[i] = static_cast<wchar_t>(lo | (hi << 8));
        }
        out_utf8 = WideToUtf8(ws);
        return true;
    }

    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0xFE &&
        static_cast<unsigned char>(raw[1]) == 0xFF) {
        size_t wchar_cnt = (raw.size() - 2) / 2;
        std::wstring ws(wchar_cnt, L'\0');
        for (size_t i = 0; i < wchar_cnt; ++i) {
            uint16_t hi = static_cast<unsigned char>(raw[2 + i * 2]);
            uint16_t lo = static_cast<unsigned char>(raw[2 + i * 2 + 1]);
            ws[i] = static_cast<wchar_t>(lo | (hi << 8));
        }
        out_utf8 = WideToUtf8(ws);
        return true;
    }

    // Validate UTF-8 strictly; if invalid, convert from system ACP (e.g. GBK/CP936).
    if (!raw.empty()) {
        int valid = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        if (valid == 0 && GetLastError() == ERROR_NO_UNICODE_TRANSLATION) {
            int acp_len = MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            if (acp_len > 0) {
                std::wstring ws(static_cast<size_t>(acp_len), L'\0');
                MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), ws.data(), acp_len);
                out_utf8 = WideToUtf8(ws);
                return true;
            }
        }
    }

    out_utf8 = std::move(raw);
    return true;
}

bool SaveTextFileUtf8(const std::wstring& path, const std::string& utf8, std::wstring& out_error) {
    out_error.clear();
    if (path.empty()) {
        out_error = L"文件路径为空";
        return false;
    }

    // Write via CreateFileW (CREATE_ALWAYS) directly on target path so NTFS/hardlink attributes preserve cleanly,
    // falling back to an error message on locked/read-only files without losing the user's open editor buffer.
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        out_error = L"无法写入文件 (错误码 " + std::to_wstring(err) + L")";
        return false;
    }

    size_t total_written = 0;
    while (total_written < utf8.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(utf8.size() - total_written, 4 * 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(hFile, utf8.data() + total_written, chunk, &written, nullptr) || written == 0) {
            DWORD err = GetLastError();
            CloseHandle(hFile);
            out_error = L"写入文件失败 (错误码 " + std::to_wstring(err) + L")";
            return false;
        }
        total_written += written;
    }

    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    return true;
}

static std::wstring GetConfigPath() {
    PWSTR app_data = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &app_data)) && app_data) {
        base = app_data;
        CoTaskMemFree(app_data);
    } else {
        base = GetExecutableDir();
    }
    std::wstring dir = base + L"\\MarkReader";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

AppConfig LoadAppConfig() {
    AppConfig cfg;
    std::wstring path = GetConfigPath();
    std::string content;
    std::wstring err;
    if (!LoadTextFileUtf8(path, content, err)) {
        // Default theme follows Windows system dark/light preference on first run.
        DWORD apps_use_light = 1;
        DWORD sz = sizeof(apps_use_light);
        if (RegGetValueW(HKEY_CURRENT_USER,
                         L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &apps_use_light, &sz) == ERROR_SUCCESS) {
            cfg.theme = (apps_use_light == 0) ? ThemeMode::Dark : ThemeMode::Light;
        }
        return cfg;
    }

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = TrimAscii(line.substr(0, eq));
        std::string val = TrimAscii(line.substr(eq + 1));
        if (key == "theme") {
            int t = std::atoi(val.c_str());
            if (t >= 0 && t <= 2) cfg.theme = static_cast<ThemeMode>(t);
        } else if (key == "zoom_percent") {
            int z = std::atoi(val.c_str());
            if (z >= 50 && z <= 250) cfg.zoom_percent = z;
        } else if (key == "outline_visible") {
            cfg.outline_visible = (val != "0" && val != "false");
        } else if (key == "outline_width") {
            float w = static_cast<float>(std::atof(val.c_str()));
            if (w >= 180.0f && w <= 460.0f) cfg.outline_width = w;
        } else if (key == "window_width") {
            int w = std::atoi(val.c_str());
            if (w >= 640 && w <= 4096) cfg.window_width = w;
        } else if (key == "window_height") {
            int h = std::atoi(val.c_str());
            if (h >= 480 && h <= 4096) cfg.window_height = h;
        } else if (key == "window_maximized") {
            cfg.window_maximized = (val == "1" || val == "true");
        } else if (key.rfind("recent_", 0) == 0 && !val.empty()) {
            std::wstring wp = Utf8ToWide(val);
            if (GetFileAttributesW(wp.c_str()) != INVALID_FILE_ATTRIBUTES) {
                cfg.recent_files.push_back(wp);
            }
        }
    }
    return cfg;
}

void SaveAppConfig(const AppConfig& cfg) {
    std::wstring path = GetConfigPath();
    std::ostringstream oss;
    oss << "theme=" << static_cast<int>(cfg.theme) << "\n"
        << "zoom_percent=" << cfg.zoom_percent << "\n"
        << "outline_visible=" << (cfg.outline_visible ? 1 : 0) << "\n"
        << "outline_width=" << static_cast<int>(cfg.outline_width) << "\n"
        << "window_width=" << cfg.window_width << "\n"
        << "window_height=" << cfg.window_height << "\n"
        << "window_maximized=" << (cfg.window_maximized ? 1 : 0) << "\n";
    for (size_t i = 0; i < cfg.recent_files.size() && i < 10; ++i) {
        oss << "recent_" << i << "=" << WideToUtf8(cfg.recent_files[i]) << "\n";
    }
    std::string data = oss.str();

    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(hFile);
    }
}

void AddRecentFile(AppConfig& cfg, const std::wstring& path) {
    if (path.empty()) return;
    cfg.recent_files.erase(
        std::remove_if(cfg.recent_files.begin(), cfg.recent_files.end(),
                       [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }),
        cfg.recent_files.end());
    cfg.recent_files.insert(cfg.recent_files.begin(), path);
    if (cfg.recent_files.size() > 10) {
        cfg.recent_files.resize(10);
    }
}

}  // namespace mr
