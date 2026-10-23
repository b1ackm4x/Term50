# Term50 豆包语音 Companion

手机录音经 TLS-PSK 发送到 Mac，Mac 将 PCM 实时送入已有的 `Doubao Stream Bridge` 虚拟麦克风，并使用豆包输入法识别。独立桥接工具的[源码](doubao-stream-bridge/README.md)也已收录在本仓库；Term50 使用本目录自己的接收与路由实现，不修改独立工具的运行状态或读取豆包账号凭证。

## 开发机准备

在 macOS 已登录、未锁屏的图形桌面上运行。需要 Xcode Command Line Tools（提供 `swiftc` 和 `codesign`）、已登录的 Mac 豆包输入法，以及先按[独立桥接指南](doubao-stream-bridge/README.md)创建的 `Doubao Stream Bridge` 虚拟音频设备和 Python 虚拟环境。该环境的 `.venv/bin/python` 需要支持 TLS-PSK（Python 3.13 或更新版本，且 OpenSSL 提供 `PSK-AES256-GCM-SHA384`）并能导入 `sounddevice`/PortAudio；虚拟设备须支持 48 kHz、双声道、16-bit 输出。`build.sh` 在本目录构建接收窗口和麦克风切换工具，并临时签名接收窗口 App。Mac 的辅助功能授权应授予实际启动这些组件的进程；后台服务可能需要单独授权。

以下命令从 Term50 仓库根目录开始。先按上面的指南创建 `companion/doubao-stream-bridge/.venv`，再把示例地址改为 **Mac 可达的 IP 地址**；不要把个人目录、设备地址或配对密钥写进仓库。手机地址字段只接受 IPv4/IPv6 字面地址。`run.sh` 使用 `TERM50_DOUBAO_BRIDGE_DIR` 寻找桥接环境；直接运行 `server.py` 时也可传 `--bridge-dir`。

```sh
export TERM50_DOUBAO_BRIDGE_DIR="$PWD/companion/doubao-stream-bridge"
cd companion
export TERM50_MAC_IP='<Mac 当前可达的 IP 地址>'
zsh build.sh
zsh run.sh --check
zsh run.sh --bind "$TERM50_MAC_IP"
```

`--check` 只读检查本地依赖、TLS 能力、已构建组件、虚拟设备格式和已有配对文件的格式/权限；缺少配对文件只是提示，因为它在**首次真正启动**时才生成。检查不创建密钥、不打开音频流，也不切换输入法或麦克风。可加 `--json` 取结构化报告。前台 `run.sh` 启动后在 `private/pairing.key` 生成权限为 `0600` 的 64 位十六进制密钥。只在本机受控地查看该文件，把密钥手动填入手机 Term50 浮动按钮的「语音输入」→「Mac 连接」，同时填写 Mac IP 和端口 `45850`，启用并保存。不要把密钥放入命令行参数、聊天、截图或提交记录；`private/` 已被 Git 忽略，服务不会打印密钥。TLS 1.2 仅允许 `PSK-AES256-GCM-SHA384`，身份为 `term50`，不回退明文。

手机点「测试连接」或使用下文不带 `--pcm` 的 `client.py`，只能验证加密配对和协议连通，不启动豆包识别。首次可保持前台服务运行完成测试，再按 Ctrl-C 停止；如要实际听写，Mac 必须已解锁、豆包已登录且输入法与虚拟麦克风可用。识别会占用 Mac 接收窗口，切换到其他应用会取消识别。

### Wi-Fi 连接与历史验收

默认端口为 `45850`，未指定 `--bind` 时只监听 `127.0.0.1`。Mac 与手机须网络互通；可使用当前局域网或 Tailscale 地址。BlackBerry USB 网络也可用，但服务须绑定 Mac 一侧当前的 USB 地址，手机填写同一地址。地址可能变化，不应假定接口或地址固定。历史设备验证与时间结果见[语音开发验收记录](../docs/voice-development-validation.md)。

### 可选：登录后自动启动

**先完成一次前台启动并生成 `private/pairing.key`。**`service.py install` 要求该文件已存在；`run.sh --check` 通过不代表它已生成。停止前台服务后，在同一目录安装 macOS 用户登录服务。`service.py` 不读取 `TERM50_DOUBAO_BRIDGE_DIR`，安装时必须显式传入 `--bridge-dir`：

```sh
"$TERM50_DOUBAO_BRIDGE_DIR/.venv/bin/python" service.py install --bind "$TERM50_MAC_IP" --bridge-dir "$TERM50_DOUBAO_BRIDGE_DIR"
"$TERM50_DOUBAO_BRIDGE_DIR/.venv/bin/python" service.py status
```

服务配置位于本用户 `~/Library/LaunchAgents/io.github.term50.voice-companion.plist`，登录后启动，退出后由系统自动重启，保留原私有配对密钥。`restart` 发送 SIGTERM，等待会话恢复后再启动；`uninstall` 停止并取消登录启动，保留配对密钥和日志。不要同时用 `run.sh` 启动第二个服务。日志为 `private/service.stdout.log` 和 `private/service.stderr.log`，权限 `0600`；不包含识别文字或音频。

Mac 的 IP 变化时，用新地址重新运行 `install --bind ... --bridge-dir ...`，并修改手机「Mac 连接」中的地址；手机自身地址变化无需重新配对。Mac 登录后网络尚未就绪时，系统会间隔重试启动。后台服务可能需要在「隐私与安全性 → 辅助功能」中另行授权实际启动的程序，以操作豆包麦克风选择界面。`MICROPHONE_ROUTE_ACCESSIBILITY` 表示此权限未授权，与连接失败不同。`status` 只检查进程和加密配对连接，不表示桌面权限或真实识别已经通过；授权后应通过后台服务发送公开 PCM 验证识别和恢复。

## 文本行为

- `partial` 是接收窗口真正观察到的完整文本快照，可能增长、缩短或整句改写；不把最终文本拆字伪装实时输出。
- 输入法在停止录音后可能进行文字整理，`final` 用完整文本替换预览。接收窗口确认原输入法与前台应用恢复、麦克风恢复完成，适配器输出结束且正常退出后，服务端才发送 `F`；额外事件或恢复失败均返回错误。
- 最终判定仍为停止后文本非空、无组字标记、连续 4 秒稳定；这不是豆包服务端正式结束事件。
- Term50 保留独立听写预览，确认插入才发送给终端；不会自动提交执行。

按日期区分的 Mac 与 Q20 验收范围、测量结果和未验证边界见[语音开发验收记录](../docs/voice-development-validation.md)。

## 生命周期与限制

一次只接受一场识别。录音最多 120 秒，音频为 48000 Hz 双声道 s16le。就绪后才开始录音，网络/管道背压有超时，断线不会重放音频或恢复旧会话。取消、断线、焦点丢失均取消识别。SIGINT/SIGTERM 会等待麦克风和输入法恢复。

音频输出流在读取/修改麦克风与启动 Receiver 前打开，并先完成一次静音写入；输出设备实际就绪且 Receiver 触发成功后才发送 R 帧。避免初次打开 CoreAudio 时手机已开始发送音频，导致尚无人消费的管道背压。启动音频失败时不会修改麦克风或按住语音键。

录音和整理超时使用固定截止时间，即使中间文字持续到达也不能延长；整理截止从收到停止指令起算。接收窗口提前退出、未确认恢复或非正常退出均返回错误，不发送成功的最终结果。原前台应用未能在限时内重新激活时返回 `FOREGROUND_RESTORE_FAILED`。文字在一次读取中经历 `A → B → A` 也会重新计算稳定窗口。

Receiver 在切换麦克风之前就取得恢复信息并继承会话锁。若 Python 适配器被强杀，独立 Receiver 的租约失效后先释放 Option，再限时恢复原麦克风和输入法，最后恢复前台应用；完成之前不允许另一会话抢占。路由辅助进程另有父进程监视，避免遗留的麦克风切换与恢复竞争。正常清理仍由 Python 恢复麦克风，Receiver 不同时操作路由。

Receiver 使用每次新建的私有临时目录保存事件，结束后清除，不保留音频或转写日志。租约恢复会在 `private/last-recovery.json` 留下权限为 `0600` 的状态记录，只含会话标识、时间和错误码，没有音频、识别文字、麦克风名称或密钥。`client.py --events` 是明确选择保存测试证据的入口。原桥接的配置和历史日志不会被复制。若整组进程同时被强杀，软件无法继续恢复系统状态，私有临时目录也可能残留。

麦克风切换会在总共 6.5 秒内等待设置页面、设备列表及选择结果就绪，不因设置窗口首次启动稍慢而立即失败；每个选择动作仅执行一次，成功需从关闭列表后的已选摘要确认。若设置进程仍在但窗口已关闭，会尝试后台重开一次；已有窗口或窗口状态不明时不重复重开，仍受同一截止时间限制。

若 Mac 禁止键盘事件或辅助功能访问，返回明确错误；需要在系统设置中授权实际启动应用。服务不自行修改系统权限。`MICROPHONE_RESTORE_FAILED` / `MICROPHONE_RESTORE_TIMEOUT` 表示需要检查豆包麦克风选择；`INPUT_SOURCE_RESTORE_FAILED` 表示输入法恢复失败；`FOREGROUND_RESTORE_FAILED` 表示之前的前台应用未能在限时内重新成为前台。恢复未成功不会发送 `restored` 或最终识别成功结果。

## 验证

```sh
"$TERM50_DOUBAO_BRIDGE_DIR/.venv/bin/python" -m unittest discover -s tests -v
"$TERM50_DOUBAO_BRIDGE_DIR/.venv/bin/python" client.py --host "$TERM50_MAC_IP" --key-file private/pairing.key
"$TERM50_DOUBAO_BRIDGE_DIR/.venv/bin/python" client.py --host "$TERM50_MAC_IP" --key-file private/pairing.key --pcm /path/to/public-test-48k-stereo-s16le.pcm
swiftc MicRouteController.swift tests/mic-route/main.swift -o /tmp/term50-mic-route-test
/tmp/term50-mic-route-test
swiftc Recovery.swift tests/recovery/main.swift -o /tmp/term50-voice-recovery-test
/tmp/term50-voice-recovery-test
```

上述命令沿用前文设置的变量，需在 `companion` 目录执行。健康检查只验证认证连接，不激活豆包。`--pcm` 要求原始 48 kHz、双声道、16-bit 小端 PCM 文件，不是带文件头的 WAV；它按真实采样速度发送公开测试音频，并报告每次快照的时间与音频是否已经发完。真实识别还要观察识别文本、麦克风和输入法恢复，不能以健康检查代替。`--show-text` 会显示文字，默认只显示长度和时序；若显式加 `--events private/<文件名>.jsonl`，事件文件会保存转写文字，请只用于可公开的测试音频并妥善保管。

如需复跑本机公开音频的 Mac 桥接测试，可在已设置 `TERM50_DOUBAO_BRIDGE_DIR` 后执行 `zsh verify-public-audio.command /path/to/public-test-48k-stereo-s16le.pcm <该文件的SHA-256>`。脚本先核对文件格式长度与摘要，再调用豆包；结果写入被 Git 忽略的 `private/public-audio-result.jsonl`。只使用可公开的测试音频，因为结果包含识别文字。此脚本直接从 Mac 发送 PCM，不覆盖手机到 Mac 的 TLS 传输和手机录音。

`server.py --mock` 仅用于协议测试，明确输出 MOCK 标识，不启动豆包，不进行语音识别。自动测试覆盖错误密钥、TCP 分片、中文完整快照与最终改写、取消、断线、独占、非法帧和音频、旧会话事件隔离、适配器强杀后的锁存活和路由子进程退出。Swift 恢复测试只调用本地假进程，验证成功、失败、启动错误、限时强制退出与私有诊断；不操作真实输入法或麦克风。

## 协议 v1

每帧为大端 `uint32 length`，随后 1 字节类型和负载，length 包含类型且为 1–65536。一次 TLS 连接仅一场听写。

| 方向 | 类型 | 负载 |
|---|---|---|
| 手机→Mac | P | 空；健康探测 |
| Mac→手机 | K | `TERM50/1` |
| 手机→Mac | B | 空；开始固定格式识别 |
| Mac→手机 | R | 空；接收窗口已就绪且触发录音快捷键，不代表云端识别确认 |
| 手机→Mac | A | PCM，非空，4 的倍数，最多 16384 字节 |
| 手机→Mac | S | 空；结束音频并等待整理 |
| 手机→Mac | C | 空；取消 |
| Mac→手机 | T | 大端 uint32 revision + 完整 UTF-8 预览 |
| Mac→手机 | F | 相同格式，最终文字；仅收到 S 后发送 |
| Mac→手机 | E | 不超过 128 字节的稳定 ASCII 错误码 |

T/F 的 revision 从 1 严格递增，最终文字相同也递增。文字最多 16384 字节，不允许 ESC/NUL 等终端控制字符或 C1 控制字符，保留换行和 TAB。无断线续传；错误后保留手机预览，由用户决定是否使用。
