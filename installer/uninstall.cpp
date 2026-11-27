#include "win_helpers.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) return true;
    }
    return false;
}

void CleanupRegistryRoot(HKEY root, const std::wstring& install_dir) {
    const std::wstring uninst_key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MarkReader";
    std::wstring reg_dir = mr::inst::RegGetSz(root, uninst_key, L"InstallLocation");

    if (!reg_dir.empty() && _wcsicmp(reg_dir.c_str(), install_dir.c_str()) != 0) {
        return;  // Don't delete registration belonging to a different installation path
    }

    std::wstring desk_lnk = mr::inst::RegGetSz(root, uninst_key, L"DesktopShortcut");
    if (!desk_lnk.empty()) {
        DeleteFileW(desk_lnk.c_str());
    }

    std::wstring sm_dir = mr::inst::RegGetSz(root, uninst_key, L"StartMenuDir");
    if (!sm_dir.empty() && fs::exists(sm_dir)) {
        std::error_code ec;
        fs::remove_all(sm_dir, ec);
    }

    RegDeleteTreeW(root, uninst_key.c_str());

    const std::wstring classes_prefix = L"SOFTWARE\\Classes\\";
    RegDeleteTreeW(root, (classes_prefix + L"MarkReader.Document").c_str());
    RegDeleteTreeW(root, (classes_prefix + L"Applications\\MarkReader.exe").c_str());

    for (const wchar_t* ext : {L".md", L".markdown"}) {
        std::wstring ext_key = classes_prefix + ext;
        if (mr::inst::RegGetSz(root, ext_key, L"") == L"MarkReader.Document") {
            HKEY hk = nullptr;
            if (RegOpenKeyExW(root, ext_key.c_str(), 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
                RegDeleteValueW(hk, nullptr);
                RegCloseKey(hk);
            }
        }
        HKEY ow_hk = nullptr;
        if (RegOpenKeyExW(root, (ext_key + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &ow_hk) == ERROR_SUCCESS) {
            RegDeleteValueW(ow_hk, L"MarkReader.Document");
            RegCloseKey(ow_hk);
        }
    }
}

void ScheduleSelfDelete(const std::wstring& self_exe, const std::wstring& install_dir) {
    // Run a hidden cmd.exe process from TEMP so install_dir is not locked as current working directory.
    std::wstring temp_dir = mr::inst::GetExeDir();
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp) > 0) temp_dir = tmp;

    std::wstring cmd_args =
        L"/c ping -n 2 127.0.0.1 >nul & del /f /q \"" + self_exe + L"\" & rmdir \"" + install_dir + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = cmd_args.c_str();
    sei.lpDirectory = temp_dir.c_str();
    sei.nShow = SW_HIDE;
    ShellExecuteExW(&sei);
    if (sei.hProcess) CloseHandle(sei.hProcess);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool silent = argv && HasFlag(argc, argv, L"/S");
    const bool already_elevated_arg = argv && HasFlag(argc, argv, L"--elevated");
    if (argv) LocalFree(argv);

    const std::wstring self_exe = mr::inst::GetExePath();
    const std::wstring install_dir = mr::inst::GetExeDir();

    // Check if installed under HKLM or protected directory -> elevate if needed.
    bool in_hklm = !mr::inst::RegGetSz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MarkReader", L"InstallLocation").empty();
    bool needs_admin = in_hklm || !mr::inst::CanWriteDirectory(install_dir);

    if (needs_admin && !mr::inst::IsProcessElevated() && !already_elevated_arg) {
        std::wstring params = silent ? L"/S --elevated" : L"--elevated";
        HINSTANCE res = ShellExecuteW(nullptr, L"runas", self_exe.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(res) > 32) {
            return 0;
        }
        if (!silent) {
            MessageBoxW(nullptr, L"需要管理员权限才能卸载此位置的 MarkReader。", L"卸载 MarkReader", MB_ICONWARNING);
        }
        return 1;
    }

    if (!silent) {
        std::wstring prompt = L"确定要从以下位置卸载 MarkReader 吗？\n\n" + install_dir;
        if (MessageBoxW(nullptr, prompt.c_str(), L"卸载 MarkReader", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return 0;
        }
    }

    // Ensure MarkReader.exe is not locking files
    const fs::path app_exe = fs::path(install_dir) / L"MarkReader.exe";
    while (fs::exists(app_exe)) {
        std::error_code ec;
        if (fs::remove(app_exe, ec)) break;
        if (silent) break;
        int choice = MessageBoxW(
            nullptr,
            L"MarkReader 正在运行，请先关闭 MarkReader 后再重试。",
            L"卸载 MarkReader",
            MB_RETRYCANCEL | MB_ICONWARNING);
        if (choice != IDRETRY) return 1;
    }

    // Remove res directory
    const fs::path res_dir = fs::path(install_dir) / L"res";
    if (fs::exists(res_dir)) {
        std::error_code ec;
        fs::remove_all(res_dir, ec);
    }

    // Clean shortcuts (registry tracked + standard fallback locations)
    CleanupRegistryRoot(HKEY_CURRENT_USER, install_dir);
    if (mr::inst::IsProcessElevated()) {
        CleanupRegistryRoot(HKEY_LOCAL_MACHINE, install_dir);
    }

    for (const auto& folder : {
             mr::inst::GetKnownDir(FOLDERID_Desktop),
             mr::inst::GetKnownDir(FOLDERID_PublicDesktop)}) {
        if (!folder.empty()) {
            fs::path lnk = fs::path(folder) / L"MarkReader.lnk";
            std::error_code ec;
            fs::remove(lnk, ec);
        }
    }
    for (const auto& folder : {
             mr::inst::GetKnownDir(FOLDERID_Programs),
             mr::inst::GetKnownDir(FOLDERID_CommonPrograms)}) {
        if (!folder.empty()) {
            fs::path sm = fs::path(folder) / L"MarkReader";
            if (fs::exists(sm)) {
                std::error_code ec;
                fs::remove_all(sm, ec);
            }
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    ScheduleSelfDelete(self_exe, install_dir);

    if (!silent) {
        MessageBoxW(nullptr, L"MarkReader 已成功卸载。", L"卸载 MarkReader", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}
