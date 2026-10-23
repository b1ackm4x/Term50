# Term50 中文支持

已于 2026-09-20 安装到实际 Q20，并通过正式版本的默认终端启动与 SSH 检查。

本次修改补齐终端正文的中文字体回退和宽字符绘制。英文、数字和已有 Powerline 符号继续使用用户选择的主字体；主字体缺字时，使用 BB10 系统中文字形。Q20 上实际存在并使用的常规字体为 `MHeiM.GB18030_200910.ttf`，不需要另外安装字体。

![Q20 上的中文验证画面](assets/chinese-q20.png)

## 实现

- 字体回退只对终端主字体启用，不干扰设置面板原有的字体探测。
- 保留主字体的行高、英文列宽和基线，中文字形限制在终端提供的两列范围内；全角标点保留原字体的左右留白。
- Ghostty 继续负责字符列宽。宽字符的首格绘制两列，尾格不再覆盖中文的右半边；行末换行占位仍然正常清理。
- 粗体、斜体、下划线与现有颜色继续生效，字形缓存区分目标格子尺寸。
- BB10 的 C locale 把中文判断为不可打印字符。配套 Zsh 5.9 使用上游 `--enable-unicode9` 选项，让行编辑器使用内置 Unicode 可打印性和列宽表，避免输入中的中文显示成 `<4e2d>`。

## 验证

字体检查覆盖多个字号、简体、繁体、全角标点和八种样式组合；检查启用回退前后英文像素一致。终端模型检查覆盖双列、行末自动换行以及宽字符被英文覆盖后的格子恢复。

Q20 实测通过：简繁中文、英文数字混排、粗体/斜体/下划线、边框对齐、行末换行；新的交互 Zsh 直接显示中文。输入 `中文` 后退格一次再输入 `字`，实际执行结果为 `中字`。`mac` 和 `macvpn` 的远程 UTF-8 回显均通过。输入验证通过终端控制接口送入 UTF-8 文本，并通过真实按键处理函数送入退格事件。

原始验收记录保存在本机工作区的 `q-term/cjk-validation/`（Term50 Git 仓库外），包括 `native-checks.txt`、`editing-checks.txt`、`production-checks.txt` 和 `production-package.json`；这些原始文件未随仓库发布。

## 安装与回退

正式安装包为 `dist/Term50-cjk-oh-my-zsh-bbtailscale.bar`。原有 Oh My Zsh、颜色、历史菜单及 SSH 配置保留。升级前的运行文件和配置保存在设备的 `data/shell/backups/before-cjk-20260920-131855`，其路径另记在 `data/shell/cjk-backup-path`。

需要回退时，重新安装上一版 `Term50-oh-my-zsh-bbtailscale.bar`，并将备份的 `zsh` 恢复到 `data/omz-runtime/bin/zsh`，备份的 `source-pins.json` 恢复到 `data/omz-runtime/share/source-pins.json`，然后重新启动应用。

临时启动钩子已恢复，设备测试程序、凭据和样例文件已清理；Mac 上的临时安装服务及收集服务已关闭。正式应用保留一个输入模式终端会话，安装包内没有测试接口。原始清理记录为本机工作区 `q-term/cjk-validation/cleanup-checks.txt`，不在此 Git 仓库中。

本次没有加入中文输入法；支持已有 UTF-8 文本的显示和编辑。罕见字的覆盖取决于系统字体。
