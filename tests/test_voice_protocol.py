#!/usr/bin/env python3
"""Native C voice client against real TLS-PSK, with synthetic audio only.

Run with Python 3.13+ and pkg-config OpenSSL: python3 tests/test_voice_protocol.py
The fixture never invokes Mac UI, Doubao, or a real microphone.
"""
import asyncio
import contextlib
import json
import os
from pathlib import Path
import shlex
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "companion"))
from protocol import receive, send, text_payload
from server import Companion, tls_context


class Native:
    def __init__(self, process):
        self.process = process
        self.events = []
        self.queue = asyncio.Queue()
        self.pump = asyncio.create_task(self.read())

    async def read(self):
        while raw := await self.process.stdout.readline():
            event = json.loads(raw)
            if "text_hex" in event:
                event["text"] = bytes.fromhex(event.pop("text_hex")).decode("utf-8")
            self.events.append(event)
            await self.queue.put(event)
        await self.queue.put(None)

    async def wait(self, predicate, timeout=4):
        async with asyncio.timeout(timeout):
            while True:
                item = await self.queue.get()
                if item is None:
                    raise AssertionError(f"Native client exited before expected event: {self.events}")
                if predicate(item):
                    return item

    async def state(self, state):
        return await self.wait(lambda event: event.get("state") == state)

    async def command(self, name):
        self.process.stdin.write((name + "\n").encode())
        await self.process.stdin.drain()

    async def close(self):
        if self.process.returncode is None:
            await self.command("QUIT")
        await asyncio.wait_for(self.process.wait(), 5)
        await self.pump
        stderr = (await self.process.stderr.read()).decode()
        if self.process.returncode:
            raise AssertionError(f"Native harness failed ({self.process.returncode}): {stderr}")
        ending = self.events[-1]
        assert ending.get("end"), self.events
        assert ending["opens"] == ending["closes"], ending
        assert ending["destroy_ms"] < 1500, ending


@unittest.skipUnless(hasattr(ssl.SSLContext, "set_psk_server_callback"), "Python TLS-PSK support required")
class NativeVoiceProtocol(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory(prefix="term50-voice-native-")
        cls.binary = Path(cls.build_dir.name) / "voice-client-test"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True))
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra",
                   "-Wno-deprecated-declarations", *shlex.split(os.environ.get("VOICE_TEST_SANITIZERS", "-fsanitize=address,undefined")), "-pthread", "-Isrc",
                   "tests/test_voice_client.c", "tests/voice_fake_audio.c", "src/voice_client.c", "src/voice_commit.c",
                   *flags, "-o", str(cls.binary)]
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
        result = subprocess.run([str(cls.binary)], check=True, capture_output=True, text=True)
        print(result.stdout.strip(), flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="term50-voice-wire-")
        self.key = os.urandom(32)
        self.key_file = Path(self.directory.name) / "fixture.key"
        self.key_file.write_text(self.key.hex()); self.key_file.chmod(0o600)
        self.clients = []
        self.handlers = set()
        self.failures = []
        self.companions = []
        self.behavior = None
        self.entered = asyncio.Event()
        self.disconnected = asyncio.Event()
        self.frames = []
        self.previous_error_handler = asyncio.get_running_loop().get_exception_handler()
        # Failed-key TLS handshakes are expected and never reach the handler.
        asyncio.get_running_loop().set_exception_handler(lambda *_: None)
        self.server = await asyncio.start_server(self.handle, "127.0.0.1", 0, ssl=tls_context(self.key),
                                                 ssl_handshake_timeout=1, ssl_shutdown_timeout=0.2)
        self.port = self.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        try:
            for native in self.clients:
                await native.close()
            self.server.close(); await self.server.wait_closed()
            for task in list(self.handlers):
                task.cancel()
            await asyncio.gather(*list(self.handlers), return_exceptions=True)
            for companion in self.companions:
                tasks = list(companion.connections)
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
            self.assertEqual(self.failures, [])
        finally:
            asyncio.get_running_loop().set_exception_handler(self.previous_error_handler)
            self.directory.cleanup()

    async def handle(self, reader, writer):
        task = asyncio.current_task(); self.handlers.add(task)
        try:
            await self.behavior(reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError):
            self.disconnected.set()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.failures.append(repr(error))
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
            self.handlers.discard(task)

    async def client(self, probe=False, wrong_key=False, fail_audio=False, delay_open=False, burst_audio=False, small_fragments=False, audio_sample=None):
        path = self.key_file
        if wrong_key:
            path = Path(self.directory.name) / "wrong.key"
            path.write_text(os.urandom(32).hex()); path.chmod(0o600)
        env = os.environ.copy()
        if fail_audio:
            env["TERM50_TEST_AUDIO_FAIL_OPEN"] = "1"
        if delay_open:
            env["TERM50_TEST_AUDIO_DELAY_OPEN"] = "1"
        if burst_audio:
            env["TERM50_TEST_AUDIO_BURST"] = "1"
        if small_fragments:
            env["TERM50_TEST_AUDIO_SMALL_FRAGMENTS"] = "1"
        if audio_sample is not None:
            env["TERM50_TEST_AUDIO_SAMPLE"] = str(audio_sample)
        process = await asyncio.create_subprocess_exec(str(self.binary), str(self.port), str(path), str(int(probe)),
                                                       stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                                                       stderr=asyncio.subprocess.PIPE, env=env)
        native = Native(process); self.clients.append(native)
        return native

    async def begin(self, reader):
        self.assertEqual(await receive(reader, 3), (b"B", b"")); self.entered.set()

    async def until_stop(self, reader):
        while True:
            kind, payload = await receive(reader, 3)
            self.frames.append((kind, len(payload)))
            if kind == b"S":
                self.assertEqual(payload, b""); return
            self.assertEqual(kind, b"A"); self.assertTrue(payload)
            self.assertEqual(len(payload) % 4, 0); self.assertEqual(set(payload), {0})

    async def fragmented(self, writer, kind, payload=b""):
        frame = struct.pack("!I", len(payload) + 1) + kind + payload
        # Split both frame headers and UTF-8 sequences across SSL records.
        cuts = sorted(set([1, 3, 4, 5, 8, 10, len(frame)]))
        start = 0
        for end in cuts:
            if end > len(frame):
                continue
            writer.write(frame[start:end]); await writer.drain(); start = end
            await asyncio.sleep(0.006)

    async def test_health_fragmented_reply_never_opens_audio(self):
        async def server(reader, writer):
            self.assertEqual(await receive(reader), (b"P", b""))
            await self.fragmented(writer, b"K", b"TERM50/1")
        self.behavior = server
        result = await (await self.client(probe=True)).state(4)
        self.assertEqual((result["opens"], result["bytes"], result["text"]), (0, 0, ""))

    async def test_wrong_key_rejected_before_capture(self):
        async def server(reader, writer):
            self.fail("wrong key reached the authenticated handler")
        self.behavior = server
        result = await (await self.client(wrong_key=True)).state(5)
        self.assertEqual((result["error"], result["opens"]), ("authentication_or_tls_failed", 0))

    async def test_audio_level_uses_actual_little_endian_pcm_without_transcript_updates(self):
        for sample, expected in ((0, 0), (1, 0), (1024, 31), (-16384, 500), (32767, 1000), (-32768, 1000)):
            with self.subTest(sample=sample):
                finalize = asyncio.Event()
                async def server(reader, writer):
                    await self.begin(reader); await send(writer, b"R")
                    kind, pcm = await receive(reader)
                    self.assertEqual(kind, b"A")
                    self.assertEqual(pcm, struct.pack("<hh", sample, 0) * (len(pcm) // 4))
                    await send(writer, b"T", text_payload(1, "音量测试"))
                    while True:
                        kind, pcm = await receive(reader)
                        if kind == b"S": break
                        self.assertEqual(kind, b"A")
                    await finalize.wait()
                    await send(writer, b"F", text_payload(2, "音量测试。"))
                self.behavior = server
                native = await self.client(audio_sample=sample)
                await native.wait(lambda e: e.get("revision") == 1)
                await native.command("SNAP")
                first = await native.wait(lambda e: e.get("requested"))
                self.assertEqual((first["state"], first["level"]), (2, expected))
                await asyncio.sleep(0.06); await native.command("SNAP")
                second = await native.wait(lambda e: e.get("requested"))
                self.assertGreater(second["bytes"], first["bytes"])
                self.assertEqual((second["level"], second["change"]), (expected, first["change"]))
                await native.command("STOP"); await native.command("SNAP")
                self.assertEqual((await native.wait(lambda e: e.get("requested")))["level"], 0)
                finalize.set()
                self.assertEqual((await native.state(4))["level"], 0)
                await native.close()

    async def test_audio_level_clears_synchronously_on_cancel(self):
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R")
            await receive(reader); await send(writer, b"T", text_payload(1, "取消音量测试"))
            while await reader.read(65536): pass
        self.behavior = server
        native = await self.client(audio_sample=-32768)
        result = await native.wait(lambda e: e.get("revision") == 1)
        self.assertEqual(result["level"], 1000)
        await native.command("CANCEL"); await native.command("SNAP")
        self.assertEqual((await native.wait(lambda e: e.get("requested")))["level"], 0)
        await native.command("SNAP")
        self.assertEqual((await native.wait(lambda e: e.get("requested")))["level"], 0)

    async def test_audio_level_clears_when_recording_connection_fails(self):
        disconnect = asyncio.Event()
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R")
            await receive(reader); await send(writer, b"T", text_payload(1, "断线音量测试"))
            await disconnect.wait()
        self.behavior = server
        native = await self.client(audio_sample=-32768)
        result = await native.wait(lambda e: e.get("revision") == 1)
        self.assertEqual(result["level"], 1000)
        disconnect.set()
        self.assertEqual((await native.state(5))["level"], 0)

    async def test_fragmented_snapshots_stop_and_final_rewrite(self):
        async def server(reader, writer):
            await self.begin(reader)
            with self.assertRaises(TimeoutError):
                await asyncio.wait_for(reader.read(1), 0.12)
            await self.fragmented(writer, b"R")
            kind, pcm = await receive(reader); self.assertEqual(kind, b"A"); self.assertEqual(len(pcm) % 4, 0)
            await self.fragmented(writer, b"T", text_payload(1, "今天下午三点"))
            await self.until_stop(reader)
            await self.fragmented(writer, b"T", text_payload(2, "今天下午三点，检查服务。"))
            await self.fragmented(writer, b"F", text_payload(3, "今天下午3点，检查服务。"))
            self.assertEqual(await reader.read(), b"")
        self.behavior = server
        native = await self.client(); await self.entered.wait(); await asyncio.sleep(0.06); await native.command("SNAP")
        before = await native.wait(lambda e: e.get("requested")); self.assertEqual((before["state"], before["opens"], before["bytes"]), (1, 0, 0))
        partial = await native.wait(lambda e: e.get("revision") == 1)
        self.assertEqual(partial["text"], "今天下午三点")
        await native.command("STOP")
        finalizing = await native.state(3); self.assertEqual(finalizing["opens"], finalizing["closes"])
        final = await native.state(4)
        self.assertEqual((final["revision"], final["text"]), (3, "今天下午3点，检查服务。"))
        self.assertEqual(self.frames[-1], (b"S", 0))

    async def test_real_companion_subprocess_contract(self):
        marker = Path(self.directory.name) / "child-cancelled"
        companion = Companion([sys.executable, str(ROOT / "companion/tests/mock_bridge.py"), str(marker)],
                              startup_timeout=2, final_timeout=2)
        self.companions.append(companion); self.behavior = companion.handle
        native = await self.client()
        await native.wait(lambda e: e.get("revision", 0) > 0)
        await native.command("STOP")
        final = await native.state(4)
        self.assertEqual(final["text"], "今天下午3点，检查服务。")
        self.assertGreater(final["revision"], 1)
        for _ in range(100):
            if not companion.active: break
            await asyncio.sleep(0.01)
        self.assertFalse(companion.active)

    async def test_converter_fragments_drain_without_readable_descriptor(self):
        async def server(reader, writer):
            await self.begin(reader)
            await send(writer, b"R")
            await self.until_stop(reader)
            await send(writer, b"F", text_payload(1, "缓冲区读取正常"))
        self.behavior = server
        native = await self.client(small_fragments=True)
        await native.state(2)
        await asyncio.sleep(0.8)
        await native.command("STOP")
        final = await native.state(4)
        self.assertEqual(final["text"], "缓冲区读取正常")
        audio = [size for kind, size in self.frames if kind == b"A"]
        self.assertGreater(len(audio), 100)
        self.assertEqual(set(audio), {960})

    async def test_invalid_utf8_controls_and_text_limit(self):
        for data in (b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe4\xb8", b"\x1b[2J", b"a\0b", b"\xc2\x85", b"x" * 16385):
            with self.subTest(payload_bytes=len(data), prefix=data[:3].hex()):
                async def server(reader, writer, payload=data):
                    await self.begin(reader); await send(writer, b"R")
                    await send(writer, b"T", struct.pack("!I", 1) + payload)
                    await reader.read()
                self.behavior = server
                result = await (await self.client()).state(5)
                self.assertEqual((result["error"], result["revision"], result["text"]), ("invalid_transcript", 0, ""))

    async def test_frame_lengths_and_unexpected_types(self):
        frames = (struct.pack("!I", 0), struct.pack("!I", 65537), struct.pack("!I", 1) + b"Z",
                  struct.pack("!I", 6) + b"F" + struct.pack("!I", 1) + b"x")
        for frame in frames:
            with self.subTest(frame=frame.hex()):
                async def server(reader, writer, raw=frame):
                    await self.begin(reader); writer.write(raw); await writer.drain(); await reader.read()
                self.behavior = server
                result = await (await self.client()).state(5)
                self.assertEqual((result["error"], result["opens"]), ("protocol_error", 0))

    async def test_nonincreasing_revisions_rejected_without_replacing_text(self):
        for revision in (0, 4, 5):
            with self.subTest(revision=revision):
                async def server(reader, writer, rev=revision):
                    await self.begin(reader); await send(writer, b"R")
                    await send(writer, b"T", text_payload(5, "保留草稿")); await asyncio.sleep(0.04)
                    await send(writer, b"T", struct.pack("!I", rev) + "错误覆盖".encode()); await reader.read()
                self.behavior = server
                result = await (await self.client()).state(5)
                self.assertEqual((result["error"], result["revision"], result["text"]), ("invalid_transcript", 5, "保留草稿"))

    async def test_final_before_stop_rejected(self):
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R"); await send(writer, b"F", text_payload(1, "提前结束")); await reader.read()
        self.behavior = server
        result = await (await self.client()).state(5)
        self.assertEqual(result["error"], "protocol_error"); self.assertEqual(result["text"], "")

    async def test_cancel_connecting_and_busy_start(self):
        async def server(reader, writer):
            await self.begin(reader); self.assertEqual(await reader.read(), b""); self.disconnected.set()
        self.behavior = server
        native = await self.client(); await asyncio.wait_for(self.entered.wait(), 3)
        await native.command("START"); self.assertEqual((await native.wait(lambda e: "start_result" in e))["start_result"], -1)
        await native.command("CANCEL"); result = await native.state(0)
        self.assertEqual((result["opens"], result["bytes"]), (0, 0)); await asyncio.wait_for(self.disconnected.wait(), 3)

    async def test_stop_before_ready_never_opens_audio(self):
        async def server(reader, writer):
            await self.begin(reader); self.assertEqual(await reader.read(), b"")
        self.behavior = server
        native = await self.client(); await asyncio.wait_for(self.entered.wait(), 3); await native.command("STOP")
        result = await native.state(5); self.assertEqual((result["error"], result["opens"]), ("cancelled", 0))

    async def test_cancel_recording_releases_audio(self):
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R")
            while await reader.read(65536): pass
            self.disconnected.set()
        self.behavior = server
        native = await self.client(); await native.state(2); await native.command("CANCEL")
        result = await native.state(0); self.assertEqual((result["opens"], result["closes"]), (1, 1))
        await asyncio.wait_for(self.disconnected.wait(), 3)

    async def test_cancel_while_finalizing_never_commits(self):
        stopped = asyncio.Event()
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R"); await self.until_stop(reader); stopped.set()
            self.assertEqual(await reader.read(), b""); self.disconnected.set()
        self.behavior = server
        native = await self.client(); await native.state(2); await native.command("STOP")
        await asyncio.wait_for(stopped.wait(), 3); await native.state(3); await native.command("CANCEL")
        result = await native.state(0); self.assertEqual((result["opens"], result["closes"], result["text"]), (1, 1, ""))
        self.assertNotIn(4, [event.get("state") for event in native.events]); await asyncio.wait_for(self.disconnected.wait(), 3)

    async def test_disconnect_during_recording_and_after_stop(self):
        for after_stop in (False, True):
            with self.subTest(after_stop=after_stop):
                async def server(reader, writer, stop=after_stop):
                    await self.begin(reader); await send(writer, b"R")
                    if stop: await self.until_stop(reader)
                    else: await receive(reader)
                self.behavior = server
                native = await self.client()
                if after_stop:
                    await native.state(2); await native.command("STOP")
                result = await native.state(5)
                self.assertEqual((result["error"], result["opens"], result["closes"]), ("connection_lost", 1, 1))

    async def test_server_error_and_microphone_open_failure(self):
        async def busy(reader, writer):
            await self.begin(reader); await send(writer, b"E", b"BUSY")
        self.behavior = busy
        result = await (await self.client()).state(5)
        self.assertEqual((result["error"], result["opens"]), ("BUSY", 0))
        async def ready(reader, writer):
            await self.begin(reader); await send(writer, b"R"); self.assertEqual(await reader.read(), b"")
        self.behavior = ready
        result = await (await self.client(fail_audio=True)).state(5)
        self.assertEqual((result["error"], result["opens"]), ("microphone_unavailable", 0))

    async def test_disconnect_before_ready_never_opens_audio(self):
        async def server(reader, writer):
            await self.begin(reader)
        self.behavior = server
        result = await (await self.client()).state(5)
        self.assertEqual((result["error"], result["opens"], result["bytes"]), ("connection_lost", 0, 0))

    async def test_exact_text_limit_is_preserved(self):
        message = "中" * 5461 + "x"  # Exactly 16384 UTF-8 bytes.
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R")
            await self.fragmented(writer, b"T", text_payload(1, message))
            await self.until_stop(reader); await self.fragmented(writer, b"F", text_payload(2, message))
        self.behavior = server
        native = await self.client(); partial = await native.wait(lambda e: e.get("revision") == 1)
        self.assertEqual(partial["text"], message); await native.command("STOP")
        final = await native.state(4); self.assertEqual(final["text"], message)

    async def test_explicit_restart_clears_old_revision_and_draft(self):
        connections = 0
        async def server(reader, writer):
            nonlocal connections
            connections += 1
            await self.begin(reader)
            if connections == 1:
                await send(writer, b"R"); await send(writer, b"T", text_payload(8, "旧会话草稿"))
                while await reader.read(65536): pass
            else:
                await asyncio.sleep(0.06)
                await send(writer, b"R"); await send(writer, b"T", text_payload(1, "新的草稿"))
                await self.until_stop(reader); await send(writer, b"F", text_payload(2, "新的结果。"))
        self.behavior = server
        native = await self.client(); await native.wait(lambda e: e.get("revision") == 8)
        await native.command("CANCEL"); await native.state(0); await asyncio.sleep(0.03)
        await native.command("START"); self.assertEqual((await native.wait(lambda e: "start_result" in e))["start_result"], 0)
        starting = await native.state(1); self.assertEqual((starting["revision"], starting["text"]), (0, ""))
        fresh = await native.wait(lambda e: e.get("revision") == 1); self.assertEqual(fresh["text"], "新的草稿")
        await native.command("STOP"); final = await native.state(4)
        self.assertEqual((final["revision"], final["text"], final["opens"], final["closes"]), (2, "新的结果。", 2, 2))

    async def test_final_snapshot_cannot_be_replaced_by_coalesced_trailing_frames(self):
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R"); await self.until_stop(reader)
            frames = []
            for kind, payload in ((b"F", text_payload(1, "最终内容。")),
                                  (b"T", text_payload(2, "不应覆盖")), (b"E", b"LATE_ERROR")):
                frames.append(struct.pack("!I", 1 + len(payload)) + kind + payload)
            writer.write(b"".join(frames)); await writer.drain(); await reader.read()
        self.behavior = server
        native = await self.client(); await native.state(2); await native.command("STOP")
        result = await native.state(4)
        await asyncio.sleep(0.05); await native.command("SNAP")
        settled = await native.wait(lambda e: e.get("requested"))
        self.assertEqual((result["revision"], result["text"]), (1, "最终内容。"))
        self.assertEqual((settled["state"], settled["revision"], settled["text"]), (4, 1, "最终内容。"))

    async def test_stop_and_cancel_during_microphone_open_never_read_audio(self):
        for command, expected_state in (("STOP", 5), ("CANCEL", 0)):
            with self.subTest(command=command):
                async def server(reader, writer):
                    await self.begin(reader); await send(writer, b"R")
                    self.assertEqual(await reader.read(), b"")
                self.behavior = server
                native = await self.client(delay_open=True)
                for _ in range(30):
                    await native.command("SNAP")
                    before = await native.wait(lambda e: e.get("requested"))
                    if before["opens"]: break
                    await asyncio.sleep(0.005)
                self.assertEqual((before["state"], before["opens"]), (1, 1))
                await native.command(command); result = await native.state(expected_state)
                self.assertEqual((result["opens"], result["closes"], result["bytes"]), (1, 1, 0))
                self.assertNotIn(2, [e.get("state") for e in native.events])

    async def test_stop_interrupts_tls_handshake_before_capture(self):
        entered = asyncio.Event()
        async def no_tls(reader, writer):
            entered.set()
            try:
                while await reader.read(65536): pass
            finally:
                writer.close(); await writer.wait_closed()
        plain = await asyncio.start_server(no_tls, "127.0.0.1", 0)
        previous_port = self.port
        try:
            self.port = plain.sockets[0].getsockname()[1]
            native = await self.client(); await asyncio.wait_for(entered.wait(), 2)
            await native.command("STOP")
            result = await native.wait(lambda e: e.get("state") == 5, timeout=1.5)
            self.assertEqual((result["error"], result["opens"]), ("cancelled", 0))
        finally:
            self.port = previous_port; plain.close(); await plain.wait_closed()

    async def test_partial_frame_timeout_closes_capture_and_preserves_last_snapshot(self):
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R")
            await send(writer, b"T", text_payload(1, "保留完整草稿"))
            writer.write(struct.pack("!I", 20) + b"T" + b"\0\0"); await writer.drain()
            while await reader.read(65536): pass
        self.behavior = server
        native = await self.client()
        result = await native.wait(lambda e: e.get("state") == 5, timeout=7)
        self.assertEqual((result["error"], result["revision"], result["text"]),
                         ("response_timeout", 1, "保留完整草稿"))
        self.assertEqual((result["opens"], result["closes"]), (1, 1))

    async def test_concurrent_starts_allow_only_one_worker(self):
        async def server(reader, writer):
            await self.begin(reader)
            while await reader.read(65536): pass
        self.behavior = server
        native = await self.client(); await asyncio.wait_for(self.entered.wait(), 2)
        for _ in range(10):
            await native.command("CANCEL"); await native.state(0); await asyncio.sleep(0.03)
            await native.command("START_RACE")
            result = await native.wait(lambda e: "race_results" in e)
            self.assertEqual(sorted(result["race_results"]), [-1, 0])
            await native.state(1)
        await native.command("CANCEL"); result = await native.state(0)
        self.assertEqual((result["opens"], result["bytes"]), (0, 0))

    async def test_cancel_under_network_backpressure_is_bounded(self):
        paused = asyncio.Event()
        async def server(reader, writer):
            await self.begin(reader); await send(writer, b"R")
            writer.transport.pause_reading(); paused.set()
            try:
                await asyncio.sleep(12)
            finally:
                writer.transport.resume_reading()
        self.behavior = server
        native = await self.client(burst_audio=True)
        await native.state(2); await asyncio.wait_for(paused.wait(), 2)
        previous = -1
        for _ in range(40):
            await asyncio.sleep(0.1); await native.command("SNAP")
            status = await native.wait(lambda e: e.get("requested"))
            self.assertEqual(status["state"], 2)
            if status["bytes"] == previous: break
            previous = status["bytes"]
        else:
            self.fail("fixture failed to produce network backpressure")
        self.assertGreater(status["bytes"], 0)
        await native.command("CANCEL")
        result = await native.wait(lambda e: e.get("state") == 0, timeout=1.5)
        self.assertEqual((result["opens"], result["closes"]), (1, 1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
