#include "payload_format.h"
#include "win_helpers.h"

#include <windows.h>
#include <commctrl.h>
#include <compressapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int IDC_EDIT_PATH = 101;
constexpr int IDC_BTN_BROWSE = 102;
constexpr int IDC_LBL_SPACE = 103;
constexpr int IDC_CHK_DESKTOP = 104;
constexpr int IDC_CHK_STARTMENU = 105;
constexpr int IDC_CHK_ASSOC = 106;
constexpr int IDC_PROGRESS = 107;
constexpr int IDC_LBL_STATUS = 108;
constexpr int IDC_CHK_LAUNCH = 109;
constexpr int IDC_BTN_INSTALL = 110;
constexpr int IDC_BTN_CANCEL = 111;

constexpr UINT WM_APP_PROGRESS = WM_APP + 201;
constexpr UINT WM_APP_DONE = WM_APP + 202;

struct InstallOptions {
    std::wstring install_dir;
    bool create_desktop = true;
    bool create_start_menu = true;
    bool associate_md = false;
    bool silent = false;
    bool auto_install = false;
    bool elevated_relaunch = false;
    bool explicit_dir = false;
    bool is_upgrade = false;
    std::wstring existing_version;
};

struct InstallResult {
    bool ok = false;
    std::wstring error_msg;
};

struct ExistingInstallInfo {
    bool found = false;
    bool system_wide = false;
    std::wstring install_dir;
    std::wstring version;
    bool create_desktop = true;
    bool create_start_menu = true;
    bool associate_md = false;
};

struct InstallerState {
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr;
    float dpi = 96.0f;

    HFONT font_title = nullptr;
    HFONT font_sub = nullptr;
    HFONT font_ui = nullptr;
    HFONT font_bold = nullptr;
    HICON icon_large = nullptr;
    HBRUSH white_brush = nullptr;

    HWND edit_path = nullptr;
    HWND btn_browse = nullptr;
    HWND lbl_space = nullptr;
    HWND chk_desktop = nullptr;
    HWND chk_startmenu = nullptr;
    HWND chk_assoc = nullptr;
    HWND progress = nullptr;
    HWND lbl_status = nullptr;
    HWND chk_launch = nullptr;
    HWND btn_install = nullptr;
    HWND btn_cancel = nullptr;

    InstallOptions opts;
    bool installing = false;
    bool finished = false;
    uint64_t payload_uncompressed_bytes = 5800000;
};

int Dip(const InstallerState* s, float v) {
    return static_cast<int>(v * (s->dpi / 96.0f) + 0.5f);
}

std::wstring DefaultInstallDir() {
    std::wstring pf = mr::inst::GetKnownDir(FOLDERID_ProgramFiles);
    if (pf.empty()) pf = L"C:\\Program Files";
    if (pf.back() != L'\\') pf.push_back(L'\\');
    return pf + L"MarkReader";
}

std::wstring TrimPath(std::wstring p) {
    while (!p.empty() && (p.front() == L' ' || p.front() == L'"')) p.erase(p.begin());
    while (!p.empty() && (p.back() == L' ' || p.back() == L'"')) p.pop_back();
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

ExistingInstallInfo DetectExistingInstall() {
    ExistingInstallInfo info;
    const std::wstring uninst_key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MarkReader";
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        std::wstring loc = TrimPath(mr::inst::RegGetSz(root, uninst_key, L"InstallLocation"));
        if (!loc.empty() && fs::exists(fs::path(loc) / L"MarkReader.exe")) {
            info.found = true;
            info.system_wide = (root == HKEY_LOCAL_MACHINE);
            info.install_dir = loc;
            info.version = mr::inst::RegGetSz(root, uninst_key, L"DisplayVersion");
            std::wstring desk_lnk = mr::inst::RegGetSz(root, uninst_key, L"DesktopShortcut");
            std::wstring sm_dir = mr::inst::RegGetSz(root, uninst_key, L"StartMenuDir");
            info.create_desktop = !desk_lnk.empty();
            info.create_start_menu = !sm_dir.empty();
            DWORD assoc = 0;
            if (mr::inst::RegGetDword(root, uninst_key, L"FileAssoc", assoc)) {
                info.associate_md = (assoc != 0);
            }
            return info;
        }
    }

    // Fallback: check default directory on disk in case registry metadata is absent
    std::wstring def = DefaultInstallDir();
    if (fs::exists(fs::path(def) / L"MarkReader.exe")) {
        info.found = true;
        info.system_wide = true;
        info.install_dir = def;
    }
    return info;
}

bool IsValidInstallDir(const std::wstring& p) {
    if (p.size() < 4) return false;
    if (!((p[0] >= L'a' && p[0] <= L'z') || (p[0] >= L'A' && p[0] <= L'Z')) || p[1] != L':' || p[2] != L'\\') {
        return false;
    }
    return p.find(L"..") == std::wstring::npos;
}

bool GetPayloadResource(const uint8_t*& out_ptr, size_t& out_size) {
    HRSRC hrsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(mr::pkg::kPayloadResourceId), RT_RCDATA);
    if (!hrsrc) return false;
    HGLOBAL hglob = LoadResource(nullptr, hrsrc);
    if (!hglob) return false;
    DWORD sz = SizeofResource(nullptr, hrsrc);
    const void* ptr = LockResource(hglob);
    if (!ptr || sz < sizeof(mr::pkg::CompressedHeader)) return false;
    out_ptr = static_cast<const uint8_t*>(ptr);
    out_size = static_cast<size_t>(sz);
    return true;
}

uint64_t QueryPayloadUncompressedSize() {
    const uint8_t* ptr = nullptr;
    size_t sz = 0;
    if (!GetPayloadResource(ptr, sz)) return 5800000;
    mr::pkg::CompressedHeader hdr{};
    memcpy(&hdr, ptr, sizeof(hdr));
    if (memcmp(hdr.magic, mr::pkg::kCompressedMagic, 4) != 0) return 5800000;
    return hdr.uncompressed_size;
}

InstallResult RunInstallation(
    const InstallOptions& opts,
    HWND progress_hwnd) {
    auto report = [&](int pct, const wchar_t* msg) {
        if (progress_hwnd && IsWindow(progress_hwnd)) {
            auto* txt = new std::wstring(msg);
            PostMessageW(progress_hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(pct), reinterpret_cast<LPARAM>(txt));
        }
    };

    const std::wstring target_dir = TrimPath(opts.install_dir);
    if (!IsValidInstallDir(target_dir)) {
        return {false, L"请输入有效的完整安装路径（例如 C:\\Program Files\\MarkReader）。"};
    }

    report(5, L"正在读取安装包资源...");
    const uint8_t* res_ptr = nullptr;
    size_t res_size = 0;
    if (!GetPayloadResource(res_ptr, res_size)) {
        return {false, L"安装包内部数据损坏（未找到资源）。"};
    }

    mr::pkg::CompressedHeader chdr{};
    memcpy(&chdr, res_ptr, sizeof(chdr));
    if (memcmp(chdr.magic, mr::pkg::kCompressedMagic, 4) != 0 || chdr.uncompressed_size == 0 ||
        chdr.uncompressed_size > 256ULL * 1024 * 1024) {
        return {false, L"安装包压缩头无效。"};
    }

    report(15, L"正在解压程序文件...");
    DECOMPRESSOR_HANDLE decomp = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decomp)) {
        return {false, L"系统不支持 LZMS 解压组件。"};
    }

    std::vector<uint8_t> archive(static_cast<size_t>(chdr.uncompressed_size));
    SIZE_T actual_out = 0;
    BOOL dec_ok = Decompress(
        decomp,
        const_cast<uint8_t*>(res_ptr + sizeof(chdr)),
        res_size - sizeof(chdr),
        archive.data(),
        archive.size(),
        &actual_out);
    CloseDecompressor(decomp);
    if (!dec_ok || actual_out != archive.size()) {
        return {false, L"解压安装包数据失败。"};
    }

    if (archive.size() < sizeof(mr::pkg::ArchiveHeader)) {
        return {false, L"归档数据格式错误。"};
    }
    mr::pkg::ArchiveHeader ahdr{};
    memcpy(&ahdr, archive.data(), sizeof(ahdr));
    if (memcmp(ahdr.magic, mr::pkg::kArchiveMagic, 4) != 0 || ahdr.file_count == 0) {
        return {false, L"归档文件头无效。"};
    }

    if (!mr::inst::EnsureDirectoryTree(target_dir)) {
        return {false, L"无法创建安装目录，请检查权限或更换目录。"};
    }

    const fs::path target_app = fs::path(target_dir) / L"MarkReader.exe";
    const bool upgrading_dir = opts.is_upgrade || fs::exists(target_app);

    // Ensure running target MarkReader.exe is not holding a write lock before unpacking files
    if (!opts.silent && fs::exists(target_app)) {
        while (true) {
            HANDLE hchk = CreateFileW(
                target_app.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hchk != INVALID_HANDLE_VALUE) {
                CloseHandle(hchk);
                break;
            }
            DWORD err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) {
                break;
            }
            int choice = MessageBoxW(
                progress_hwnd,
                L"检测到 MarkReader 正在运行，请先关闭所有 MarkReader 窗口后点击“重试”继续更新。",
                L"MarkReader 更新程序",
                MB_RETRYCANCEL | MB_ICONWARNING);
            if (choice != IDRETRY) {
                return {false, L"已取消更新（MarkReader 仍在运行）。"};
            }
        }
    }

    size_t cursor = sizeof(ahdr);
    for (uint32_t i = 0; i < ahdr.file_count; ++i) {
        if (cursor + sizeof(uint32_t) > archive.size()) return {false, L"归档记录损坏。"};
        uint32_t path_len = 0;
        memcpy(&path_len, archive.data() + cursor, sizeof(path_len));
        cursor += sizeof(path_len);

        if (path_len == 0 || path_len > 1024 || cursor + path_len + sizeof(uint64_t) > archive.size()) {
            return {false, L"归档路径长度异常。"};
        }
        std::string rel_utf8(reinterpret_cast<const char*>(archive.data() + cursor), path_len);
        cursor += path_len;

        uint64_t file_size = 0;
        memcpy(&file_size, archive.data() + cursor, sizeof(file_size));
        cursor += sizeof(file_size);

        if (cursor + file_size > archive.size()) {
            return {false, L"归档文件内容截断。"};
        }

        std::wstring rel_w = mr::inst::Utf8ToWide(rel_utf8.data(), rel_utf8.size());
        if (rel_w.empty() || rel_w.find(L"..") != std::wstring::npos || rel_w[0] == L'\\' || rel_w[0] == L'/') {
            return {false, L"检测到非法文件路径。"};
        }

        fs::path out_file = fs::path(target_dir) / fs::path(rel_w);
        mr::inst::EnsureDirectoryTree(out_file.parent_path().wstring());

        HANDLE hf = CreateFileW(
            out_file.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            return {false, L"写入文件失败（若程序正在运行请先关闭）：\n" + out_file.wstring()};
        }
        size_t written_total = 0;
        while (written_total < file_size) {
            DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(file_size - written_total, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(hf, archive.data() + cursor + written_total, chunk, &written, nullptr)) {
                CloseHandle(hf);
                return {false, L"写入磁盘失败：" + out_file.wstring()};
            }
            written_total += written;
        }
        CloseHandle(hf);
        cursor += static_cast<size_t>(file_size);

        int pct = 20 + static_cast<int>((static_cast<double>(i + 1) / ahdr.file_count) * 65.0);
        report(pct, upgrading_dir ? L"正在更新文件..." : L"正在写入文件...");
    }

    report(90, L"正在创建快捷方式与注册卸载信息...");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring app_exe = (fs::path(target_dir) / L"MarkReader.exe").wstring();
    const std::wstring uninst_exe = (fs::path(target_dir) / L"Uninstall.exe").wstring();
    const bool system_wide = mr::inst::IsProcessElevated();

    std::wstring desktop_lnk;
    if (opts.create_desktop) {
        std::wstring desk = system_wide
            ? mr::inst::GetKnownDir(FOLDERID_PublicDesktop)
            : mr::inst::GetKnownDir(FOLDERID_Desktop);
        if (desk.empty()) desk = mr::inst::GetKnownDir(FOLDERID_Desktop);
        if (!desk.empty()) {
            desktop_lnk = (fs::path(desk) / L"MarkReader.lnk").wstring();
            mr::inst::CreateShortcut(
                desktop_lnk, app_exe, L"", target_dir,
                L"MarkReader — 轻量原生 Markdown 阅读器", app_exe);
        }
    }

    std::wstring start_menu_dir;
    if (opts.create_start_menu) {
        std::wstring sm = system_wide
            ? mr::inst::GetKnownDir(FOLDERID_CommonPrograms)
            : mr::inst::GetKnownDir(FOLDERID_Programs);
        if (sm.empty()) sm = mr::inst::GetKnownDir(FOLDERID_Programs);
        if (!sm.empty()) {
            start_menu_dir = (fs::path(sm) / L"MarkReader").wstring();
            mr::inst::CreateShortcut(
                (fs::path(start_menu_dir) / L"MarkReader.lnk").wstring(),
                app_exe, L"", target_dir,
                L"MarkReader — 轻量原生 Markdown 阅读器", app_exe);
            mr::inst::CreateShortcut(
                (fs::path(start_menu_dir) / L"卸载 MarkReader.lnk").wstring(),
                uninst_exe, L"", target_dir,
                L"卸载 MarkReader", uninst_exe);
        }
    }

    mr::inst::RegisterAppAndUninstall(
        system_wide,
        target_dir,
        app_exe,
        uninst_exe,
        opts.associate_md,
        desktop_lnk,
        start_menu_dir,
        static_cast<DWORD>(std::max<uint64_t>(1, archive.size() / 1024)));

    CoUninitialize();
    report(100, upgrading_dir ? L"更新完成！" : L"安装完成！");
    return {true, L""};
}

void UpdateFontsAndLayout(InstallerState* s) {
    if (s->font_title) DeleteObject(s->font_title);
    if (s->font_sub) DeleteObject(s->font_sub);
    if (s->font_ui) DeleteObject(s->font_ui);
    if (s->font_bold) DeleteObject(s->font_bold);

    auto mk = [&](int dip_h, int weight) {
        return CreateFontW(
            -Dip(s, static_cast<float>(dip_h)), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    s->font_title = mk(20, FW_BOLD);
    s->font_sub = mk(13, FW_NORMAL);
    s->font_ui = mk(13, FW_NORMAL);
    s->font_bold = mk(13, FW_SEMIBOLD);

    auto setf = [](HWND h, HFONT f) {
        if (h && f) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    setf(s->edit_path, s->font_ui);
    setf(s->btn_browse, s->font_ui);
    setf(s->lbl_space, s->font_sub);
    setf(s->chk_desktop, s->font_ui);
    setf(s->chk_startmenu, s->font_ui);
    setf(s->chk_assoc, s->font_ui);
    setf(s->lbl_status, s->font_ui);
    setf(s->chk_launch, s->font_ui);
    setf(s->btn_install, s->font_bold);
    setf(s->btn_cancel, s->font_ui);

    // Position child controls cleanly
    MoveWindow(s->edit_path, Dip(s, 28), Dip(s, 130), Dip(s, 396), Dip(s, 28), TRUE);
    MoveWindow(s->btn_browse, Dip(s, 434), Dip(s, 129), Dip(s, 78), Dip(s, 30), TRUE);
    MoveWindow(s->lbl_space, Dip(s, 28), Dip(s, 166), Dip(s, 480), Dip(s, 20), TRUE);

    MoveWindow(s->chk_desktop, Dip(s, 28), Dip(s, 198), Dip(s, 440), Dip(s, 24), TRUE);
    MoveWindow(s->chk_startmenu, Dip(s, 28), Dip(s, 228), Dip(s, 440), Dip(s, 24), TRUE);
    MoveWindow(s->chk_assoc, Dip(s, 28), Dip(s, 258), Dip(s, 440), Dip(s, 24), TRUE);

    MoveWindow(s->lbl_status, Dip(s, 28), Dip(s, 294), Dip(s, 484), Dip(s, 20), TRUE);
    MoveWindow(s->progress, Dip(s, 28), Dip(s, 318), Dip(s, 484), Dip(s, 12), TRUE);
    MoveWindow(s->chk_launch, Dip(s, 28), Dip(s, 302), Dip(s, 260), Dip(s, 24), TRUE);

    MoveWindow(s->btn_install, Dip(s, 300), Dip(s, 355), Dip(s, 112), Dip(s, 34), TRUE);
    MoveWindow(s->btn_cancel, Dip(s, 422), Dip(s, 355), Dip(s, 90), Dip(s, 34), TRUE);
}

void UpdateElevationShield(InstallerState* s) {
    if (!s->edit_path || !s->btn_install || !s->lbl_space) return;
    int len = GetWindowTextLengthW(s->edit_path);
    std::wstring path(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(s->edit_path, path.data(), len + 1);
    path.resize(static_cast<size_t>(len));
    path = TrimPath(path);

    bool need_admin = !mr::inst::IsProcessElevated() && !mr::inst::CanWriteDirectory(path);
    SendMessageW(s->btn_install, BCM_SETSHIELD, 0, need_admin ? TRUE : FALSE);

    const bool has_existing = fs::exists(fs::path(path) / L"MarkReader.exe");
    s->opts.is_upgrade = has_existing;
    if (!s->installing && !s->finished) {
        SetWindowTextW(s->btn_install, has_existing ? L"立即更新" : L"立即安装");
        RECT hdr{0, 0, Dip(s, 540), Dip(s, 88)};
        InvalidateRect(s->hwnd, &hdr, FALSE);
    }

    double mb = static_cast<double>(s->payload_uncompressed_bytes) / (1024.0 * 1024.0);
    int tenths = static_cast<int>(mb * 10.0 + 0.5);
    std::wstring text = L"所需磁盘空间: 约 " + std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" MB";
    if (has_existing) {
        text += need_admin
            ? L"  （检测到已安装版本，将提权覆盖更新）"
            : L"  （检测到已安装版本，将直接覆盖更新）";
    } else if (need_admin) {
        text += L"  （安装到系统目录将请求管理员权限）";
    }
    SetWindowTextW(s->lbl_space, text.c_str());
}

void BrowseInstallFolder(InstallerState* s) {
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"选择 MarkReader 安装目录");
        if (SUCCEEDED(dlg->Show(s->hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    std::wstring chosen = TrimPath(path);
                    const std::wstring suffix = L"\\MarkReader";
                    if (chosen.size() < suffix.size() ||
                        _wcsicmp(chosen.c_str() + chosen.size() - suffix.size(), suffix.c_str()) != 0) {
                        chosen += suffix;
                    }
                    SetWindowTextW(s->edit_path, chosen.c_str());
                    UpdateElevationShield(s);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
}

void BeginInstallFromUi(InstallerState* s) {
    if (s->installing) return;
    if (s->finished) {
        if (SendMessageW(s->chk_launch, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            fs::path app = fs::path(TrimPath(s->opts.install_dir)) / L"MarkReader.exe";
            ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, TrimPath(s->opts.install_dir).c_str(), SW_SHOWNORMAL);
        }
        DestroyWindow(s->hwnd);
        return;
    }

    int len = GetWindowTextLengthW(s->edit_path);
    std::wstring path(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(s->edit_path, path.data(), len + 1);
    path.resize(static_cast<size_t>(len));
    path = TrimPath(path);

    if (!IsValidInstallDir(path)) {
        MessageBoxW(s->hwnd, L"请输入有效的完整安装路径（例如 C:\\Program Files\\MarkReader）。", L"MarkReader 安装程序", MB_ICONWARNING);
        return;
    }

    s->opts.install_dir = path;
    s->opts.create_desktop = (SendMessageW(s->chk_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    s->opts.create_start_menu = (SendMessageW(s->chk_startmenu, BM_GETCHECK, 0, 0) == BST_CHECKED);
    s->opts.associate_md = (SendMessageW(s->chk_assoc, BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (!mr::inst::IsProcessElevated() && !mr::inst::CanWriteDirectory(path)) {
        std::wstring args =
            L"--elevated --autoinstall"
            L" --dir=\"" + path + L"\""
            L" --desktop=" + std::wstring(s->opts.create_desktop ? L"1" : L"0") +
            L" --startmenu=" + std::wstring(s->opts.create_start_menu ? L"1" : L"0") +
            L" --assoc=" + std::wstring(s->opts.associate_md ? L"1" : L"0");
        HINSTANCE res = ShellExecuteW(s->hwnd, L"runas", mr::inst::GetExePath().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(res) > 32) {
            DestroyWindow(s->hwnd);
        }
        return;
    }

    s->installing = true;
    EnableWindow(s->edit_path, FALSE);
    EnableWindow(s->btn_browse, FALSE);
    EnableWindow(s->chk_desktop, FALSE);
    EnableWindow(s->chk_startmenu, FALSE);
    EnableWindow(s->chk_assoc, FALSE);
    EnableWindow(s->btn_install, FALSE);
    EnableWindow(s->btn_cancel, FALSE);
    ShowWindow(s->lbl_status, SW_SHOW);
    ShowWindow(s->progress, SW_SHOW);
    SendMessageW(s->progress, PBM_SETPOS, 5, 0);

    InstallOptions copy_opts = s->opts;
    HWND hwnd = s->hwnd;
    std::thread([copy_opts, hwnd]() {
        auto* res = new InstallResult(RunInstallation(copy_opts, hwnd));
        PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}

LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    InstallerState* s = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        s = static_cast<InstallerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = hwnd;
    } else {
        s = reinterpret_cast<InstallerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_CREATE: {
            s->dpi = static_cast<float>(GetDpiForWindow(hwnd));
            if (s->dpi <= 0.0f) s->dpi = 96.0f;
            s->white_brush = CreateSolidBrush(RGB(255, 255, 255));
            s->icon_large = static_cast<HICON>(LoadImageW(
                s->hinst, MAKEINTRESOURCEW(101), IMAGE_ICON, Dip(s, 48), Dip(s, 48), LR_DEFAULTCOLOR));

            COLORREF cap = RGB(248, 250, 252);
            DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));

            s->edit_path = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", s->opts.install_dir.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 100, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_PATH)), s->hinst, nullptr);
            s->btn_browse = CreateWindowExW(
                0, L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 80, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BROWSE)), s->hinst, nullptr);
            s->lbl_space = CreateWindowExW(
                0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE,
                0, 0, 200, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LBL_SPACE)), s->hinst, nullptr);

            s->chk_desktop = CreateWindowExW(
                0, L"BUTTON", L"创建桌面快捷方式",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                0, 0, 200, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_DESKTOP)), s->hinst, nullptr);
            s->chk_startmenu = CreateWindowExW(
                0, L"BUTTON", L"创建开始菜单快捷方式",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                0, 0, 200, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_STARTMENU)), s->hinst, nullptr);
            s->chk_assoc = CreateWindowExW(
                0, L"BUTTON", L"将 .md / .markdown 文件关联到 MarkReader",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                0, 0, 200, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_ASSOC)), s->hinst, nullptr);

            s->lbl_status = CreateWindowExW(
                0, L"STATIC", L"准备就绪",
                WS_CHILD,
                0, 0, 200, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LBL_STATUS)), s->hinst, nullptr);
            s->progress = CreateWindowExW(
                0, PROGRESS_CLASSW, L"",
                WS_CHILD | PBS_SMOOTH,
                0, 0, 200, 12, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)), s->hinst, nullptr);
            SendMessageW(s->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            s->chk_launch = CreateWindowExW(
                0, L"BUTTON", L"安装完成后立即启动 MarkReader",
                WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
                0, 0, 200, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_LAUNCH)), s->hinst, nullptr);
            SendMessageW(s->chk_launch, BM_SETCHECK, BST_CHECKED, 0);

            s->btn_install = CreateWindowExW(
                0, L"BUTTON", s->opts.is_upgrade ? L"立即更新" : L"立即安装",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_INSTALL)), s->hinst, nullptr);
            s->btn_cancel = CreateWindowExW(
                0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 80, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL)), s->hinst, nullptr);

            SendMessageW(s->chk_desktop, BM_SETCHECK, s->opts.create_desktop ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(s->chk_startmenu, BM_SETCHECK, s->opts.create_start_menu ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(s->chk_assoc, BM_SETCHECK, s->opts.associate_md ? BST_CHECKED : BST_UNCHECKED, 0);

            UpdateFontsAndLayout(s);
            UpdateElevationShield(s);

            if (s->opts.auto_install) {
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_INSTALL, BN_CLICKED), 0);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);

            // White main body
            FillRect(hdc, &rc, s->white_brush);

            // Top banner
            RECT banner{0, 0, rc.right, Dip(s, 88)};
            HBRUSH hb_top = CreateSolidBrush(RGB(248, 250, 252));
            FillRect(hdc, &banner, hb_top);
            DeleteObject(hb_top);

            // Footer bar
            RECT footer{0, Dip(s, 340), rc.right, rc.bottom};
            HBRUSH hb_ftr = CreateSolidBrush(RGB(248, 250, 252));
            FillRect(hdc, &footer, hb_ftr);
            DeleteObject(hb_ftr);

            // Hairline dividers
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
            HGDIOBJ old_pen = SelectObject(hdc, pen);
            MoveToEx(hdc, 0, banner.bottom, nullptr);
            LineTo(hdc, rc.right, banner.bottom);
            MoveToEx(hdc, 0, footer.top, nullptr);
            LineTo(hdc, rc.right, footer.top);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);

            if (s->icon_large) {
                DrawIconEx(hdc, Dip(s, 26), Dip(s, 20), s->icon_large, Dip(s, 48), Dip(s, 48), 0, nullptr, DI_NORMAL);
            }

            SetBkMode(hdc, TRANSPARENT);
            HGDIOBJ old_font = SelectObject(hdc, s->font_title);
            SetTextColor(hdc, RGB(30, 41, 59));
            RECT tr{Dip(s, 88), Dip(s, 18), rc.right - Dip(s, 20), Dip(s, 48)};
            const wchar_t* title_txt = s->finished
                ? (s->opts.is_upgrade ? L"MarkReader 更新完成" : L"MarkReader 安装完成")
                : (s->opts.is_upgrade ? L"更新 MarkReader" : L"安装 MarkReader");
            DrawTextW(hdc, title_txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, s->font_sub);
            SetTextColor(hdc, RGB(100, 116, 139));
            RECT sr{Dip(s, 88), Dip(s, 48), rc.right - Dip(s, 20), Dip(s, 72)};
            std::wstring sub_txt = L"轻量原生 Markdown 阅读器 v1.1.5 · 支持公式、代码块、图片与微调";
            if (s->opts.is_upgrade && !s->finished) {
                if (!s->opts.existing_version.empty()) {
                    sub_txt = L"检测到已安装 v" + s->opts.existing_version + L" · 即将就地升级至 v1.1.5";
                } else {
                    sub_txt = L"检测到已安装旧版本 · 即将就地更新至 v1.1.5";
                }
            }
            DrawTextW(hdc, sub_txt.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, s->font_bold);
            SetTextColor(hdc, RGB(51, 65, 85));
            RECT lr{Dip(s, 28), Dip(s, 104), Dip(s, 300), Dip(s, 126)};
            DrawTextW(hdc, L"安装路径", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, old_font);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            HWND ctrl = reinterpret_cast<HWND>(lp);
            SetBkMode(hdc, TRANSPARENT);
            if (ctrl == s->lbl_space) {
                SetTextColor(hdc, RGB(100, 116, 139));
            } else {
                SetTextColor(hdc, RGB(31, 35, 40));
            }
            return reinterpret_cast<LRESULT>(s->white_brush);
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IDC_EDIT_PATH && code == EN_CHANGE) {
                UpdateElevationShield(s);
            } else if (id == IDC_BTN_BROWSE) {
                BrowseInstallFolder(s);
            } else if (id == IDC_BTN_INSTALL) {
                BeginInstallFromUi(s);
            } else if (id == IDC_BTN_CANCEL) {
                if (!s->installing) DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_APP_PROGRESS: {
            auto* txt = reinterpret_cast<std::wstring*>(lp);
            SendMessageW(s->progress, PBM_SETPOS, wp, 0);
            if (txt) {
                SetWindowTextW(s->lbl_status, txt->c_str());
                delete txt;
            }
            return 0;
        }

        case WM_APP_DONE: {
            auto* res = reinterpret_cast<InstallResult*>(lp);
            s->installing = false;
            if (res && res->ok) {
                s->finished = true;
                ShowWindow(s->progress, SW_HIDE);
                ShowWindow(s->lbl_status, SW_HIDE);
                ShowWindow(s->chk_launch, SW_SHOW);
                SendMessageW(s->btn_install, BCM_SETSHIELD, 0, FALSE);
                SetWindowTextW(s->btn_install, L"完成");
                EnableWindow(s->btn_install, TRUE);
                EnableWindow(s->btn_cancel, FALSE);
                ShowWindow(s->btn_cancel, SW_HIDE);
                InvalidateRect(hwnd, nullptr, TRUE);
            } else {
                std::wstring err = res ? res->error_msg : L"安装失败。";
                MessageBoxW(hwnd, err.c_str(), L"安装失败", MB_ICONERROR);
                EnableWindow(s->edit_path, TRUE);
                EnableWindow(s->btn_browse, TRUE);
                EnableWindow(s->chk_desktop, TRUE);
                EnableWindow(s->chk_startmenu, TRUE);
                EnableWindow(s->chk_assoc, TRUE);
                EnableWindow(s->btn_install, TRUE);
                EnableWindow(s->btn_cancel, TRUE);
                ShowWindow(s->progress, SW_HIDE);
                ShowWindow(s->lbl_status, SW_HIDE);
            }
            delete res;
            return 0;
        }

        case WM_CLOSE:
            if (!s->installing) DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (s->white_brush) DeleteObject(s->white_brush);
            if (s->font_title) DeleteObject(s->font_title);
            if (s->font_sub) DeleteObject(s->font_sub);
            if (s->font_ui) DeleteObject(s->font_ui);
            if (s->font_bold) DeleteObject(s->font_bold);
            if (s->icon_large) DestroyIcon(s->icon_large);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

InstallOptions ParseCommandLine() {
    InstallOptions opts;
    opts.install_dir = DefaultInstallDir();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opts;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (_wcsicmp(a.c_str(), L"/S") == 0) {
            opts.silent = true;
        } else if (_wcsicmp(a.c_str(), L"--autoinstall") == 0) {
            opts.auto_install = true;
        } else if (_wcsicmp(a.c_str(), L"--elevated") == 0) {
            opts.elevated_relaunch = true;
        } else if (a.rfind(L"/D=", 0) == 0 || a.rfind(L"/d=", 0) == 0) {
            opts.install_dir = TrimPath(a.substr(3));
            opts.explicit_dir = true;
        } else if (a.rfind(L"--dir=", 0) == 0) {
            opts.install_dir = TrimPath(a.substr(6));
            opts.explicit_dir = true;
        } else if (a == L"--desktop=0") {
            opts.create_desktop = false;
        } else if (a == L"--desktop=1") {
            opts.create_desktop = true;
        } else if (a == L"--startmenu=0") {
            opts.create_start_menu = false;
        } else if (a == L"--startmenu=1") {
            opts.create_start_menu = true;
        } else if (a == L"--assoc=0") {
            opts.associate_md = false;
        } else if (a == L"--assoc=1") {
            opts.associate_md = true;
        }
    }
    LocalFree(argv);

    ExistingInstallInfo existing = DetectExistingInstall();
    if (existing.found) {
        if (!opts.explicit_dir && !existing.install_dir.empty()) {
            opts.install_dir = existing.install_dir;
            opts.is_upgrade = true;
            opts.existing_version = existing.version;
            if (!opts.elevated_relaunch) {
                opts.create_desktop = existing.create_desktop;
                opts.create_start_menu = existing.create_start_menu;
                opts.associate_md = existing.associate_md;
            }
        } else if (opts.explicit_dir &&
                   _wcsicmp(TrimPath(opts.install_dir).c_str(), TrimPath(existing.install_dir).c_str()) == 0) {
            opts.is_upgrade = true;
            opts.existing_version = existing.version;
        }
    } else if (opts.explicit_dir && fs::exists(fs::path(opts.install_dir) / L"MarkReader.exe")) {
        opts.is_upgrade = true;
    }

    return opts;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    InstallOptions opts = ParseCommandLine();

    if (opts.silent) {
        if (!mr::inst::IsProcessElevated() && !mr::inst::CanWriteDirectory(opts.install_dir) && !opts.elevated_relaunch) {
            std::wstring args =
                L"/S --elevated"
                L" --dir=\"" + opts.install_dir + L"\""
                L" --desktop=" + std::wstring(opts.create_desktop ? L"1" : L"0") +
                L" --startmenu=" + std::wstring(opts.create_start_menu ? L"1" : L"0") +
                L" --assoc=" + std::wstring(opts.associate_md ? L"1" : L"0");
            HINSTANCE res = ShellExecuteW(nullptr, L"runas", mr::inst::GetExePath().c_str(), args.c_str(), nullptr, SW_HIDE);
            CoUninitialize();
            return (reinterpret_cast<INT_PTR>(res) > 32) ? 0 : 1;
        }
        InstallResult res = RunInstallation(opts, nullptr);
        CoUninitialize();
        return res.ok ? 0 : 1;
    }

    InstallerState state;
    state.hinst = hInstance;
    state.opts = opts;
    state.payload_uncompressed_bytes = QueryPayloadUncompressedSize();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SetupWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MarkReaderSetupWindow";
    if (!RegisterClassExW(&wc)) return 1;

    HDC hdc = GetDC(nullptr);
    int sys_dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(nullptr, hdc);
    float scale = static_cast<float>(std::max(96, sys_dpi)) / 96.0f;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{0, 0, static_cast<LONG>(540 * scale + 0.5f), static_cast<LONG>(405 * scale + 0.5f)};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int wx = work.left + ((work.right - work.left) - win_w) / 2;
    int wy = work.top + ((work.bottom - work.top) - win_h) / 2;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"MarkReader 安装程序",
        style, wx, wy, win_w, win_h,
        nullptr, nullptr, hInstance, &state);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
