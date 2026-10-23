"""Audio startup ordering, without CoreAudio, Cocoa, or system mutations."""
import argparse
import contextlib
import fcntl
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

COMPANION = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(COMPANION))
import transcribe


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.binary = self.root / "build/Term50VoiceReceiver.app/Contents/MacOS/Term50VoiceReceiver"
        self.binary.parent.mkdir(parents=True)
        self.binary.touch()
        (self.root / "build/mic-route").touch()
        self.pcm = self.root / "public-fixture.pcm"
        self.pcm.write_bytes(b"\x01\x02\x03\x04" * 960)
        self.args = argparse.Namespace(device="Doubao Stream Bridge", bridge_dir=self.root / "bridge",
                                       pcm=self.pcm, manual_microphone=False, max_seconds=120,
                                       settle=4, timeout=25)
        self.stages = []
        self.events = []
        self.writes = []
        self.stream_active = False
        self.failure_stage = None
        self.receiver = mock.Mock()
        self.receiver.ready = False
        self.receiver.process.poll.return_value = None
        self.receiver.wait.side_effect = lambda event, *args, **kwargs: self.stages.append("receiver_" + event)
        self.receiver.command.side_effect = self.command
        self.receiver.preview.side_effect = lambda: self.stages.append("preview")
        self.receiver.close.side_effect = self.close_receiver
        self.receiver.wait_final.side_effect = lambda **kwargs: self.final_text()

    def command(self, action):
        self.stages.append("command_" + action)
        return "fixture-command"

    def close_receiver(self, success):
        self.stages.append("restore_receiver")
        return None

    def final_text(self):
        self.stages.append("final_text")
        return "公开测试句"

    def build_receiver(self, *args, **kwargs):
        self.stages.append("receiver_launch")
        return self.receiver

    def route(self, helper, target=None, **kwargs):
        stage = "microphone_read" if target is None else (
            "microphone_set" if target == self.args.device else "microphone_restore")
        self.stages.append(stage)
        return "fixture_previous_microphone"

    def emit(self, name, **fields):
        self.stages.append("emit_" + name)
        self.events.append((name, fields))

    def stream(self, **kwargs):
        self.stages.append("audio_construct")
        if self.failure_stage == "construct":
            raise RuntimeError("fixture audio startup failure")
        owner = self

        class Output:
            def __enter__(self):
                owner.stages.append("audio_start")
                if owner.failure_stage == "start":
                    raise RuntimeError("fixture audio startup failure")
                owner.stream_active = True
                return self

            def write(self, data):
                owner.stages.append("audio_prime" if not owner.writes else "audio_write")
                if owner.failure_stage == "prime" and not owner.writes:
                    raise RuntimeError("fixture audio startup failure")
                owner.writes.append(data)
                return False

            def __exit__(self, *args):
                owner.stages.append("audio_close")
                owner.stream_active = False

        return Output()

    @contextlib.contextmanager
    def doubles(self):
        sounddevice = SimpleNamespace(query_devices=lambda: [dict(name=self.args.device, max_output_channels=2)],
                                      RawOutputStream=self.stream)
        real_read = transcribe.os.read

        def read(*args):
            self.stages.append("pcm_read")
            return real_read(*args)

        with mock.patch.object(transcribe, "ROOT", self.root), \
             mock.patch.dict(sys.modules, {"sounddevice": sounddevice}), \
             mock.patch.object(transcribe, "Receiver", side_effect=self.build_receiver) as receiver, \
             mock.patch.object(transcribe, "microphone", side_effect=self.route) as microphone, \
             mock.patch.object(transcribe, "emit", side_effect=self.emit), \
             mock.patch.object(transcribe.signal, "signal"), \
             mock.patch.object(transcribe.select, "select", side_effect=lambda streams, *args: (streams, [], [])), \
             mock.patch.object(transcribe.os, "read", side_effect=read):
            yield receiver, microphone

    def assert_lock_released(self):
        with (self.root / "private/session.lock").open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(lock, fcntl.LOCK_UN)

    def test_audio_is_started_and_primed_before_system_changes_or_ready(self):
        with self.doubles():
            transcribe.transcribe(self.args)
        for later in ("microphone_read", "receiver_launch", "microphone_set", "command_start", "emit_ready"):
            self.assertLess(self.stages.index("audio_prime"), self.stages.index(later))
        self.assertLess(self.stages.index("receiver_ready"), self.stages.index("emit_ready"))
        self.assertLess(self.stages.index("emit_ready"), self.stages.index("pcm_read"))
        self.assertLess(self.stages.index("audio_close"), self.stages.index("command_stop"))
        self.assertLess(self.stages.index("audio_close"), self.stages.index("final_text"))
        self.assertLess(self.stages.index("restore_receiver"), self.stages.index("microphone_restore"))
        self.assertLess(self.stages.index("microphone_restore"), self.stages.index("emit_final"))
        self.assertEqual(self.writes[0], bytes(3840))
        self.assertEqual(self.writes[1], self.pcm.read_bytes())
        final = next(fields for name, fields in self.events if name == "final")
        self.assertEqual(final["audio_seconds"], 0.02)  # Prime silence is not captured speech.
        self.assertEqual(final["output_underruns"], 0)
        self.assertFalse(self.stream_active)
        self.assert_lock_released()

    def test_audio_startup_failure_never_changes_microphone_or_starts_receiver(self):
        for stage in ("construct", "start", "prime"):
            with self.subTest(stage=stage):
                self.failure_stage = stage
                self.stages.clear()
                self.events.clear()
                self.writes.clear()
                with self.doubles() as (receiver, microphone):
                    with self.assertRaisesRegex(RuntimeError, "fixture audio startup failure"):
                        transcribe.transcribe(self.args)
                receiver.assert_not_called()
                microphone.assert_not_called()
                self.assertFalse(self.events)
                self.assertFalse(self.stream_active)
                self.assert_lock_released()
                if stage == "prime":
                    self.assertIn("audio_close", self.stages)

    def test_receiver_start_failure_closes_audio_and_restores_microphone_without_ready(self):
        def wait(event, *args, **kwargs):
            if event == "ready":
                raise transcribe.BridgeError("EVENT_PERMISSION")

        self.receiver.wait.side_effect = wait
        with self.doubles():
            with self.assertRaises(transcribe.BridgeError) as failure:
                transcribe.transcribe(self.args)
        self.assertEqual(failure.exception.code, "EVENT_PERMISSION")
        self.assertFalse(self.events)
        self.assertFalse(self.stream_active)
        self.assertIn("audio_close", self.stages)
        self.assertIn("microphone_restore", self.stages)
        self.receiver.close.assert_called_once_with(False)
        self.assert_lock_released()


if __name__ == "__main__":
    unittest.main()
