#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

namespace mr::inst {

inline std::wstring Utf8ToWide(const char* data, size_t len) {
    if (!data || len == 0) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), out.data(), needed);
    return out;
}

inline bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD sz = sizeof(elev);
    bool elevated = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz) && elev.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

inline bool EnsureDirectoryTree(const std::wstring& path) {
    if (path.empty()) return false;
    int res = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return res == ERROR_SUCCESS || res == ERROR_ALREADY_EXISTS || res == ERROR_FILE_EXISTS;
}

inline bool CanWriteDirectory(const std::wstring& target_dir) {
    if (target_dir.empty()) return false;
    std::wstring probe_dir = target_dir;
    while (!probe_dir.empty()) {
        DWORD attr = GetFileAttributesW(probe_dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            break;
        }
        size_t sep = probe_dir.find_last_of(L"\\/");
        if (sep == std::wstring::npos || (sep == 2 && probe_dir[1] == L':')) {
            if (sep == 2 && probe_dir[1] == L':') probe_dir = probe_dir.substr(0, 3);
            break;
        }
        probe_dir = probe_dir.substr(0, sep);
    }
    if (probe_dir.empty()) return false;
    if (probe_dir.back() != L'\\') probe_dir.push_back(L'\\');
    std::wstring probe_file = probe_dir + L".mr_write_probe_" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(
        probe_file.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

inline std::wstring GetKnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring res;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        res = p;
        CoTaskMemFree(p);
    }
    return res;
}

inline bool CreateShortcut(
    const std::wstring& lnk_path,
    const std::wstring& target_exe,
    const std::wstring& args,
    const std::wstring& work_dir,
    const std::wstring& description,
    const std::wstring& icon_path) {
    size_t sep = lnk_path.find_last_of(L"\\/");
    if (sep != std::wstring::npos) {
        EnsureDirectoryTree(lnk_path.substr(0, sep));
    }

    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (FAILED(hr) || !psl) return false;

    psl->SetPath(target_exe.c_str());
    if (!args.empty()) psl->SetArguments(args.c_str());
    if (!work_dir.empty()) psl->SetWorkingDirectory(work_dir.c_str());
    if (!description.empty()) psl->SetDescription(description.c_str());
    if (!icon_path.empty()) psl->SetIconLocation(icon_path.c_str(), 0);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->Save(lnk_path.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

inline void RegSetSz(HKEY root, const std::wstring& subkey, const std::wstring& name, const std::wstring& value) {
    HKEY hk = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(
            hk, name.empty() ? nullptr : name.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }
}

inline void RegSetDword(HKEY root, const std::wstring& subkey, const std::wstring& name, DWORD value) {
    HKEY hk = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hk, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hk);
    }
}

inline std::wstring RegGetSz(HKEY root, const std::wstring& subkey, const std::wstring& name) {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, subkey.c_str(), name.empty() ? nullptr : name.c_str(),
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        return std::wstring(buf);
    }
    return {};
}

inline void RegisterAppAndUninstall(
    bool system_wide,
    const std::wstring& install_dir,
    const std::wstring& app_exe,
    const std::wstring& uninstall_exe,
    bool associate_md,
    const std::wstring& desktop_lnk,
    const std::wstring& start_menu_dir,
    DWORD estimated_kb) {
    HKEY root = system_wide ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const std::wstring uninst_key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MarkReader";

    RegSetSz(root, uninst_key, L"DisplayName", L"MarkReader");
    RegSetSz(root, uninst_key, L"DisplayVersion", L"1.0.0");
    RegSetSz(root, uninst_key, L"Publisher", L"MarkReader");
    RegSetSz(root, uninst_key, L"DisplayIcon", app_exe + L",0");
    RegSetSz(root, uninst_key, L"InstallLocation", install_dir);
    RegSetSz(root, uninst_key, L"UninstallString", L"\"" + uninstall_exe + L"\"");
    RegSetSz(root, uninst_key, L"QuietUninstallString", L"\"" + uninstall_exe + L"\" /S");
    RegSetDword(root, uninst_key, L"EstimatedSize", estimated_kb);
    RegSetDword(root, uninst_key, L"NoModify", 1);
    RegSetDword(root, uninst_key, L"NoRepair", 1);
    RegSetSz(root, uninst_key, L"DesktopShortcut", desktop_lnk);
    RegSetSz(root, uninst_key, L"StartMenuDir", start_menu_dir);
    RegSetDword(root, uninst_key, L"FileAssoc", associate_md ? 1 : 0);

    const std::wstring classes_prefix = L"SOFTWARE\\Classes\\";
    RegSetSz(root, classes_prefix + L"MarkReader.Document", L"", L"Markdown 文档");
    RegSetSz(root, classes_prefix + L"MarkReader.Document\\DefaultIcon", L"", app_exe + L",0");
    RegSetSz(root, classes_prefix + L"MarkReader.Document\\shell\\open\\command", L"", L"\"" + app_exe + L"\" \"%1\"");

    RegSetSz(root, classes_prefix + L"Applications\\MarkReader.exe", L"FriendlyAppName", L"MarkReader");
    RegSetSz(root, classes_prefix + L"Applications\\MarkReader.exe\\DefaultIcon", L"", app_exe + L",0");
    RegSetSz(root, classes_prefix + L"Applications\\MarkReader.exe\\shell\\open\\command", L"", L"\"" + app_exe + L"\" \"%1\"");
    RegSetSz(root, classes_prefix + L"Applications\\MarkReader.exe\\SupportedTypes", L".md", L"");
    RegSetSz(root, classes_prefix + L"Applications\\MarkReader.exe\\SupportedTypes", L".markdown", L"");

    RegSetSz(root, classes_prefix + L".md\\OpenWithProgids", L"MarkReader.Document", L"");
    RegSetSz(root, classes_prefix + L".markdown\\OpenWithProgids", L"MarkReader.Document", L"");

    if (associate_md) {
        RegSetSz(root, classes_prefix + L".md", L"", L"MarkReader.Document");
        RegSetSz(root, classes_prefix + L".markdown", L"", L"MarkReader.Document");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

inline std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring s(buf, len);
    size_t sep = s.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? L"." : s.substr(0, sep);
}

inline std::wstring GetExePath() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, len);
}

}  // namespace mr::inst
