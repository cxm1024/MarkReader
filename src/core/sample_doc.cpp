#include "core/sample_doc.h"

namespace mr {

const std::string& GetWelcomeMarkdownUtf8() {
    static const std::string kWelcome = R"MD(# MarkReader — 轻量原生 Markdown 阅读器

欢迎使用 **MarkReader**！这是一个完全基于 **Win32 + Direct2D / DirectWrite** 构建的纯阅读型 Markdown 阅读器，无需 WebView 或 Electron，冷启动毫秒级、内存占用极低，同时提供现代优雅的排版与阅读体验。

> [!TIP]
> 你可以直接将 `.md` 文件拖入窗口、按 `Ctrl + O` 打开本地文档，或通过命令行传入文件路径。外部编辑器保存修改后，阅读器会自动同步刷新内容。

---

## 核心阅读功能

### 1. 数学公式渲染（行内 & 行间居中）

基于内嵌 TeX 排版引擎与 Computer Modern 矢量字体，无需安装外部 LaTeX 环境即可高清渲染数学公式：

- **行内公式**：爱因斯坦质能方程 $E = mc^2$，欧拉恒等式 $e^{i\pi} + 1 = 0$，以及行内分式 $\frac{\partial L}{\partial q_i} - \frac{d}{dt}\left(\frac{\partial L}{\partial \dot{q}_i}\right) = 0$ 会自动对齐文本基线。
- **行间居中公式**：支持积分、求和、矩阵、分段函数与多行对齐。

高斯积分公式：

$$
\int_{-\infty}^{+\infty} e^{-x^2}\,dx = \sqrt{\pi}
$$

傅里叶变换与矩阵示例：

$$
\hat{f}(\xi) = \int_{-\infty}^{\infty} f(x)\, e^{-2\pi i x \xi}\,dx, \qquad
A = \begin{pmatrix}
\cos\theta & -\sin\theta \\
\sin\theta & \cos\theta
\end{pmatrix}
$$

分段函数（`cases` 环境）：

$$
\text{ReLU}(x) = \begin{cases}
x, & x > 0 \\
0, & x \le 0
\end{cases}
$$

---

### 2. 代码块与行内代码

行内代码支持圆角高亮徽章，例如 `std::vector<Block>`、`Ctrl + F` 或 `IDWriteTextLayout`。

行间代码块支持多语言语法高亮、语言标签以及右上角一键复制按钮：

```cpp
#include <iostream>
#include <vector>

// 计算前缀和并返回结果
std::vector<int> prefix_sum(const std::vector<int>& nums) {
    std::vector<int> res;
    res.reserve(nums.size());
    int acc = 0;
    for (int v : nums) {
        acc += v;
        res.push_back(acc);
    }
    return res;
}
```

```python
import math

def normal_pdf(x: float, mu: float = 0.0, sigma: float = 1.0) -> float:
    """Standard Gaussian probability density function."""
    coeff = 1.0 / (sigma * math.sqrt(2.0 * math.pi))
    return coeff * math.exp(-0.5 * ((x - mu) / sigma) ** 2)
```

---

### 3. 表格、任务列表与提示框

| 快捷键 / 操作 | 功能说明 | 适用范围 |
| :--- | :---: | ---: |
| `Ctrl + O` / 拖放文件 | 打开 Markdown 文件 | 全局 |
| `Ctrl + B` | 折叠 / 展开侧栏大纲 | 全局 |
| `Ctrl + +` / `Ctrl + -` / `Ctrl + 滚轮` | 缩放字号（50% – 250%） | 阅读区 |
| `Ctrl + 0` | 重置字号为 100% | 阅读区 |
| `Ctrl + F` | 文档内搜索（Enter 下一个） | 全局 |
| `Alt + ←` / `Alt + →` | 跨文档链接后退 / 前进 | 导航 |

#### 阅读体验清单

- [x] **无 WebView / Electron**：纯原生 Direct2D 硬件加速渲染
- [x] **侧栏大纲**：自动提取多级标题，滚动高亮当前章节并支持点击跳转
- [x] **图片显示**：支持 PNG / JPEG / GIF / WebP / BMP 与矢量 SVG（点击图片可放大预览）
- [x] **三种阅读主题**：支持亮色纸张、暗夜深空与暖色护眼模式切换
- [x] **文本划词复制**：支持跨段落、代码块划词选中与 `Ctrl + C` 复制

> [!NOTE]
> 这是一个 `NOTE` 提示框；MarkReader 同样支持 `TIP`、`IMPORTANT`、`WARNING` 和 `CAUTION` 等 GitHub Flavored Markdown 风格提示框，以及标准引用块与脚注[^1]。

---

### 4. 图片显示提示

打开包含相对路径或绝对路径图片的本地 Markdown 文档时，MarkReader 会自动加载本地位图与矢量 SVG；若文档中包含网络图片链接，也会异步缓存显示。点击任意独立图片即可进入**灯箱放大预览模式**。

[^1]: 你可以通过工具栏右上角的主题按钮随时切换 Light / Dark / Sepia 三种配色方案。
)MD";
    return kWelcome;
}

}  // namespace mr
