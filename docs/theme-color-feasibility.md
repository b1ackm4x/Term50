# Term50 主题、状态栏与真彩色可行性

这是 2026-09-20 的历史可行性快照，以下“当前”仅指当日构建。后续已加入 [中文正文回退与实机验证](chinese-support.md)，并有 [Oh My Zsh 与 Git 的后续实机记录](oh-my-zsh-q20.md)；请以这些记录及现行源码判断已实现状态。

调研日期：2026-09-20。范围：当前 Term50 源码及本轮此前已安装的 `Term50-unified-history-bbtailscale.bar`；结合 Zsh、Powerlevel10k 等项目官方文档。此次为可行性调查，没有安装主题、修改用户配置或重新部署应用，也没有新做手机色卡/渐变实测。

## 结论

可以做。手机 Shell 本来就是打包的 Zsh 5.9，彩色目录、主机名、执行结果、双行提示符和右侧提示符都属于 Zsh 的现成功能。终端当前也已经具备 ANSI、256 色及 RGB 真彩色的数据通路。无需为此更换 Shell 或重写终端核心。

| 功能 | 当前基础 | 后续工作 |
| --- | --- | --- |
| 彩色目录、主机、执行结果 | Zsh 提示符支持 | 轻量主题配置，适配窄屏和已有历史组件 |
| Powerline 风格色块与箭头 | 四款随包字体中三款含常用箭头 | 字宽、连续背景和换行实机校验 |
| 24 位真彩色文字/背景 | 解析、RGB 传递、RGBA8888 绘制均存在 | 能力声明、渐变实测、SSH 与 tmux 分别验证 |
| 应用整体配色主题 | 核心提供前景、背景、光标和调色板接口 | 将持久设置接入核心，并提供主题选项 |
| 固定目录状态栏 | 已有原生标签栏布局；核心能读取 OSC 7 目录 | Shell 上报、每标签元数据、保留屏幕空间、远端恢复逻辑 |
| 丰富图标/彩色 emoji | 当前支持不完整 | 字体补充、字体回退及字形渲染工作，不能与真彩色混为一谈 |

## 已确认的具体问题

1. `src/main.c:terminal_setenv()` 设置 `TERM=xterm-256color`。正式包里的 `native/bbnix/terminfo/x/xterm-256color` 数值能力 `colors` 为 256。源码和随包环境清单未设置 `COLORTERM=truecolor`；这可能让按环境检测的应用选择较低颜色模式，但不能仅凭此认定某个具体应用的故障原因。
2. `src/ghostty_bridge.c:gb_resolve_style_color()` 同时处理调色板和 RGB；`render_ghostty_cell()` 将 RGB 交给渲染器；`font.c` 使用三个 8 位通道混合文字颜色；`platform_screen.c` 创建 RGBA8888 窗口。当前链路没有把所有颜色强制压缩成 16 色或 256 色。此为源码证据，尚非本轮手机渐变实测结论。
3. `text_color`、`background_color` 仍被 Lua 配置加载和导出，但当前正文渲染读取 Ghostty 的默认颜色，创建终端时没有将这两项配置传给 Ghostty。修改这些旧配置不会完成预期的正文主题设置。现有库头文件已提供 `GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND/BACKGROUND/CURSOR/PALETTE`，可以直接接入。
4. 正式包包含 `promptinit`、`colors`、`add-zsh-hook`、`vcs_info` 和多款 Zsh 内置主题函数；不必先安装完整 Oh My Zsh。包内有 tmux，未发现 `native/bbnix/bin/git`；手机主题的 Git 状态不能假定已有可用 Git，需检测并优雅隐藏。Mac 端应独立检测。
5. 直接读取正式包字体的 cmap：Iosevka Term、JetBrains Mono、Source Code Pro 含 U+E0B0/U+E0B2 分段箭头，Ubuntu Mono 不含。四款均缺少此次抽查的 U+F07B、U+F418 图标，以及 U+4E2D、U+1F600。不能把这些字体当成完整 Nerd Font 或中文/emoji 字体。
6. 正文只有一个字体，没有按字符寻找备用字体；中文设置界面使用独立字体，因此界面能显示中文不代表正文已有中文回退。桥接层只把一个 grapheme 的首个码点交给绘制层；FreeType 绘制只处理灰度位图，未实现彩色字形绘制。组合字符及复杂 emoji 需要另行处理。

## 两种“状态栏”

**随命令行出现的彩色提示栏**最容易实现：第一行用色块显示 `Q20 / Mac`、缩短后的目录、上一条命令的结果，第二行留给输入。窄屏下优先保留目录，自动隐藏时间等次要项。Zsh 原生 `%~`、`%F`、`%K`、条件提示及 `RPROMPT` 可以完成大部分内容。历史列表、长命令、多行编辑需要共同验证。

**固定在顶部或底部的原生状态栏**需要终端与 Shell 配合。Zsh 可在 `chpwd/precmd` 上报 OSC 7 目录，当前 Ghostty C API 有对应的目录读取接口，但 Term50 尚未接入。每个标签应保存独立主机/目录状态；远端 Mac 需安装同类上报逻辑。不能从 Mac 的本地 SSH 进程工作目录推断远端目录。退出 SSH 时由本地 Shell 重新上报，并处理全屏应用、屏幕尺寸和不支持上报的会话。目录视为不可信文本，编码/解码、长度与控制字符需要处理。

也可先用已有 tmux 做固定状态栏，但需引入 tmux 会话和另一层快捷键、颜色协商。若目标是与悬浮入口和现有标签页一致，原生栏更适合长期方案。

## 主题选择建议

第一版采用小型纯 Zsh 主题，提供简洁和分段两种外观；先使用已存在的箭头或 ASCII，避免把额外二进制依赖作为基本显示的前提。颜色可采用常见深色主题风格，终端调色板与 Shell 提示符分别配置。

Powerlevel10k 可以借鉴外观；其官方说明允许默认字体，完整样式才需 Nerd Fonts。但它的 gitstatus 后端官方运行平台清单没有 QNX，不能当成可直接安装的 BB10 方案。SSH 到 Mac 时主题在 Mac 上运行，这个后端限制不同；图标最终仍由手机字体绘制。

## 建议实施顺序和验收

1. 临时色卡验证 ANSI/256 色、`38;2`/`48;2` 真彩色、背景连续色块、默认色复位，覆盖本地和两条 SSH 路径。通过后补齐能力声明；SSH 不假定自动传递 `COLORTERM`。
2. 接通原生配色设置，再安装轻量双行 Zsh 主题；手机与 Mac 的 SSH 会话保持一致视觉，保留本地 Mac 现有偏好。
3. 需要更多图标时补充合适的等宽字体，并验证宽度、缺字和渲染成本。
4. 最后实现固定状态栏及远端目录上报。检查多标签隔离、退出 SSH 后恢复、历史列表重绘、长路径、全屏程序和字体缩放。

本轮只能确认工程可行性和已识别的缺口，不能将源码支持等同于所有应用的显示效果均已验证。

## 官方参考

- [Zsh Prompt Expansion](https://zsh.sourceforge.io/Doc/Release/Prompt-Expansion.html)：目录、条件提示、前景与背景颜色。
- [Zsh Prompt Themes](https://zsh.sourceforge.io/Doc/Release/User-Contributions.html#Prompt-Themes)：内置主题函数、初始化与切换。
- [Zsh Character Highlighting](https://zsh.sourceforge.io/Doc/Release/Zsh-Line-Editor.html#Character-Highlighting)：RGB 颜色与终端能力。
- [Powerlevel10k](https://github.com/romkatv/powerlevel10k)：字体选项、颜色/locale 检测及显示故障说明。
- [gitstatus Requirements](https://github.com/romkatv/gitstatus#requirements)：后端运行平台边界。
