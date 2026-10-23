#!/usr/bin/env python3
"""Private per-session Doubao adapter; stdin is 48k stereo s16le, stdout NDJSON."""
import argparse
import contextlib
import fcntl
import json
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parent


class BridgeError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__(code)


def emit(event, **fields):
    print(json.dumps(dict(event=event, **fields), ensure_ascii=False), flush=True)


def stable_code(value, fallback):
    if (isinstance(value, str) and 0 < len(value) <= 128
            and all(character in "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789" for character in value)):
        return value
    return fallback


class Receiver:
    def __init__(self, directory, binary, *, helper=None, previous_microphone=None,
                 recovery_report=None, lock_fds=()):
        self.path = pathlib.Path(directory)
        self.session = uuid.uuid4().hex
        self.events_path = self.path / "events.jsonl"
        self.events_path.touch(mode=0o600)
        self.events_file = self.events_path.open("rb")
        self.pending = b""
        self.next_lease = 0
        self.text = ""
        self.marked = False
        self.changed = time.monotonic()
        self.ready = False
        self.last_preview = ""
        self.last_revision = 0
        self.heartbeat()
        # Inherited locks keep an orphan Receiver's recovery exclusive even if
        # this Python process is killed before its finally block can run.
        self.process = subprocess.Popen(
            [str(binary), str(self.path), self.session,
             str(helper) if helper else "", previous_microphone or "",
             str(recovery_report) if recovery_report else ""],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, pass_fds=tuple(lock_fds))

    def heartbeat(self):
        if time.monotonic() < self.next_lease:
            return
        temporary = self.path / "lease.tmp"
        temporary.write_text(uuid.uuid4().hex)
        temporary.replace(self.path / "lease")
        self.next_lease = time.monotonic() + 0.25

    def command(self, action):
        ident = uuid.uuid4().hex
        temporary = self.path / "control.tmp"
        temporary.write_text(json.dumps(dict(id=ident, action=action, session=self.session)))
        temporary.replace(self.path / "control.json")
        return ident

    def preview(self):
        if self.ready and self.text != self.last_preview:
            emit("partial", text=self.text, marked=self.marked)
            self.last_preview = self.text

    def poll(self, *, tolerate_errors=False):
        self.heartbeat()
        self.pending += self.events_file.read(65536)
        if len(self.pending) > 131072:
            raise BridgeError("RECEIVER_OVERFLOW")
        result = []
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            try:
                item = json.loads(line)
            except ValueError:
                raise BridgeError("RECEIVER_PROTOCOL") from None
            if not isinstance(item, dict):
                raise BridgeError("RECEIVER_PROTOCOL")
            if item.get("session") != self.session:
                continue
            if item.get("event") == "error" and not tolerate_errors:
                raise BridgeError(stable_code(item.get("code"), "RECEIVER_ERROR"))
            if item.get("event") == "restore_failed":
                item["code"] = stable_code(item.get("code"), "RECEIVER_CLEANUP_FAILED")
            if item.get("event") == "text":
                text = item.get("text")
                marked = item.get("marked")
                revision = item.get("revision")
                if (not isinstance(text, str) or type(marked) is not bool
                        or type(revision) is not int or not self.last_revision < revision <= 0xffffffff):
                    raise BridgeError("RECEIVER_PROTOCOL")
                try:
                    size = len(text.encode("utf-8"))
                except UnicodeError:
                    raise BridgeError("RECEIVER_PROTOCOL") from None
                if size > 16384:
                    raise BridgeError("TEXT_LIMIT")
                self.last_revision = revision
                if (text, marked) != (self.text, self.marked):
                    self.changed = time.monotonic()
                self.text, self.marked = text, marked
                self.preview()
            result.append(item)
        return result

    def wait(self, event, ident=None, timeout=10, *, tolerate_errors=False):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for item in self.poll(tolerate_errors=tolerate_errors):
                if item.get("event") == "restore_failed":
                    raise BridgeError(item.get("code", "RECEIVER_CLEANUP_FAILED"))
                if item.get("event") == event and (ident is None or item.get("id") == ident):
                    return item
            if self.process.poll() is not None:
                raise BridgeError("RECEIVER_EXIT")
            time.sleep(0.025)
        raise BridgeError("RECEIVER_TIMEOUT")

    def wait_final(self, *, settle, timeout):
        stopped_at = time.monotonic()
        deadline = stopped_at + timeout
        while time.monotonic() < deadline:
            self.poll()
            if self.process.poll() is not None:
                raise BridgeError("RECEIVER_EXIT")
            # poll() can consume several revisions, including A -> B -> A.
            # Compare the time of every observed edit, not only the last text.
            if self.text and not self.marked and time.monotonic() - max(stopped_at, self.changed) >= settle:
                return self.text
            time.sleep(0.05)
        raise BridgeError("NO_STABLE_TEXT" if self.text else "NO_SPEECH")

    def close(self, success):
        error = None
        # The peer may already be gone. Cleanup must not depend on stdout.
        self.ready = False
        # Receiver restores input source and foreground app before acknowledging.
        if self.process.poll() is None:
            try:
                ident = self.command("restore" if success else "cancel")
                self.wait("restored", ident, timeout=3, tolerate_errors=True)
            except BridgeError as exc:
                error = exc
        else:
            # Process exit, including exit(0), is not proof that input source and
            # focus were restored. A final requires this session's restore ack.
            error = BridgeError("RECEIVER_CLEANUP_FAILED")
        self.events_file.close()
        try:
            if self.process.wait(timeout=5) != 0:
                error = error or BridgeError("RECEIVER_CLEANUP_FAILED")
        except subprocess.TimeoutExpired:
            # Stop heartbeat first: independent receiver watchdog performs cleanup.
            try:
                if self.process.wait(timeout=5) != 0:
                    error = error or BridgeError("RECEIVER_CLEANUP_FAILED")
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill(); self.process.wait()
                error = BridgeError("RECEIVER_CLEANUP_FAILED")
        return error


def microphone(helper, target=None, *, keepalive=None):
    try:
        command = [sys.executable, str(ROOT / "route_process.py"), str(os.getpid()),
                   str(helper)] + ([target] if target else [])
        with subprocess.Popen(command, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as process:
            deadline = time.monotonic() + 9.5
            try:
                while True:
                    try:
                        stdout, _ = process.communicate(timeout=0.1)
                        break
                    except subprocess.TimeoutExpired:
                        if keepalive:
                            keepalive()
                        if time.monotonic() >= deadline:
                            raise BridgeError("MICROPHONE_ROUTE_TIMEOUT")
            except BaseException:
                process.terminate()
                try:
                    process.communicate(timeout=1)
                except subprocess.TimeoutExpired:
                    process.kill(); process.communicate()
                raise
        if process.returncode == 124:
            raise BridgeError("MICROPHONE_ROUTE_TIMEOUT")
        if process.returncode:
            try:
                response = json.loads(stdout)
                code = response.get("error") if isinstance(response, dict) else None
            except ValueError:
                code = None
            if isinstance(code, str) and code.startswith("MICROPHONE_ROUTE_") and stable_code(code, None):
                raise BridgeError(code)
            raise BridgeError("MICROPHONE_ROUTE_FAILED")
        response = json.loads(stdout)
        if not isinstance(response, dict):
            raise BridgeError("MICROPHONE_ROUTE_PROTOCOL")
        previous = response.get("previous")
        if not isinstance(previous, str) or not previous.strip():
            raise BridgeError("MICROPHONE_ROUTE_PROTOCOL")
        return previous
    except subprocess.TimeoutExpired:
        raise BridgeError("MICROPHONE_ROUTE_TIMEOUT") from None
    except ValueError:
        raise BridgeError("MICROPHONE_ROUTE_PROTOCOL") from None


def cancelled(signum, frame):
    raise BridgeError("CANCELLED")


def transcribe(args):
    import sounddevice as sd
    binary = ROOT / "build/Term50VoiceReceiver.app/Contents/MacOS/Term50VoiceReceiver"
    helper = ROOT / "build/mic-route"
    if not binary.is_file() or not helper.is_file():
        raise BridgeError("BUILD_REQUIRED")
    device = next((i for i, value in enumerate(sd.query_devices())
                   if value["name"] == args.device and value["max_output_channels"] >= 2), None)
    if device is None:
        raise BridgeError("VIRTUAL_MICROPHONE_MISSING")
    private = ROOT / "private"
    private.mkdir(mode=0o700, exist_ok=True)
    with contextlib.ExitStack() as stack:
        lock = stack.enter_context((private / "session.lock").open("a+b"))
        os.chmod(private / "session.lock", 0o600)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            lock_fds = [lock.fileno()]
            # Cooperate with the original bridge, without changing any of its files.
            legacy = args.bridge_dir / "runtime/session.lock"
            if legacy.exists():
                legacy_lock = stack.enter_context(legacy.open("rb"))
                fcntl.flock(legacy_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                lock_fds.append(legacy_lock.fileno())
        except BlockingIOError:
            raise BridgeError("BUSY") from None
        runtime = stack.enter_context(tempfile.TemporaryDirectory(prefix="term50-voice-"))
        receiver = None
        previous_mic = None
        success = False
        result = None
        cleanup_error = None
        try:
            with sd.RawOutputStream(device=device, samplerate=48000, channels=2,
                                    dtype="int16", blocksize=960) as output:
                # CoreAudio can take time to start on its first use. Prove the
                # stream can write before changing microphone/input state or
                # allowing the phone to fill the bounded audio pipe.
                output.write(bytes(3840))
                if not args.manual_microphone:
                    # Learn previous state before attempting a potentially failing mutation.
                    previous_mic = microphone(helper)
                receiver = Receiver(runtime, binary, helper=helper,
                                    previous_microphone=previous_mic,
                                    recovery_report=private / "last-recovery.json",
                                    lock_fds=lock_fds)
                receiver.wait("launched")
                if not args.manual_microphone:
                    # Install the independent recovery owner before changing routes.
                    microphone(helper, args.device, keepalive=receiver.poll)
                receiver.wait("ready", receiver.command("start"))
                receiver.ready = True
                emit("ready")
                receiver.preview()
                started = time.monotonic()
                pending = bytearray()
                total = 0
                underruns = 0
                source = args.pcm.open("rb") if args.pcm else sys.stdin.buffer
                if args.pcm:
                    stack.callback(source.close)
                while True:
                    receiver.poll()
                    if receiver.process.poll() is not None:
                        raise BridgeError("RECEIVER_EXIT")
                    if time.monotonic() - started > args.max_seconds:
                        raise BridgeError("AUDIO_LIMIT")
                    readable, _, _ = select.select([source], [], [], 0.01)
                    eof = False
                    if readable:
                        chunk = os.read(source.fileno(), 3840 - len(pending))
                        if not chunk:
                            eof = True
                        else:
                            pending.extend(chunk)
                    if len(pending) == 3840 or eof:
                        if len(pending) % 4:
                            raise BridgeError("BAD_AUDIO")
                        if pending:
                            total += len(pending)
                            if total > int(args.max_seconds * 192000):
                                raise BridgeError("AUDIO_LIMIT")
                            underruns += bool(output.write(bytes(pending)))
                            pending.clear()
                    elif not readable:
                        underruns += bool(output.write(bytes(3840)))
                    if eof:
                        break
                for _ in range(25):
                    receiver.poll()
                    underruns += bool(output.write(bytes(3840)))
            receiver.wait("stopping", receiver.command("stop"))
            text = receiver.wait_final(settle=args.settle, timeout=args.timeout)
            result = dict(text=text, completion="unmarked_text_stable",
                          audio_seconds=total / 192000, output_underruns=underruns)
            success = True
        finally:
            # A second termination request must not interrupt restoration halfway through.
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            if receiver:
                try:
                    cleanup_error = receiver.close(success)
                except Exception:
                    cleanup_error = BridgeError("RECEIVER_CLEANUP_FAILED")
            if previous_mic is not None:
                try:
                    microphone(helper, previous_mic)
                except Exception:
                    cleanup_error = BridgeError("MICROPHONE_RESTORE_FAILED")
            if cleanup_error:
                # Surface restore failure even if recognition itself succeeded.
                raise cleanup_error
        emit("final", **result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-dir", type=pathlib.Path, required=True)
    parser.add_argument("--pcm", type=pathlib.Path)
    parser.add_argument("--device", default="Doubao Stream Bridge")
    parser.add_argument("--max-seconds", type=float, default=120)
    parser.add_argument("--settle", type=float, default=4)
    parser.add_argument("--timeout", type=float, default=25)
    parser.add_argument("--manual-microphone", action="store_true")
    args = parser.parse_args()
    if not (0 < args.max_seconds <= 120 and 0 < args.settle <= args.timeout <= 40):
        parser.error("Invalid duration limits")
    signal.signal(signal.SIGINT, cancelled)
    signal.signal(signal.SIGTERM, cancelled)
    try:
        transcribe(args)
        return 0
    except BridgeError as exc:
        emit("error", code=exc.code)
    except Exception:
        emit("error", code="BRIDGE_INTERNAL_ERROR")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
