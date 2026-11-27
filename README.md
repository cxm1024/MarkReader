# MarkReader

[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20(x64)-0969DA)](#)
[![Tech](https://img.shields.io/badge/Stack-C%2B%2B20%20%7C%20Win32%20%7C%20Direct2D%20%7C%20DirectWrite-1A7F37)](#)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)

专注纯阅读体验的极简原生 Windows Markdown 阅读器。  
拒绝 Electron / WebView 浏览器套壳，基于 **C++20 + Win32 + Direct3D 11 + Direct2D 1.3 / DirectWrite + WIC** 从零构建。

> **开发声明**：本项目全部应用程序源码、Direct2D 排版渲染引擎、MicroTeX Direct2D 后端以及自解压安装/卸载器均由 **Gemini 4 Hydrogen** 编写与构建验证。

---

## 核心亮点

- **专注阅读**：纯粹的阅读器定位——无编辑器、无导出弹窗、无冗余字数统计与广告干扰。采用 44 DIP 单层无边框融合顶栏与可折叠大纲侧栏（收起侧栏后自动放宽正文排版列宽并锚定当前阅读位置）。
- **超低延迟**：彻底摆脱 Chromium / Node 初始化链路，Windows 实测从进程冷启动到首屏窗口完整呈现仅约 **67 ms**；借助 Direct2D 1.3 硬件加速与字体轮廓/公式几何缓存，高速滚动、实时搜索与无级字号缩放保持高帧率响应。
- **超低占用**：单进程原生桌面应用，无多进程浏览器内核常驻开销。实测完整加载 Direct2D GPU 管线与离线 LaTeX 数学排版引擎后的进程私有内存（Private Bytes）仅约 **65.8 MB**（工作集约 **75.9 MB**），静态阅读期间 CPU 占用 **0%**。
- **超小安装包**：Release 版便携主程序仅约 **3.4 MB**（`3,585,024` 字节），包含完整离线 LaTeX 数学字体、自定义路径 GUI 与独立卸载器的独立安装程序 `MarkReaderSetup.exe` 通过 Windows 原生 LZMS（`Cabinet.dll`）高压缩打包后仅约 **3.7 MB**（`3,871,744` 字节），静态链接运行时，仅依赖系统内置 DLL，开箱即用。

---

## 阅读功能一览

- **完整的 LaTeX 数学公式支持**
  - 内置 [MicroTeX](https://github.com/NanoMichael/MicroTeX) 排版引擎与 Computer Modern 矢量字体，无需安装外部 TeX 发行版。
  - 支持行内公式（`$...$` 与 `\(...\)`，兼容紧邻中英文与数字排版如 `第$k$个` 且不额外留白，长公式支持在顶层关系/运算符处自动折行）与居中块级公式（`$$...$$`、`\[...\]` 及 ```` ```math ````）。
  - 结合 `IDWriteFontFace::GetGlyphRunOutline` 优化数学符号字重，悬停块级公式即可一键复制 LaTeX 源码。
- **现代表格与代码块**
  - 智能列宽分配与单元格边界裁剪；遇到过宽多列表格时不再硬压缩，自动启用平滑横向滚动条与 `Shift + 滚轮` / 触控板横滑浏览。
  - 支持多语言语法高亮代码块、圆角行内代码徽章、横向滚动条以及右上角一键复制按钮。
- **浏览器级浏览交互**
  - 支持**鼠标中键快捷滚动**：正文区域中键激活上下无级平滑滚动（`▲ • ▼`），在过宽表格/代码块上中键激活左右横向滚动（`◄ • ►`），支持轻点锁定或按住拖放。
  - 支持跨段落/表格/代码块的划词文本选取复制、内部章节与相对 `.md` 文件跳转、文档内搜索（原生 Direct2D 输入框 + 完整 Windows 中文 IME 输入法跟随）。
- **图片、主题与缩放**
  - 通过 WIC 支持 PNG / JPEG / GIF / WebP / BMP，通过 Direct2D 原生渲染 SVG 矢量图（含 `<text>` 标注），支持本地相对路径与远程图片异步缓存，点击图片可灯箱放大预览。
  - 支持 `50% – 250%` 字号缩放以及 **亮色 (Light)**、**暗色 (Dark)**、**护眼 (Sepia)** 三套主题，支持外部文件保存自动热重载。

---

## 下载安装

前往本仓库的 [Releases](https://github.com/cxm1024/MarkReader/releases) 页面下载最新 **`MarkReaderSetup.exe`**：

- 安装包默认安装至 `C:\Program Files\MarkReader`（无管理员权限时支持切换至自定义用户目录并自动处理 UAC 提权）。
- 支持选择是否创建桌面快捷方式与开始菜单快捷方式，并内置独立的 `Uninstall.exe` 卸载程序（支持静默安装 `/S /D=<路径>`）。

---

## 快捷键与鼠标操作

| 操作 / 快捷键 | 功能 |
| :--- | :--- |
| `Ctrl + O` / 拖放 `.md` 文件 | 打开 Markdown 文档 |
| `Ctrl + B` / 顶栏侧栏图标 | 展开 / 收起大纲目录（同步动态调整阅读栏宽） |
| `Ctrl + F` / 顶栏搜索图标 | 文档内搜索（支持中文输入法，`Enter` / `Shift+Enter` 切换匹配） |
| `Ctrl + +` / `Ctrl + -` / `Ctrl + 滚轮` | 放大 / 缩小字号（`50% – 250%`） |
| `Ctrl + 0` | 重置字号为 `100%` |
| **鼠标中键**（点击或按住拖动） | 快捷滚动（正文上下滚动；宽表格 / 代码块内左右横向滚动） |
| `Shift + 滚轮` / 拖动底部滚动条 | 横向滚动过宽表格或代码块 |
| `Alt + ←` / `Alt + →` | 跨文档访问历史后退 / 前进 |
| `Ctrl + C` / `Ctrl + A` | 复制选中文本（公式还原为 `$...$`） / 全选 |
| `F5` / `Ctrl + R` | 重新加载当前文档 |
| `Esc` | 退出中键滚动 / 关闭灯箱 / 关闭搜索栏 / 清除选区 |

---

## 从源码构建（Windows + MinGW64 + CMake）

需要具备 **CMake 3.20+**、支持 C++20 的 **MinGW-w64 (GCC)**（已验证 GCC 15.2.0）以及 Windows SDK / MinGW Win32/Direct2D 头文件：

```powershell
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

构建完成后产出：
- 便携主程序：`build\MarkReader.exe`（需搭配同目录 `build\res\` 数学字体资源）
- 自解压安装包：`build\MarkReaderSetup.exe`

---

## 开源致谢

- [md4c](https://github.com/mity/md4c) (MIT License) — 高性能 C 语言 CommonMark/GFM 解析器（本项目对行内 `$` 分隔规则做了贴合中文混排的轻量增强）。
- [MicroTeX](https://github.com/NanoMichael/MicroTeX) (MIT License) — 轻量跨平台 LaTeX 公式排版引擎（本项目为其实现了 Direct2D 1.3 + DirectWrite 矢量轮廓增强渲染后端）。
- [tinyxml2](https://github.com/leethomason/tinyxml2) (zlib License) — XML 解析支持库。
