# Doubao Stream Bridge（独立工具）

这是此前为 Term50 语音工作流编写的独立 macOS 工具：将公开测试音频按实时速度送入 `Doubao Stream Bridge` 虚拟麦克风，触发已登录的豆包输入法，并把接收窗口中的最终文字输出为 JSON。它不调用豆包官方 ASR API，也不读取豆包账号凭证。

此目录只收录源码和依赖清单。Python 虚拟环境、编译结果、Loopback 配置、运行时转写文字、本机备份及验证音频均不在仓库中。Term50 手机链路使用上一级 [`companion/`](../README.md) 中改写的 Receiver、麦克风路由和转写器；两者共用这里的 Python 虚拟环境，并通过 `runtime/session.lock` 避免同时操作同一个豆包输入法。

## 准备 Mac

需要已登录且未锁屏的 macOS 图形桌面、已安装并登录的豆包输入法、Xcode Command Line Tools，以及一个名为 `Doubao Stream Bridge` 的 Loopback 虚拟音频设备。该设备应是 **48 kHz、双声道、16-bit** 的 Pass-Thru 路由，不混入实体麦克风或扬声器监听。创建虚拟设备需要用户自行安装和配置 Loopback；仓库不提供该应用或本机设备配置。

在本目录执行以下命令。`python3` 必须是支持 TLS-PSK 的 Python 3.13 或更新版本；Companion 的服务端会使用同一虚拟环境：

```sh
python3 -c 'import ssl, sys; assert sys.version_info >= (3, 13) and hasattr(ssl.SSLContext, "set_psk_server_callback")'
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
zsh build.sh
```

需要从常见音频格式转为原始 PCM 时还需 `ffmpeg`。请先确认豆包界面能够手动选中 `Doubao Stream Bridge`，并给实际启动接收窗口和路由工具的进程授予必要的 macOS 辅助功能权限。

## 独立测试

仅使用可公开的测试音频。`transcribe.sh` 借助 `ffmpeg` 转换输入，并按实际速度发送；结果中可能包含识别文字，因此建议写入被 Git 忽略的 `runtime/`：

```sh
zsh transcribe.sh /path/to/public-audio.wav --output runtime/result.json
```

也可将 **48 kHz、双声道、16-bit little-endian、无文件头**的 PCM 送入 `.venv/bin/python transcribe.py --pcm /path/to/public-audio.pcm`，或写入它的标准输入。一次只运行一场识别。完成后可用 `zsh stop.sh` 退出独立接收窗口；中断正在转写的 Python 进程会尝试恢复原麦克风和输入法。

本工具的最终结果以非空、无组字标记的文字稳定 4 秒为界；这是界面层判断，不是豆包服务端的正式完成事件。它曾在 2026-09-21 的本机测试中验证实时 PCM 输入、最终文字和设备恢复；原始证据含音频及转写内容，未随仓库发布。当前 Doubao 版本、权限及音频路由在另一台 Mac 上须重新检查。

为 Term50 Companion 提供环境时，从仓库根目录设置 `TERM50_DOUBAO_BRIDGE_DIR` 为此目录的绝对路径，再遵循 [Companion 指南](../README.md) 构建和运行上一级服务。不要同时运行独立转写器和 Term50 语音会话。
