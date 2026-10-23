#!/usr/bin/env python3
"""Read-only local prerequisites; never opens an audio stream or activates Doubao."""
import argparse
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys

from pairing import read_key

ROOT = Path(__file__).resolve().parent


def inspect_key(path):
    """Do not create a key, follow a symlink, or include its contents in errors."""
    try:
        read_key(path)
    except FileNotFoundError:
        return "info", "首次启动时会创建私有配对密钥。"
    except RuntimeError:
        return "error", "配对密钥必须是权限为 0600 的普通文件，内容为 64 位十六进制字符。"
    except OSError:
        return "error", "无法读取配对密钥文件。"
    return "ok", "私有配对密钥格式和权限正常。"


def probe_audio(python):
    # Keep native-library loading/device enumeration bounded. This child only
    # queries capabilities: no recording, playback, stream, or default-device edit.
    program = r'''
import json
try:
    import sounddevice as sd
except Exception:
    print(json.dumps({"code": "AUDIO_LIBRARY_MISSING"}))
else:
    try:
        devices = sd.query_devices()
        device = next((i for i, d in enumerate(devices)
                       if d["name"] == "Doubao Stream Bridge"
                       and d["max_output_channels"] >= 2), None)
        if device is None:
            result = {"code": "VIRTUAL_DEVICE_MISSING"}
        else:
            sd.check_output_settings(device=device, channels=2,
                                     dtype="int16", samplerate=48000)
            result = {"code": "OK"}
        print(json.dumps(result))
    except Exception:
        print(json.dumps({"code": "AUDIO_FORMAT_UNAVAILABLE"}))
'''
    try:
        result = subprocess.run([str(python), "-c", program], capture_output=True,
                                text=True, timeout=6, check=False)
        if result.returncode:
            return "error", "音频能力检查进程未能完成。"
        code = json.loads(result.stdout).get("code")
    except subprocess.TimeoutExpired:
        return "error", "音频设备查询超时；没有打开录音或播放。"
    except (OSError, ValueError, AttributeError):
        return "error", "无法读取音频能力检查结果。"
    if not isinstance(code, str):
        return "error", "音频能力检查返回未知状态。"
    messages = {
        "OK": ("ok", "虚拟麦克风支持 48 kHz 双声道 16-bit 音频输出。"),
        "AUDIO_LIBRARY_MISSING": ("error", "原桥接 Python 环境缺少可用的 sounddevice/PortAudio。"),
        "VIRTUAL_DEVICE_MISSING": ("error", "未找到双声道 Doubao Stream Bridge 虚拟音频设备。"),
        "AUDIO_FORMAT_UNAVAILABLE": ("error", "虚拟音频设备不支持所需格式或设备查询失败。"),
    }
    return messages.get(code, ("error", "音频能力检查返回未知状态。"))


def inspect(bridge_dir, key_file, *, root=ROOT, platform=sys.platform):
    checks = []

    def add(name, status, message):
        checks.append(dict(name=name, status=status, message=message))

    if platform != "darwin":
        add("platform", "error", "真实豆包桥接需要 macOS 图形桌面。")
    else:
        add("platform", "ok", "正在 macOS 上检查本地环境。")

    try:
        if not hasattr(ssl.SSLContext, "set_psk_server_callback"):
            raise RuntimeError
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers("PSK-AES256-GCM-SHA384")
        add("tls", "ok", "Python 支持所需的加密配对协议。")
    except (RuntimeError, ssl.SSLError):
        add("tls", "error", "需要支持 TLS-PSK 的 Python 3.13 或更新版本。")

    python = bridge_dir / ".venv/bin/python"
    if python.is_file() and os.access(python, os.X_OK):
        add("bridge_runtime", "ok", "原桥接 Python 环境可执行。")
        if platform == "darwin":
            add("virtual_audio", *probe_audio(python))
    else:
        add("bridge_runtime", "error", "未找到原桥接的 .venv/bin/python；请检查 TERM50_DOUBAO_BRIDGE_DIR。")

    for name, relative in (
        ("receiver", "build/Term50VoiceReceiver.app/Contents/MacOS/Term50VoiceReceiver"),
        ("microphone_helper", "build/mic-route"),
    ):
        path = root / relative
        if path.is_file() and os.access(path, os.X_OK):
            add(name, "ok", "接收窗口已构建。" if name == "receiver" else "麦克风切换工具已构建。")
        else:
            add(name, "error", "缺少可执行的组件，请先运行 companion/build.sh。")
    add("pairing_key", *inspect_key(key_file))
    add("recognition_boundary", "info", "本检查不验证豆包登录、桌面权限、网络识别或手机连接；这些在实际会话中验证。")
    return dict(ok=not any(item["status"] == "error" for item in checks),
                scope="local_prerequisites", checks=checks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-dir", type=Path,
                        default=Path(os.environ["TERM50_DOUBAO_BRIDGE_DIR"])
                        if os.environ.get("TERM50_DOUBAO_BRIDGE_DIR") else None)
    parser.add_argument("--key-file", type=Path, default=ROOT / "private/pairing.key")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.bridge_dir is None:
        parser.error("请用 --bridge-dir 或 TERM50_DOUBAO_BRIDGE_DIR 指定原桥接目录。")
    report = inspect(args.bridge_dir, args.key_file)
    if args.json:
        print(json.dumps(report, ensure_ascii=False))
    else:
        labels = {"ok": "通过", "info": "说明", "error": "需处理"}
        for item in report["checks"]:
            print(f'[{labels[item["status"]]}] {item["message"]}')
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
