#!/usr/bin/env python3
"""Manage the Mac login-session service without exposing pairing material."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time

from pairing import read_key

ROOT = Path(__file__).resolve().parent
LABEL = "io.github.term50.voice-companion"
PLIST = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")


class ServiceError(RuntimeError):
    pass


def launchctl(*args, required=True):
    result = subprocess.run(["/bin/launchctl", *args], capture_output=True,
                            text=True, timeout=10 if args[0] == "print" else 55)
    if required and result.returncode:
        raise ServiceError("无法管理登录会话服务；请查看 companion/private/service.stderr.log。")
    return result


def target():
    return f"gui/{os.getuid()}/{LABEL}"


def job():
    result = launchctl("print", target(), required=False)
    if result.returncode:
        return dict(loaded=False, pid=None)
    match = re.search(r"^\s*pid = (\d+)\s*$", result.stdout, re.M)
    return dict(loaded=True, pid=int(match[1]) if match else None)


def config(bind, port, bridge):
    address = ipaddress.ip_address(bind)
    if address.is_unspecified or address.is_multicast:
        raise ValueError("请指定 Mac 的单个局域网地址，不使用所有接口或组播地址。")
    if not 1 <= port <= 65535:
        raise ValueError("端口必须为 1–65535。")
    bridge = bridge.absolute()
    python = bridge / ".venv/bin/python"
    if not python.is_file() or not os.access(python, os.X_OK):
        raise ServiceError("未找到原桥接的 Python 环境。")
    read_key(ROOT / "private/pairing.key")
    return {
        "Label": LABEL,
        "ProgramArguments": [str(python), str(ROOT / "server.py"),
                             "--bridge-dir", str(bridge), "--bind", str(address),
                             "--port", str(port), "--key-file", str(ROOT / "private/pairing.key")],
        "WorkingDirectory": str(ROOT),
        "EnvironmentVariables": {"PYTHONUNBUFFERED": "1"},
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 5,
        # SIGTERM may spend 28 s awaiting adapter recovery, plus I/O cleanup.
        "ExitTimeOut": 45,
        "ProcessType": "Interactive",
        "Umask": 0o077,
        "StandardOutPath": str(ROOT / "private/service.stdout.log"),
        "StandardErrorPath": str(ROOT / "private/service.stderr.log"),
    }


def read_config():
    if PLIST.is_symlink():
        raise ServiceError("服务配置不能是符号链接。")
    with PLIST.open("rb") as stream:
        cfg = plistlib.load(stream)
    if cfg.get("Label") != LABEL or cfg.get("ProgramArguments", [None, None])[1:2] != [str(ROOT / "server.py")]:
        raise ServiceError("已有服务配置不属于当前 Companion，请先检查配置。")
    return cfg


def health(cfg, timeout=10):
    args = cfg["ProgramArguments"]
    host = args[args.index("--bind") + 1]
    port = args[args.index("--port") + 1]
    key_file = args[args.index("--key-file") + 1]
    result = subprocess.run([args[0], str(ROOT / "client.py"), "--host", host,
                             "--port", port, "--key-file", key_file],
                            capture_output=True, timeout=timeout)
    return result.returncode == 0


def wait_ready(cfg, old_pid=None):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        state = job()
        remaining = deadline - time.monotonic()
        if state["pid"] and state["pid"] != old_pid and remaining > 0:
            try:
                if health(cfg, timeout=min(10, remaining)):
                    return dict(**state, authenticated_health=True)
            except subprocess.TimeoutExpired:
                pass
        time.sleep(0.5)
    raise ServiceError("服务尚未通过认证连接检查，请查看 companion/private/service.stderr.log。")


def write_plist(data):
    temporary = PLIST.with_suffix(".plist.tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(data)
    temporary.replace(PLIST)


def install(args):
    cfg = config(args.bind, args.port, args.bridge_dir)
    # Fail before changing an existing service if this runtime lacks prerequisites.
    checked = subprocess.run([cfg["ProgramArguments"][0], str(ROOT / "doctor.py"),
                              "--bridge-dir", str(args.bridge_dir), "--json"],
                             capture_output=True, timeout=12)
    if checked.returncode:
        raise ServiceError("本地依赖检查失败；请先运行 companion/run.sh --check。")
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    old_data = None
    if PLIST.exists() or PLIST.is_symlink():
        read_config()
        old_data = PLIST.read_bytes()
    for name in ("service.stdout.log", "service.stderr.log"):
        path = ROOT / "private" / name
        fd = os.open(path, os.O_CREAT | os.O_APPEND | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
        try:
            os.fchmod(fd, 0o600)
        finally:
            os.close(fd)
    previously_loaded = job()["loaded"]
    launchctl("bootout", target(), required=False)
    if job()["loaded"]:
        raise ServiceError("旧服务未能停止，没有修改配置。")
    try:
        write_plist(plistlib.dumps(cfg))
        launchctl("bootstrap", f"gui/{os.getuid()}", str(PLIST))
    except (OSError, RuntimeError, subprocess.TimeoutExpired):
        # Do not lose a working configuration when launchd rejects an update.
        if old_data is not None:
            write_plist(old_data)
            if previously_loaded:
                launchctl("bootstrap", f"gui/{os.getuid()}", str(PLIST), required=False)
        else:
            PLIST.unlink(missing_ok=True)
        raise
    return dict(action="installed", login_start=True, auto_restart=True,
                **wait_ready(cfg))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    start = commands.add_parser("install", help="安装登录时启动、退出后重启的用户服务")
    start.add_argument("--bind", required=True)
    start.add_argument("--port", type=int, default=45850)
    start.add_argument("--bridge-dir", type=Path, required=True)
    commands.add_parser("status", help="检查进程和加密配对连接；不打开麦克风")
    commands.add_parser("restart", help="先等待会话恢复，再由系统重新启动")
    commands.add_parser("uninstall", help="停止并移除本用户服务，保留配对和日志")
    args = parser.parse_args()
    try:
        if sys.platform != "darwin":
            raise ServiceError("此命令仅用于 macOS 登录会话。")
        if args.action == "install":
            report = install(args)
        elif args.action == "uninstall":
            if PLIST.exists() or PLIST.is_symlink():
                read_config()
            launchctl("bootout", target(), required=False)
            if job()["loaded"]:
                raise ServiceError("服务未能停止，没有移除配置。")
            PLIST.unlink(missing_ok=True)
            report = dict(action="uninstalled", loaded=False)
        else:
            if not PLIST.exists() and not PLIST.is_symlink() and args.action == "status":
                print(json.dumps(dict(action="status", loaded=False, pid=None,
                                      authenticated_health=False, installed=False)))
                return 1
            cfg = read_config()
            if args.action == "restart":
                before = job()
                if not before["loaded"]:
                    launchctl("bootstrap", f"gui/{os.getuid()}", str(PLIST))
                elif before["pid"]:
                    launchctl("kill", "SIGTERM", target())
                else:
                    launchctl("kickstart", target())
                report = dict(action="restarted", **wait_ready(cfg, before["pid"]))
            else:
                state = job()
                report = dict(action="status", **state,
                              authenticated_health=bool(state["pid"] and health(cfg)))
        print(json.dumps(report, ensure_ascii=False))
        return 0 if report.get("authenticated_health", args.action == "uninstall") else 1
    except ServiceError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        # Errors from subprocesses/invalid private files must not echo their data.
        print("服务操作未完成；请运行 companion/run.sh --check 并检查私有服务日志。", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
