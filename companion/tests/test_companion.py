import asyncio
import contextlib
import fcntl
import json
import os
import pathlib
import signal
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from protocol import CIPHER, VoiceError, receive, send, text_payload
from server import Companion, load_key, tls_context
from client import context_for
from transcribe import BridgeError, Receiver, microphone


def client_context(key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # Authentication is TLS-PSK, not a certificate.
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers(CIPHER)
    context.set_psk_client_callback(lambda hint: ("term50", key))
    return context


@unittest.skipUnless(hasattr(ssl.SSLContext, "set_psk_server_callback"), "TLS-PSK Python required")
class CompanionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.key = load_key(pathlib.Path(self.directory.name) / "pairing.key")
        self.marker = pathlib.Path(self.directory.name) / "cancelled"
        self.companion = Companion([sys.executable, str(ROOT / "tests/mock_bridge.py"), str(self.marker)],
                                   startup_timeout=2, final_timeout=2)
        self.server = await asyncio.start_server(self.companion.handle, "127.0.0.1", 0,
                                                 ssl=tls_context(self.key), ssl_handshake_timeout=1,
                                                 ssl_shutdown_timeout=1)
        self.port = self.server.sockets[0].getsockname()[1]
        self.clients = []

    async def asyncTearDown(self):
        for writer in self.clients:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
        self.server.close(); await self.server.wait_closed()
        tasks = list(self.companion.connections)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        self.directory.cleanup()

    async def connect(self, key=None):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port,
                                                       ssl=client_context(key or self.key))
        self.clients.append(writer)
        return reader, writer

    async def begin(self):
        reader, writer = await self.connect()
        await send(writer, b"B")
        self.assertEqual(await receive(reader, 3), (b"R", b""))
        return reader, writer

    async def wait_idle(self):
        for _ in range(100):
            if not self.companion.active:
                return
            await asyncio.sleep(0.01)
        self.fail("companion did not release exclusive session")

    async def test_health_and_key_permissions(self):
        reader, writer = await self.connect()
        await send(writer, b"P")
        self.assertEqual(await receive(reader), (b"K", b"TERM50/1"))
        self.assertEqual(os.stat(pathlib.Path(self.directory.name) / "pairing.key").st_mode & 0o777, 0o600)

    async def test_wrong_key_rejected(self):
        loop = asyncio.get_running_loop()
        previous = loop.get_exception_handler()
        loop.set_exception_handler(lambda *args: None)  # Expected failed TLS handshakes.
        try:
            with self.assertRaises((ssl.SSLError, ConnectionError)):
                await self.connect(os.urandom(32))
        finally:
            loop.set_exception_handler(previous)

    async def test_fragmented_audio_and_final_rewrite(self):
        reader, writer = await self.begin()
        frame = struct.pack("!I", 7681) + b"A" + bytes(7680)
        # Exercise independent TCP boundaries in header, frame type and audio.
        for piece in (frame[:2], frame[2:4], frame[4:3000], frame[3000:]):
            writer.write(piece); await writer.drain()
        await send(writer, b"S")
        result = []
        while True:
            kind, payload = await receive(reader, 3)
            self.assertIn(kind, (b"T", b"F"))
            revision = struct.unpack("!I", payload[:4])[0]
            text = payload[4:].decode("utf-8")
            result.append((revision, text))
            if kind == b"F":
                break
        self.assertEqual([row[0] for row in result], list(range(1, len(result) + 1)))
        self.assertIn("三点", result[-2][1])
        self.assertEqual(result[-1][1], "今天下午3点，检查服务。")

    async def test_exclusive_and_cancel(self):
        reader, writer = await self.begin()
        other, second = await self.connect()
        await send(second, b"B")
        self.assertEqual(await receive(other), (b"E", b"BUSY"))
        await send(writer, b"C")
        self.assertEqual(await receive(reader), (b"E", b"CANCELLED"))
        await self.wait_idle()
        self.assertEqual(self.marker.read_text(), "cancelled\n")
        await self.begin()  # The next session can start after cleanup.

    async def test_disconnect_cancels(self):
        reader, writer = await self.begin()
        writer.close(); await writer.wait_closed()
        await self.wait_idle()
        self.assertTrue(self.marker.exists())

    async def test_bad_pcm_and_oversized_frames(self):
        reader, writer = await self.begin()
        await send(writer, b"A", b"123")
        self.assertEqual(await receive(reader), (b"E", b"BAD_AUDIO"))
        await self.wait_idle()
        reader, writer = await self.connect()
        writer.write(struct.pack("!I", 65537)); await writer.drain()
        self.assertEqual(await receive(reader), (b"E", b"BAD_FRAME"))

    async def test_audio_after_stop_rejected(self):
        reader, writer = await self.begin()
        # One coalesced write makes both control frames available before a mock final.
        writer.write(struct.pack("!I", 1) + b"S" + struct.pack("!I", 5) + b"A1234")
        await writer.drain()
        self.assertEqual(await receive(reader), (b"E", b"BAD_STATE"))

    async def test_cancel_during_startup(self):
        self.companion.command.append("slow_start")
        reader, writer = await self.connect()
        await send(writer, b"B")
        await asyncio.sleep(0.03)
        await send(writer, b"C")
        self.assertEqual(await receive(reader), (b"E", b"CANCELLED"))
        await self.wait_idle()

    async def test_cleanup_failure_never_sends_final(self):
        self.companion.command.append("cleanup_error")
        reader, writer = await self.begin()
        await send(writer, b"S")
        kind, payload = await receive(reader)
        self.assertEqual(kind, b"T")
        self.assertEqual(await receive(reader), (b"E", b"MICROPHONE_RESTORE_FAILED"))

    async def test_final_requires_clean_bridge_exit_and_no_trailing_events(self):
        for mode, expected in (("bad_exit_after_final", b"BRIDGE_EXIT"),
                               ("trailing_event_after_final", b"BRIDGE_PROTOCOL")):
            with self.subTest(mode=mode):
                self.companion.command = self.companion.command[:3] + [mode]
                reader, writer = await self.begin()
                await send(writer, b"S")
                self.assertEqual((await receive(reader))[0], b"T")
                self.assertEqual(await receive(reader), (b"E", expected))
                await self.wait_idle()

    async def test_final_claim_does_not_extend_final_deadline(self):
        self.companion.command.append("hang_after_final")
        self.companion.final_timeout = 0.05
        reader, writer = await self.begin()
        await send(writer, b"S")
        self.assertEqual((await receive(reader))[0], b"T")
        self.assertEqual(await receive(reader), (b"E", b"FINAL_TIMEOUT"))
        await self.wait_idle()

    async def test_final_timeout_and_audio_duration_limit(self):
        self.companion.command.append("slow_final")
        self.companion.final_timeout = 0.05
        reader, writer = await self.begin()
        await send(writer, b"S")
        kind, payload = await receive(reader)
        self.assertEqual(kind, b"T")
        self.assertEqual(await receive(reader), (b"E", b"FINAL_TIMEOUT"))
        await self.wait_idle()
        self.companion.max_seconds = 0.05
        reader, writer = await self.begin()
        self.assertEqual(await receive(reader), (b"E", b"AUDIO_LIMIT"))

    async def test_continuous_partials_cannot_extend_deadlines(self):
        for mode, code in (("stream_final", b"FINAL_TIMEOUT"),
                           ("stream_recording", b"AUDIO_LIMIT")):
            with self.subTest(mode=mode):
                self.companion.command = self.companion.command[:3] + [mode]
                self.companion.final_timeout = 0.05
                self.companion.max_seconds = 0.05 if mode == "stream_recording" else 120
                reader, writer = await self.begin()
                if mode == "stream_final":
                    await send(writer, b"S")
                started = time.monotonic()
                while True:
                    kind, payload = await receive(reader, 1)
                    if kind != b"T":
                        break
                self.assertEqual((kind, payload), (b"E", code))
                self.assertLess(time.monotonic() - started, 0.25)
                await self.wait_idle()

    async def test_cancel_and_disconnect_after_stop_never_deliver_final(self):
        self.companion.command.append("slow_final")
        reader, writer = await self.begin()
        await send(writer, b"S")
        self.assertEqual((await receive(reader))[0], b"T")
        await send(writer, b"C")
        self.assertEqual(await receive(reader), (b"E", b"CANCELLED"))
        with self.assertRaises(asyncio.IncompleteReadError):
            await receive(reader, 2)
        await self.wait_idle()
        self.marker.unlink()
        reader, writer = await self.begin()
        await send(writer, b"S")
        self.assertEqual((await receive(reader))[0], b"T")
        writer.close(); await writer.wait_closed()
        await self.wait_idle()
        self.assertTrue(self.marker.exists())

    async def test_shutdown_during_subprocess_creation_still_cleans_child(self):
        original = asyncio.create_subprocess_exec
        created = asyncio.Event()
        async def delayed(*args, **kwargs):
            child = await original(*args, **kwargs)
            created.set()
            await asyncio.sleep(0.05)
            return child
        with mock.patch("server.asyncio.create_subprocess_exec", delayed):
            reader, writer = await self.connect()
            await send(writer, b"B")
            await asyncio.wait_for(created.wait(), 2)
            task = next(iter(self.companion.connections))
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        await self.wait_idle()
        self.assertEqual(self.marker.read_text(), "cancelled\n")


class AdapterTests(unittest.TestCase):
    def test_route_helper_failures_expose_only_stable_error_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            helper = pathlib.Path(directory) / "route-fixture"
            for error, expected in [("MICROPHONE_ROUTE_PAGE_TIMEOUT", "MICROPHONE_ROUTE_PAGE_TIMEOUT"),
                                    ("private freeform diagnostic", "MICROPHONE_ROUTE_FAILED")]:
                helper.write_text("#!" + sys.executable + "\nimport json\nprint(json.dumps(" +
                                  repr({"error": error}) + "))\nraise SystemExit(1)\n")
                helper.chmod(0o700)
                with self.assertRaises(BridgeError) as failure:
                    microphone(helper)
                self.assertEqual(failure.exception.code, expected)

    def test_route_helper_success_requires_a_nonempty_previous_microphone(self):
        with tempfile.TemporaryDirectory() as directory:
            helper = pathlib.Path(directory) / "route-fixture"
            for result in ('[]', '{}', '{"previous": ""}', '{"previous": "   "}',
                           '{"previous": 123}', 'not JSON'):
                with self.subTest(result=result):
                    helper.write_text("#!" + sys.executable + "\nprint(" + repr(result) + ")\n")
                    helper.chmod(0o700)
                    with self.assertRaises(BridgeError) as failure:
                        microphone(helper)
                    self.assertEqual(failure.exception.code, "MICROPHONE_ROUTE_PROTOCOL")

    def test_restore_failure_is_not_an_acknowledgement(self):
        receiver = Receiver.__new__(Receiver)
        receiver.poll = mock.Mock(return_value=[dict(event="restore_failed", id="cleanup",
                                                    code="INPUT_SOURCE_RESTORE_FAILED")])
        with self.assertRaises(BridgeError) as failure:
            receiver.wait("restored", "cleanup", tolerate_errors=True)
        self.assertEqual(failure.exception.code, "INPUT_SOURCE_RESTORE_FAILED")

    def test_hard_kill_keeps_receiver_lock_and_stops_route_child(self):
        # Entirely inert subprocesses: no Cocoa receiver, audio, or settings UI.
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            runtime = root / "runtime"; runtime.mkdir()
            release = root / "release"
            lock_path = root / "session.lock"
            route_lock = root / "route.lock"
            route_ready = root / "route-ready"
            mock_receiver = root / "receiver"
            mock_receiver.write_text("#!" + sys.executable + "\n" + """
import json,pathlib,sys,time
root=pathlib.Path(sys.argv[1]).parent
(root/'invocation.json').write_text(json.dumps(sys.argv[3:]))
while not (root/'release').exists(): time.sleep(0.01)
""")
            mock_receiver.chmod(0o700)
            helper = root / "route-helper"
            helper.write_text("#!" + sys.executable + "\n" + """
import fcntl,pathlib,sys,time
root=pathlib.Path(sys.argv[0]).parent
lock=(root/'route.lock').open('w')
fcntl.flock(lock,fcntl.LOCK_EX)
(root/'route-ready').touch()
while True: time.sleep(0.05)
""")
            helper.chmod(0o700)
            owner = root / "owner.py"
            owner.write_text("""
import fcntl,pathlib,sys,time
sys.path.insert(0,sys.argv[1])
from transcribe import Receiver,microphone
root=pathlib.Path(sys.argv[2])
lock=(root/'session.lock').open('w')
fcntl.flock(lock,fcntl.LOCK_EX)
receiver=Receiver(root/'runtime',root/'receiver',helper=root/'route-helper',
 previous_microphone='prior microphone',recovery_report=root/'recovery.json',lock_fds=[lock.fileno()])
microphone(root/'route-helper')
""")
            process = subprocess.Popen([sys.executable, str(owner), str(ROOT), str(root)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            def wait_for(predicate):
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if predicate():
                        return
                    time.sleep(0.01)
                self.fail("mock lifecycle deadline")
            def unlocked(path):
                if not path.exists():
                    return False
                with path.open("rb") as stream:
                    try:
                        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        return True
                    except BlockingIOError:
                        return False
            try:
                wait_for(lambda: route_ready.exists() and (root / "invocation.json").exists())
                self.assertEqual(json.loads((root / "invocation.json").read_text()),
                                 [str(helper), "prior microphone", str(root / "recovery.json")])
                process.kill(); process.wait(timeout=2)
                self.assertFalse(unlocked(lock_path))  # Surviving Receiver still owns the lock.
                wait_for(lambda: unlocked(route_lock))  # Orphaned route mutation is stopped.
                release.touch()
                wait_for(lambda: unlocked(lock_path))
            finally:
                release.touch()
                if process.poll() is None:
                    process.kill(); process.wait(timeout=2)

    def test_cleanup_does_not_emit_preview_to_disconnected_stdout(self):
        receiver = Receiver.__new__(Receiver)
        receiver.ready = True
        receiver.events_file = mock.Mock()
        receiver.process = mock.Mock()
        receiver.process.poll.return_value = None
        receiver.process.wait.return_value = 0
        receiver.command = mock.Mock(return_value="cleanup")
        def restored(*args, **kwargs):
            self.assertFalse(receiver.ready)
        receiver.wait = restored
        self.assertIsNone(receiver.close(False))
        receiver.command.assert_called_once_with("cancel")
        receiver.events_file.close.assert_called_once()

    def test_exited_receiver_is_not_a_restore_acknowledgement(self):
        for exit_code in (0, 1, -signal.SIGKILL):
            with self.subTest(exit_code=exit_code):
                receiver = Receiver.__new__(Receiver)
                receiver.ready = True
                receiver.events_file = mock.Mock()
                receiver.process = mock.Mock()
                receiver.process.poll.return_value = exit_code
                receiver.process.wait.return_value = exit_code
                failure = receiver.close(True)
                self.assertIsInstance(failure, BridgeError)
                self.assertEqual(failure.code, "RECEIVER_CLEANUP_FAILED")
                receiver.events_file.close.assert_called_once()

    def test_abnormal_exit_after_restore_acknowledgement_is_failure(self):
        receiver = Receiver.__new__(Receiver)
        receiver.events_file = mock.Mock()
        receiver.process = mock.Mock()
        receiver.process.poll.return_value = None
        receiver.process.wait.return_value = -signal.SIGKILL
        receiver.command = mock.Mock(return_value="cleanup")
        receiver.wait = mock.Mock()
        self.assertEqual(receiver.close(True).code, "RECEIVER_CLEANUP_FAILED")

    def test_incremental_json_keeps_partial_tail_and_filters_sessions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "events.jsonl"
            path.touch()
            receiver = Receiver.__new__(Receiver)
            receiver.path = root; receiver.session = "ours"
            receiver.events_file = path.open("rb")
            receiver.pending = b""; receiver.next_lease = 10**20
            receiver.ready = False; receiver.text = ""; receiver.marked = False
            receiver.changed = 0; receiver.last_preview = None; receiver.last_revision = 0
            payload = json.dumps(dict(event="text", session="ours", revision=1,
                                      text="中文", marked=True), ensure_ascii=False).encode() + b"\n"
            with path.open("ab") as stream:
                stream.write(payload[:-2]); stream.flush()
                self.assertEqual(receiver.poll(), [])
                stream.write(payload[-2:]); stream.flush()
                self.assertEqual(len(receiver.poll()), 1)
                self.assertEqual((receiver.text, receiver.marked), ("中文", True))
                stream.write(json.dumps(dict(event="text", session="old", text="stale", marked=False)).encode() + b"\n")
                stream.flush()
                self.assertEqual(receiver.poll(), [])
                self.assertEqual(receiver.text, "中文")
            receiver.events_file.close()

    def test_receiver_rejects_malformed_text_events_without_advancing_state(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.jsonl"
            receiver = Receiver.__new__(Receiver)
            receiver.session = "ours"; receiver.pending = b""; receiver.next_lease = 10**20
            receiver.ready = False; receiver.text = "first"; receiver.marked = False
            receiver.changed = 0; receiver.last_preview = "first"; receiver.last_revision = 1
            samples = [
                [],
                dict(event="text", session="ours", revision=2, text="next", marked="false"),
                dict(event="text", session="ours", revision=1, text="next", marked=False),
                dict(event="text", session="ours", revision=True, text="next", marked=False),
                dict(event="text", session="ours", revision=2, text="\ud800", marked=False),
            ]
            with path.open("wb") as output, path.open("rb") as receiver.events_file:
                for item in samples:
                    with self.subTest(item=item):
                        receiver.pending = b""
                        output.write(json.dumps(item).encode() + b"\n"); output.flush()
                        with self.assertRaises(BridgeError) as failure:
                            receiver.poll()
                        self.assertEqual(failure.exception.code, "RECEIVER_PROTOCOL")
                        self.assertEqual((receiver.text, receiver.marked, receiver.last_revision),
                                         ("first", False, 1))

    def test_receiver_error_codes_are_stable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.jsonl"
            receiver = Receiver.__new__(Receiver)
            receiver.session = "ours"; receiver.pending = b""; receiver.next_lease = 10**20
            with path.open("wb") as output, path.open("rb") as receiver.events_file:
                output.write(json.dumps(dict(event="error", session="ours",
                                             code="private freeform diagnostic")).encode() + b"\n")
                output.flush()
                with self.assertRaises(BridgeError) as failure:
                    receiver.poll()
                self.assertEqual(failure.exception.code, "RECEIVER_ERROR")
                output.write(json.dumps(dict(event="restore_failed", session="ours",
                                             code="private freeform diagnostic")).encode() + b"\n")
                output.flush()
                self.assertEqual(receiver.poll()[0]["code"], "RECEIVER_CLEANUP_FAILED")

    def test_final_settle_resets_for_revisions_that_return_to_same_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "events.jsonl"; path.touch()
            receiver = Receiver.__new__(Receiver)
            receiver.path = root; receiver.session = "ours"
            receiver.pending = b""; receiver.next_lease = 10**20
            receiver.ready = False; receiver.text = "初稿"; receiver.marked = False
            receiver.changed = 0; receiver.last_preview = "初稿"; receiver.last_revision = 1
            receiver.process = mock.Mock(); receiver.process.poll.return_value = None
            clock = [0.0]
            revised = False
            def pause(seconds):
                nonlocal revised
                clock[0] += seconds
                if clock[0] >= 0.75 and not revised:
                    revised = True
                    with path.open("a") as stream:
                        for revision, text in enumerate(("新修订", "初稿"), start=2):
                            stream.write(json.dumps(dict(event="text", session="ours", revision=revision,
                                                         text=text, marked=False)) + "\n")
            with path.open("rb") as receiver.events_file, \
                    mock.patch("transcribe.time.monotonic", side_effect=lambda: clock[0]), \
                    mock.patch("transcribe.time.sleep", side_effect=pause):
                self.assertEqual(receiver.wait_final(settle=1, timeout=3), "初稿")
                self.assertGreaterEqual(clock[0], 1.75)
                self.assertLess(clock[0], 1.9)
                receiver.process.poll.return_value = 0
                with self.assertRaises(BridgeError) as failure:
                    receiver.wait_final(settle=0.1, timeout=1)
                self.assertEqual(failure.exception.code, "RECEIVER_EXIT")

    def test_text_limits_and_terminal_controls(self):
        self.assertEqual(text_payload(1, "中文")[4:].decode(), "中文")
        for text in ("\x1b[31m", "x\x00y", "\x85", "中" * 6000):
            with self.assertRaises(VoiceError):
                text_payload(1, text)

    def test_key_permissions_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "key"
            path.write_text("a" * 64); path.chmod(0o644)
            with self.assertRaises(RuntimeError):
                load_key(path)

    def test_both_key_readers_reject_embedded_whitespace_and_invalid_hex(self):
        samples = {
            "space": b"a" * 30 + b"  " + b"a" * 32,
            "tab": b"a" * 30 + b"\t\t" + b"a" * 32,
            "newline": b"a" * 30 + b"\n\n" + b"a" * 32,
            "nonhex": b"g" * 64,
            "short": b"a" * 63,
            "long": b"a" * 65,
            "nonascii": b"a" * 62 + b"\xff\xff",
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "pairing.key"
            for name, value in samples.items():
                path.write_bytes(value); path.chmod(0o600)
                for reader in (load_key, context_for):
                    with self.subTest(sample=name, reader=reader.__name__):
                        with self.assertRaises(RuntimeError):
                            reader(path)
                        self.assertTrue(path.read_bytes() == value)

    def test_both_key_readers_require_private_regular_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "pairing.key"
            path.write_text(os.urandom(32).hex() + "\n")
            for mode in (0o644, 0o400, 0o660):
                path.chmod(mode)
                for reader in (load_key, context_for):
                    with self.subTest(mode=oct(mode), reader=reader.__name__):
                        with self.assertRaises(RuntimeError):
                            reader(path)
            path.chmod(0o600)
            link = root / "link"; link.symlink_to(path)
            fifo = root / "fifo"; os.mkfifo(fifo, 0o600)
            folder = root / "folder"; folder.mkdir()
            for invalid in (link, fifo, folder):
                for reader in (load_key, context_for):
                    with self.subTest(kind=invalid.name, reader=reader.__name__):
                        with self.assertRaises(RuntimeError):
                            reader(invalid)

    def test_valid_key_readers_produce_exactly_32_byte_psk(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "pairing.key"
            key = os.urandom(32)
            path.write_text(key.hex().upper() + "\n"); path.chmod(0o600)
            self.assertEqual(len(load_key(path)), 32)
            self.assertTrue(load_key(path) == key)
            with mock.patch("client.ssl.SSLContext") as constructor:
                context_for(path)
                callback = constructor.return_value.set_psk_client_callback.call_args.args[0]
                identity, provided = callback(None)
                self.assertEqual(identity, "term50")
                self.assertEqual(len(provided), 32)
                self.assertTrue(provided == key)


if __name__ == "__main__":
    unittest.main(verbosity=2)
