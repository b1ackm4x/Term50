#!/usr/bin/env python3
"""Synthetic protocol fixture. Does not run Doubao or recognize speech."""
import json
import os
import pathlib
import signal
import sys
import time


def emit(event, **fields):
    print(json.dumps(dict(event=event, **fields), ensure_ascii=False), flush=True)


def cancelled(signum, frame):
    if len(sys.argv) > 1:
        pathlib.Path(sys.argv[1]).write_text("cancelled\n")
    raise SystemExit(1)


signal.signal(signal.SIGTERM, cancelled)
mode = sys.argv[2] if len(sys.argv) > 2 else "normal"
if mode == "slow_start":
    time.sleep(10)
emit("ready")
if mode == "stream_recording":
    for _ in range(150):
        emit("partial", text="录音中的草稿", marked=True)
        time.sleep(0.002)
count = 0
while chunk := os.read(0, 3840):
    count += len(chunk)
    emit("partial", text="今天下午" if count <= 3840 else "今天下午三点", marked=True)
    time.sleep(0.005)
emit("partial", text="今天下午三点，检查服务。", marked=False)
if mode == "cleanup_error":
    emit("error", code="MICROPHONE_RESTORE_FAILED")
    raise SystemExit(1)
if mode == "slow_final":
    time.sleep(0.25)
if mode == "stream_final":
    for _ in range(150):
        emit("partial", text="仍在整理的草稿", marked=False)
        time.sleep(0.002)
emit("final", text="今天下午3点，检查服务。")
if mode == "bad_exit_after_final":
    raise SystemExit(2)
if mode == "trailing_event_after_final":
    emit("error", code="MICROPHONE_RESTORE_FAILED")
if mode == "hang_after_final":
    time.sleep(10)
