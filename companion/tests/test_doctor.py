import contextlib
import io
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import doctor


class DoctorTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.key = self.root / "private/pairing.key"

    def write_key(self, value=b"a" * 64 + b"\n", mode=0o600):
        self.key.parent.mkdir(exist_ok=True)
        self.key.write_bytes(value)
        self.key.chmod(mode)

    def test_absent_key_check_does_not_create_secret_or_directory(self):
        self.assertEqual(doctor.inspect_key(self.key)[0], "info")
        self.assertFalse(self.key.parent.exists())

    def test_key_format_permissions_and_non_regular_file(self):
        self.write_key()
        self.assertEqual(doctor.inspect_key(self.key)[0], "ok")
        for raw in (b"a" * 62 + b"  ", b"zz" * 32, b"a" * 64 + b" " * 130 + b"invalid"):
            with self.subTest(raw_length=len(raw)):
                self.write_key(raw)
                self.assertEqual(doctor.inspect_key(self.key)[0], "error")
        self.write_key(mode=0o644)
        self.assertEqual(doctor.inspect_key(self.key)[0], "error")
        target = self.root / "target"
        target.write_text("b" * 64)
        target.chmod(0o600)
        self.key.unlink()
        self.key.symlink_to(target)
        self.assertEqual(doctor.inspect_key(self.key)[0], "error")
        self.key.unlink()
        os.mkfifo(self.key, 0o600)
        self.assertEqual(doctor.inspect_key(self.key)[0], "error")

    def test_audio_probe_never_exposes_library_error_output(self):
        secret = "PRIVATE_LIBRARY_DIAGNOSTIC"
        failures = [
            subprocess.CompletedProcess([], 1, secret, secret),
            subprocess.CompletedProcess([], 0, secret, secret),
            subprocess.CompletedProcess([], 0, json.dumps({"code": secret}), secret),
            subprocess.CompletedProcess([], 0, "[]", secret),
            subprocess.CompletedProcess([], 0, '{"code": []}', secret),
            subprocess.CompletedProcess([], 0, '{"code": {}}', secret),
        ]
        for result in failures:
            with self.subTest(output_kind=result.returncode):
                with mock.patch.object(doctor.subprocess, "run", return_value=result):
                    status, message = doctor.probe_audio(Path("/unused/python"))
                self.assertEqual(status, "error")
                self.assertNotIn(secret, message)
        with mock.patch.object(doctor.subprocess, "run", side_effect=subprocess.TimeoutExpired([], 6)):
            self.assertEqual(doctor.probe_audio(Path("/unused/python"))[0], "error")

    def test_audio_probe_maps_status_and_is_bounded(self):
        for code in ("OK", "AUDIO_LIBRARY_MISSING", "VIRTUAL_DEVICE_MISSING", "AUDIO_FORMAT_UNAVAILABLE"):
            with self.subTest(code=code):
                result = subprocess.CompletedProcess([], 0, json.dumps({"code": code}), "")
                with mock.patch.object(doctor.subprocess, "run", return_value=result) as run:
                    status, _ = doctor.probe_audio(Path("/unused/python"))
                self.assertEqual(status, "ok" if code == "OK" else "error")
                self.assertEqual(run.call_args.kwargs["timeout"], 6)

    def test_missing_runtime_report_is_read_only_and_does_not_probe_audio(self):
        with mock.patch.object(doctor, "probe_audio") as probe:
            result = doctor.inspect(self.root / "missing-bridge", self.key,
                                    root=self.root, platform="darwin")
        probe.assert_not_called()
        self.assertFalse(result["ok"])
        self.assertFalse(self.key.parent.exists())
        self.assertEqual(result["scope"], "local_prerequisites")
        self.assertIn("bridge_runtime", [c["name"] for c in result["checks"] if c["status"] == "error"])

    @unittest.skipUnless(hasattr(ssl.SSLContext, "set_psk_server_callback"), "TLS-PSK Python required")
    def test_complete_report_redacts_key_and_labels_recognition_boundary(self):
        secret = b"cafebabe" * 8
        self.write_key(secret)
        for relative in (".venv/bin/python", "build/mic-route",
                         "build/Term50VoiceReceiver.app/Contents/MacOS/Term50VoiceReceiver"):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
            path.chmod(0o700)
        with mock.patch.object(doctor, "probe_audio", return_value=("ok", "fixture")):
            result = doctor.inspect(self.root, self.key, root=self.root, platform="darwin")
        self.assertTrue(result["ok"])
        self.assertNotIn(secret.decode(), json.dumps(result))
        self.assertEqual(result["checks"][-1]["name"], "recognition_boundary")

    def test_cli_returns_failure_with_parseable_json(self):
        output = io.StringIO()
        with mock.patch.object(sys, "argv", ["doctor", "--json", "--bridge-dir", str(self.root),
                                            "--key-file", str(self.key)]):
            with contextlib.redirect_stdout(output):
                status = doctor.main()
        self.assertEqual(status, 1)
        self.assertFalse(json.loads(output.getvalue())["ok"])
        self.assertFalse(self.key.exists())


if __name__ == "__main__":
    unittest.main()
