# Q20 与 SSH 远端统一历史列表

在悬浮菜单选择 **终端** 模式，然后在 Shell 提示符处按 **r** 打开历史列表。本地显示手机的历史；通过 `mac` 或 `macvpn` SSH 登录 Mac 后显示 Mac 的历史。

`r` 发送标准 **Ctrl+R**；外接键盘的 Ctrl+R 和 Q20 的 **Shift → Alt → R** 也保留。它保持终端模式，因此随后可以继续使用 I/K。

- 最近的命令在前，相同命令只显示一次。
- 上下方向键选择；Term50 的「终端」模式也可用 **I / K** 上下选择。
- **Enter** 把选项填回命令行，可以继续编辑；再次 **Enter** 才执行。
- **Esc** 取消并恢复原来的输入。Q20 可按 **Alt → Sym**。
- 在 **tty 模式**输入文字可筛选，不区分大小写；Backspace 删除筛选字符，Ctrl+U 清空筛选。

手机与 Mac 的 SSH 会话加载同一份 Zsh 历史列表脚本，使用相同的选择、筛选、取消和确认逻辑。Mac 的普通本地窗口继续使用原先的 fzf 历史配置。

对于其他 Zsh 主机，`r` 同样会转发 Ctrl+R；若需要相同的可上下选择列表，需在那台主机安装此脚本。未安装时使用远端原有的 Ctrl+R 行为，不能保证与本列表相同。

## 保存与安装

源码为 `shell/history.zsh`，使用 Zsh 自带 ZLE 和 `zsh/parameter`，不依赖 Oh My Zsh、Git、fzf 或新的原生库。使用 Zsh 官方的 [ZLE 自定义组件](https://zsh.sourceforge.io/Doc/Release/Zsh-Line-Editor.html)机制。

安装器 `shell/install-history.zsh` 接受插件文件路径，需要在 Term50 环境运行。它备份现有 `.zshrc`，在末尾增加一个带标记的加载段，保留其他配置；重复安装不会重复增加加载段。

- 插件：`$SANDBOX/data/shell/history.zsh`
- 历史：未指定 `HISTFILE` 时使用 `$SANDBOX/data/shell/history`
- 备份：`$SANDBOX/data/shell/backups/`
- 列表最多处理最近 5000 个不同命令，最多同时显示 8 行。
- 至少保存 5000 条历史，立即追加；以空格开头的命令不保存。
- 尊重原有 `HISTFILE`；只会展示实际已有的历史，无法恢复此前没有保存的旧命令。

本轮在手机上原先检测到 `HISTFILE` 未配置，因此从启用后开始持久保存。应用更新可保留私有数据，卸载应用会删除这些私有文件。

选择历史只修改 Zsh 编辑缓冲区，不调用 `eval`。列表把命令中的控制字符转为可见表示；接受后保留命令原文，包括多行内容。

## Mac 与其他 Zsh 主机

将本仓库 `shell/install-host-history.zsh` 和 `shell/history.zsh` 复制到目标主机的同一目录，在该目录运行 `zsh install-host-history.zsh history.zsh`。安装器保存 `.zshrc` 备份，然后为 SSH 交互会话加载脚本；重连 SSH 后生效。

- 脚本：`~/.local/share/term50-shell/history.zsh`
- 备份：`~/.local/share/term50-shell/backups/`
- 保留该主机现有 `HISTFILE`、历史大小和保存选项，不复制手机的历史文件。
- 已处理 zsh-autosuggestions 对列表显示区域的干扰。脚本应放在其他 Zsh 插件加载之后。
- `r` 仅添加到 Term50 的终端模式；Claude 模式保留原键位，tty 模式仍可正常输入字母 r。

统一快捷键与 SSH 的历史实测记录位于本机工作区的 `q-term/unified-history-validation/RESULTS.md`，初始本地功能记录位于 `q-term/history-validation/RESULTS.md`。两者都在 Term50 Git 仓库外，原始记录未随源码发布。
