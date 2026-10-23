"""Service lifecycle tests use only temporary files and inert process doubles."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

COMPANION = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(COMPANION))
import service


class ServiceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.root = self.directory / "companion"
        self.private = self.root / "private"
        self.private.mkdir(parents=True)
        self.key = self.private / "pairing.key"
        self.secret = "a1" * 32
        self.key.write_text(self.secret + "\n")
        self.key.chmod(0o600)
        self.bridge = self.directory / "豆包 bridge"
        python = self.bridge / ".venv/bin/python"
        python.parent.mkdir(parents=True)
        python.write_text("unused runtime fixture\n")
        python.chmod(0o700)
        self.plist = self.directory / "LaunchAgents" / (service.LABEL + ".plist")
        for name, value in (("ROOT", self.root), ("PLIST", self.plist)):
            patch = mock.patch.object(service, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        platform = mock.patch.object(service.sys, "platform", "darwin")
        platform.start()
        self.addCleanup(platform.stop)
        # No test may run launchctl, a client, doctor, or a bridge process.
        run = mock.patch.object(service.subprocess, "run", return_value=
                                subprocess.CompletedProcess([], 0, "", ""))
        self.run = run.start()
        self.addCleanup(run.stop)
        launchctl = mock.patch.object(service, "launchctl", return_value=
                                      subprocess.CompletedProcess([], 0, "", ""))
        self.launchctl = launchctl.start()
        self.addCleanup(launchctl.stop)

    def args(self, bind="192.0.2.98", port=45850):
        return argparse.Namespace(bind=bind, port=port, bridge_dir=self.bridge)

    def write_config(self, port=45851):
        cfg = service.config("192.0.2.98", port, self.bridge)
        self.plist.parent.mkdir(parents=True, exist_ok=True)
        # Preserve exact original bytes, not merely an equivalent parsed plist.
        original = plistlib.dumps(cfg) + b"\n"
        self.plist.write_bytes(original)
        self.plist.chmod(0o600)
        return cfg, original

    def invoke(self, action):
        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(service.sys, "argv", ["service.py", action]), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = service.main()
        return code, output.getvalue(), errors.getvalue()

    def test_failed_bootout_cannot_replace_loaded_service_or_bootstrap(self):
        _, original = self.write_config()
        self.launchctl.return_value = subprocess.CompletedProcess([], 5, "", "")
        with mock.patch.object(service, "job", return_value=dict(loaded=True, pid=1234)):
            with self.assertRaises(RuntimeError):
                service.install(self.args())
        self.assertEqual(self.plist.read_bytes(), original)
        self.assertFalse(any(call.args[0] == "bootstrap" for call in self.launchctl.call_args_list))

    def test_bootstrap_failure_restores_exact_previous_config_and_reloads_it(self):
        _, original = self.write_config()
        loaded_plists = []

        def launch(*args, **kwargs):
            if args[0] == "bootstrap":
                loaded_plists.append(self.plist.read_bytes())
                if len(loaded_plists) == 1:
                    raise RuntimeError("fixture bootstrap failure")
            return subprocess.CompletedProcess([], 0, "", "")

        self.launchctl.side_effect = launch
        with mock.patch.object(service, "job", side_effect=[dict(loaded=True, pid=1234),
                                                            dict(loaded=False, pid=None)]):
            with self.assertRaises(RuntimeError):
                service.install(self.args())
        self.assertEqual(self.plist.read_bytes(), original)
        self.assertGreaterEqual(len(loaded_plists), 2)
        self.assertNotEqual(loaded_plists[0], original)
        self.assertEqual(loaded_plists[-1], original)

    def test_restart_installed_but_unloaded_job_bootstraps_saved_plist(self):
        self.write_config()
        with mock.patch.object(service, "job", return_value=dict(loaded=False, pid=None)), \
             mock.patch.object(service, "wait_ready", return_value=
                               dict(loaded=True, pid=4321, authenticated_health=True)):
            code, output, errors = self.invoke("restart")
        self.assertEqual(code, 0, errors)
        self.assertEqual(json.loads(output)["action"], "restarted")
        self.assertTrue(any(call.args[0] == "bootstrap" and call.args[-1] == str(self.plist)
                            for call in self.launchctl.call_args_list))
        self.assertFalse(any(call.args[0] == "kickstart" for call in self.launchctl.call_args_list))

    def test_absent_status_returns_structured_false_without_creating_files(self):
        with mock.patch.object(service, "job", return_value=dict(loaded=False, pid=None)):
            code, output, errors = self.invoke("status")
        self.assertEqual(code, 1)
        self.assertEqual(errors, "")
        report = json.loads(output)
        self.assertEqual(report["action"], "status")
        self.assertFalse(report["loaded"])
        self.assertFalse(report["authenticated_health"])
        self.assertFalse(self.plist.exists())
        self.assertEqual(self.key.read_text(), self.secret + "\n")

    def test_status_discards_health_process_output_and_private_logs(self):
        self.write_config()
        private_diagnostic = "PRIVATE_LOG_AND_CHILD_OUTPUT"
        (self.private / "service.stderr.log").write_text(private_diagnostic)
        self.run.return_value = subprocess.CompletedProcess([], 0, self.secret, private_diagnostic)
        with mock.patch.object(service, "job", return_value=dict(loaded=True, pid=1234)):
            code, output, errors = self.invoke("status")
        self.assertEqual(code, 0, errors)
        self.assertTrue(json.loads(output)["authenticated_health"])
        self.assertNotIn(self.secret, output + errors)
        self.assertNotIn(private_diagnostic, output + errors)

    def test_status_failure_redacts_subprocess_exception_material(self):
        self.write_config()
        diagnostic = "PRIVATE_PROCESS_EXCEPTION"
        with mock.patch.object(service, "job", return_value=dict(loaded=True, pid=1234)), \
             mock.patch.object(service, "health", side_effect=
                               subprocess.TimeoutExpired([diagnostic], 1, output=self.secret)):
            code, output, errors = self.invoke("status")
        self.assertEqual(code, 1)
        self.assertTrue(errors)
        self.assertNotIn(self.secret, output + errors)
        self.assertNotIn(diagnostic, output + errors)

    def test_binding_rejects_unspecified_multicast_and_non_numeric_hosts(self):
        for bind in ("0.0.0.0", "::", "224.0.0.1", "ff02::1", "mac.local"):
            with self.subTest(bind=bind), self.assertRaises(ValueError):
                service.config(bind, 45850, self.bridge)
        self.assertFalse(self.plist.exists())
        self.launchctl.assert_not_called()

    def test_config_keeps_key_material_out_of_persistent_service_settings(self):
        cfg = service.config("192.0.2.98", 45850, self.bridge)
        encoded = plistlib.dumps(cfg)
        self.assertNotIn(self.secret.encode(), encoded)
        self.assertEqual(cfg["ProgramArguments"][0], str(self.bridge / ".venv/bin/python"))
        self.assertIn(str(self.key), cfg["ProgramArguments"])
        self.assertEqual(cfg["StandardErrorPath"], str(self.private / "service.stderr.log"))
        self.assertTrue(cfg["RunAtLoad"])
        self.assertTrue(cfg["KeepAlive"])


if __name__ == "__main__":
    unittest.main()
