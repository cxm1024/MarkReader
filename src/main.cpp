#include "ui/main_window.h"
#include "core/sample_doc.h"
#include "math/d2d_tex_backend.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // Parse CLI arguments safely with CommandLineToArgvW for full Unicode path support.
    std::wstring initial_file;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2 && argv[1] && argv[1][0] != L'\0') {
            initial_file = argv[1];
        }
        LocalFree(argv);
    }

    mr::MainWindow win;
    if (!win.Create(hInstance, nCmdShow, initial_file)) {
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    mr::MathEngine::Instance().Shutdown();
    return static_cast<int>(msg.wParam);
}
