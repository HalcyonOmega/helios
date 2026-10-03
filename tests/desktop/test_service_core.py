"""Controller checks that never execute systemctl or touch a real host/display."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from service_core import parse_state, service_arguments, validate_service, validate_url


class ServiceCoreTest(unittest.TestCase):
    """Validate safe control arguments and honest readiness-independent status."""

    def test_running_and_failed_states(self):
        state = parse_state("LoadState=loaded\nActiveState=active\nSubState=running\nResult=success\n")
        self.assertTrue(state.running)
        self.assertEqual(state.load, "loaded")
        state = parse_state("LoadState=loaded\nActiveState=failed\nResult=exit-code\n")
        self.assertFalse(state.running)
        self.assertEqual(state.result, "exit-code")
        self.assertFalse(parse_state("").running)

    def test_commands_use_user_service_not_pid_killing(self):
        for action in ("start", "stop", "restart"):
            self.assertEqual(service_arguments(action, "sunshine.service"), ["--user", action, "sunshine.service"])
        self.assertIn("--property=LoadState,ActiveState,SubState,Result,ExecStart", service_arguments("status", "sunshine.service"))
        with self.assertRaises(ValueError):
            service_arguments("kill", "sunshine.service")

    def test_rejects_service_options_and_shell_fragments(self):
        for service in ("--system", "sunshine.service; reboot", "../sunshine.service", "", "*.service"):
            with self.subTest(service=service), self.assertRaises(ValueError):
                validate_service(service)

    def test_requires_exact_https_loopback_origin(self):
        self.assertEqual(validate_url("https://127.0.0.1:47990"), "https://127.0.0.1:47990/")
        self.assertEqual(validate_url("https://[::1]:47990/"), "https://[::1]:47990/")
        for url in ("http://localhost:47990/", "https://localhost.evil:47990/", "https://192.168.0.77:47990/", "https://user:pass@localhost:47990/", "https://localhost:47990/config", "https://localhost:47990/?x=1", "https://localhost/", "https://localhost:80/"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                validate_url(url)

    def test_only_store_configuration_is_read_only(self):
        self.assertTrue(parse_state("ExecStart={ argv[]=/nix/store/abc-helios/bin/helios /nix/store/def-sunshine.conf ; }").config_managed)
        self.assertFalse(parse_state("ExecStart={ argv[]=/nix/store/abc-helios/bin/helios /home/user/.config/sunshine/sunshine.conf ; }").config_managed)


if __name__ == "__main__":
    unittest.main()
