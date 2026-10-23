#!/usr/bin/env python3
"""Authenticated, encrypted Term50 -> macOS Doubao voice companion."""
import argparse
import asyncio
import contextlib
import json
import os
import pathlib
import signal
import ssl
import sys
import time

from protocol import CIPHER, MAX_AUDIO, MAX_SECONDS, VoiceError, receive, send, text_payload
from pairing import read_key

ROOT = pathlib.Path(__file__).resolve().parent


def load_key(path):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        pass
    else:
        with os.fdopen(fd, "w") as stream:
            stream.write(os.urandom(32).hex() + "\n")
    return read_key(path)


def tls_context(key):
    if not hasattr(ssl.SSLContext, "set_psk_server_callback"):
        raise RuntimeError("Python with TLS-PSK support is required (Python 3.13 or newer)")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers(CIPHER)
    context.options |= ssl.OP_NO_TICKET
    context.set_psk_server_callback(lambda identity: key if identity == "term50" else b"")
    return context


class Companion:
    def __init__(self, command, max_seconds=MAX_SECONDS, startup_timeout=30, final_timeout=40):
        self.command = command
        self.max_seconds = max_seconds
        self.startup_timeout = startup_timeout
        self.final_timeout = final_timeout
        self.active = False
        self.connections = set()

    async def stop_child(self, child):
        if child is None:
            return
        if child.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                child.send_signal(signal.SIGTERM)
        if child.stdin:
            child.stdin.close()
        async def discard_output():
            while await child.stdout.read(65536):
                pass
        drain = asyncio.create_task(discard_output())
        try:
            await asyncio.wait_for(child.wait(), 28)
        except TimeoutError:
            # Receiver also has an independent lease and exits after key release.
            with contextlib.suppress(ProcessLookupError):
                child.kill()
            await child.wait()
        finally:
            try:
                await asyncio.wait_for(drain, 2)
            except TimeoutError:
                drain.cancel()
                await asyncio.gather(drain, return_exceptions=True)

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        self.connections.add(task)
        child = None
        owned = False
        jobs = []
        try:
            if len(self.connections) > 8:
                raise VoiceError("SERVER_BUSY")
            kind, payload = await receive(reader, 10)
            if kind == b"P" and not payload:
                await send(writer, b"K", b"TERM50/1")
                return
            if kind != b"B" or payload:
                raise VoiceError("BAD_START")
            if self.active:
                raise VoiceError("BUSY")
            self.active = owned = True
            spawning = asyncio.create_task(asyncio.create_subprocess_exec(
                *self.command, stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
                limit=65536, start_new_session=True))
            spawn_cancelled = False
            # A shutdown during fork/exec still has to acquire and stop the child.
            while not spawning.done():
                try:
                    await asyncio.shield(spawning)
                except asyncio.CancelledError:
                    spawn_cancelled = True
            child = spawning.result()
            if spawn_cancelled:
                raise asyncio.CancelledError
            ready = False
            stopped = asyncio.Event()
            stopped_at = None
            revision = 0

            async def incoming():
                nonlocal stopped_at
                started = None
                audio_bytes = 0
                while True:
                    kind, payload = await receive(reader, 45 if stopped.is_set() else (15 if ready else 35))
                    if kind == b"C" and not payload:
                        raise VoiceError("CANCELLED")
                    if not ready:
                        raise VoiceError("NOT_READY")
                    if started is None:
                        started = time.monotonic()
                    if kind == b"S" and not payload and not stopped.is_set():
                        stopped_at = time.monotonic()
                        stopped.set()
                        child.stdin.close()
                        # Keep observing EOF/disconnect while awaiting final ASR.
                        continue
                    if stopped.is_set() or kind != b"A":
                        raise VoiceError("BAD_STATE")
                    if not payload or len(payload) > MAX_AUDIO or len(payload) % 4:
                        raise VoiceError("BAD_AUDIO")
                    audio_bytes += len(payload)
                    if audio_bytes > int(self.max_seconds * 192000) or time.monotonic() - started > self.max_seconds:
                        raise VoiceError("AUDIO_LIMIT")
                    child.stdin.write(payload)
                    try:
                        await asyncio.wait_for(child.stdin.drain(), 3)
                    except TimeoutError:
                        raise VoiceError("AUDIO_BACKPRESSURE") from None

            async def outgoing():
                nonlocal ready, revision
                ready_at = None
                startup_deadline = time.monotonic() + self.startup_timeout

                async def confirm_final_exit():
                    # A final event is only a claim from the adapter. Do not
                    # commit text to the phone until its output is complete and
                    # the process exits successfully after restoring desktop state.
                    remaining = stopped_at + self.final_timeout - time.monotonic()
                    if remaining <= 0:
                        raise VoiceError("FINAL_TIMEOUT")
                    try:
                        trailing = await asyncio.wait_for(child.stdout.readline(), remaining)
                    except TimeoutError:
                        raise VoiceError("FINAL_TIMEOUT") from None
                    if trailing:
                        raise VoiceError("BRIDGE_PROTOCOL")
                    remaining = stopped_at + self.final_timeout - time.monotonic()
                    if remaining <= 0:
                        raise VoiceError("FINAL_TIMEOUT")
                    try:
                        status = await asyncio.wait_for(child.wait(), remaining)
                    except TimeoutError:
                        raise VoiceError("FINAL_TIMEOUT") from None
                    if status != 0:
                        raise VoiceError("BRIDGE_EXIT")

                def deadline():
                    if not ready:
                        return startup_deadline, "BRIDGE_START_TIMEOUT"
                    if stopped.is_set():
                        return stopped_at + self.final_timeout, "FINAL_TIMEOUT"
                    return ready_at + self.max_seconds, "AUDIO_LIMIT"

                while True:
                    expires, code = deadline()
                    remaining = expires - time.monotonic()
                    if remaining <= 0:
                        raise VoiceError(code)
                    # Recheck S even when no text arrives. Deadlines also apply
                    # while partials arrive continuously, not just on read timeout.
                    timeout = min(1.0, remaining)
                    try:
                        raw = await asyncio.wait_for(child.stdout.readline(), timeout)
                    except TimeoutError:
                        continue
                    expires, code = deadline()
                    if time.monotonic() >= expires:
                        raise VoiceError(code)
                    if not raw:
                        raise VoiceError("BRIDGE_EXIT")
                    try:
                        event = json.loads(raw)
                        name = event["event"]
                    except (ValueError, KeyError, TypeError):
                        raise VoiceError("BRIDGE_PROTOCOL") from None
                    if name == "ready" and not ready:
                        ready = True
                        ready_at = time.monotonic()
                        await send(writer, b"R")
                    elif name in ("partial", "final") and ready:
                        if name == "final" and not stopped.is_set():
                            raise VoiceError("BRIDGE_PROTOCOL")
                        text = event.get("text")
                        if not isinstance(text, str):
                            raise VoiceError("BRIDGE_PROTOCOL")
                        revision += 1
                        payload = text_payload(revision, text)
                        if name == "final":
                            await confirm_final_exit()
                        await send(writer, b"F" if name == "final" else b"T", payload)
                        if name == "final":
                            return
                    elif name == "error":
                        code = event.get("code", "BRIDGE_ERROR")
                        if not isinstance(code, str) or not code or len(code) > 128 or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789" for c in code):
                            code = "BRIDGE_ERROR"
                        raise VoiceError(code)
                    else:
                        raise VoiceError("BRIDGE_PROTOCOL")

            jobs = [asyncio.create_task(incoming()), asyncio.create_task(outgoing())]
            done, _ = await asyncio.wait(jobs, return_when=asyncio.FIRST_COMPLETED)
            for finished in done:
                finished.result()
        except VoiceError as exc:
            with contextlib.suppress(Exception):
                await send(writer, b"E", exc.code.encode("ascii"))
        except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError):
            pass
        except asyncio.CancelledError:
            raise
        except Exception:
            with contextlib.suppress(Exception):
                await send(writer, b"E", b"INTERNAL_ERROR")
        finally:
            async def finalize():
                for job in jobs:
                    job.cancel()
                if jobs:
                    await asyncio.gather(*jobs, return_exceptions=True)
                try:
                    await self.stop_child(child)
                finally:
                    if owned:
                        self.active = False
                    writer.close()
                    with contextlib.suppress(Exception):
                        await asyncio.wait_for(writer.wait_closed(), 2)
                    self.connections.discard(task)
            # Shutdown can overlap connection-loss cleanup. Repeated cancellation
            # must never interrupt key release and microphone restoration.
            cleanup = asyncio.create_task(finalize())
            while not cleanup.done():
                try:
                    await asyncio.shield(cleanup)
                except asyncio.CancelledError:
                    pass
            cleanup.result()


async def run(args):
    key = load_key(args.key_file)
    context = tls_context(key)
    if args.mock:
        command = [sys.executable, str(ROOT / "tests/mock_bridge.py")]
    else:
        command = [str(args.bridge_dir / ".venv/bin/python"), str(ROOT / "transcribe.py"),
                   "--bridge-dir", str(args.bridge_dir)]
    companion = Companion(command)
    server = await asyncio.start_server(companion.handle, args.bind, args.port, ssl=context,
                                        ssl_handshake_timeout=5, ssl_shutdown_timeout=2,
                                        limit=65536, backlog=4)
    print("Term50 voice companion ready (TLS-PSK); pairing key stored privately at " + str(args.key_file), flush=True)
    if args.mock:
        print("MOCK BRIDGE: no microphone or speech recognition is used", flush=True)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    async with server:
        await stop.wait()
    for task in list(companion.connections):
        task.cancel()
    await asyncio.gather(*list(companion.connections), return_exceptions=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1", help="Use the Mac LAN/Tailscale address to allow the phone")
    parser.add_argument("--port", type=int, default=45850)
    parser.add_argument("--key-file", type=pathlib.Path, default=ROOT / "private/pairing.key")
    parser.add_argument("--bridge-dir", type=pathlib.Path,
                        default=pathlib.Path(os.environ["TERM50_DOUBAO_BRIDGE_DIR"])
                        if os.environ.get("TERM50_DOUBAO_BRIDGE_DIR") else None)
    parser.add_argument("--mock", action="store_true", help="Protocol tests only; never actual ASR")
    args = parser.parse_args()
    if not args.mock and args.bridge_dir is None:
        parser.error("请用 --bridge-dir 或 TERM50_DOUBAO_BRIDGE_DIR 指定原桥接目录。")
    try:
        asyncio.run(run(args))
    except (OSError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
