"""Native JSON/TLS integration tests on a private ephemeral HTTPS fixture, never real Helios."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from PySide6.QtCore import QCoreApplication, QEvent
from PySide6.QtWidgets import QApplication
from helios_desktop import HeliosWindow
from native_api import HeliosApi


class Handler(BaseHTTPRequestHandler):
    """Require Basic auth and a fresh CSRF token, and record only fixture requests."""

    def response(self, status, data):
        content = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def authorized(self):
        if self.headers.get("Authorization") == "Basic dGVzdDp0ZXN0":
            return True
        self.response(401, {"error": "Unauthorized"})
        return False

    def do_GET(self):
        self.server.calls.append(("GET", self.path, {key.lower(): value for key, value in self.headers.items()}, None))
        if self.path == "/api/configLocale":
            self.response(200, {"status": True, "locale": "en"})
        elif not self.authorized():
            return
        elif self.path == "/api/csrf-token":
            self.response(200, self.server.token)
        else:
            self.response(200, self.server.models.get(self.path, {"status": True}))

    def do_POST(self):
        content = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        self.server.calls.append((self.command, self.path, {key.lower(): value for key, value in self.headers.items()}, content))
        if not self.authorized():
            return
        if self.headers.get("X-CSRF-Token") != "fixture-token":
            self.response(400, {"error": "Missing CSRF token", "status": False})
        else:
            self.response(200, self.server.result)

    do_DELETE = do_POST

    def log_message(self, *args):
        """No credentials or fixture payloads in test logs."""


class NativeApiTest(unittest.TestCase):
    """Test real QNetwork requests, pinned TLS, login, page models and CSRF-protected writes."""

    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.temp = tempfile.TemporaryDirectory(prefix="helios-api-test-")
        root = Path(cls.temp.name)
        cls.cert, key = root / "certificate.pem", root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cls.cert), "-days", "1", "-subj", "/CN=helios-test"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cls.cert, key)
        cls.server.socket = context.wrap_socket(cls.server.socket, server_side=True)
        cls.worker = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.worker.start()
        cls.url = f"https://127.0.0.1:{cls.server.server_port}/"
        cls.fake = root / "fake-systemctl"
        cls.fake.write_text(f"#!{sys.executable}\nprint('LoadState=loaded\\nActiveState=active\\nSubState=running\\nResult=success')\n")
        cls.fake.chmod(0o755)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join(timeout=3)
        cls.temp.cleanup()

    def setUp(self):
        self.server.calls = []
        self.server.models = {
            "/api/config": {"status": True, "platform": "linux", "version": "fixture", "port": "47989"},
            "/api/apps": {"apps": [{"name": "Fixture game", "cmd": "fixture"}], "detected_apps": []},
            "/api/clients/list": {"named_certs": [{"name": "Fixture client", "uuid": "uuid-1", "enabled": True}]},
            "/api/pin": {"pairings": []},
        }
        self.server.token = {"csrf_token": "fixture-token"}
        self.server.result = {"status": True}
        self.api = HeliosApi(self.url, self.cert)
        self.results = []

    def tearDown(self):
        self.api.cancel()
        self.api.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)

    def wait_until(self, predicate):
        """Keep network testing bounded and isolated from the desktop event loop."""
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline and not predicate():
            self.app.processEvents()
            time.sleep(0.01)
        self.assertTrue(predicate(), "Fixture operation did not finish")

    def done(self, data, error):
        self.results.append((data, error))

    def test_pinned_tls_and_basic_authentication(self):
        self.api.get("/api/config", self.done)
        self.wait_until(lambda: self.results)
        self.assertIn("Sign in", self.results[-1][1])
        self.results.clear()
        self.api.set_credentials("test", "test")
        self.api.get("/api/config", self.done)
        self.wait_until(lambda: self.results)
        self.assertIsNone(self.results[-1][1])
        self.assertEqual(self.results[-1][0]["version"], "fixture")
        self.api.clear_credentials()
        self.assertEqual(self.api.authorization, b"")

    def test_wrong_or_missing_certificate_rejects_connection(self):
        self.api.certificate = self.cert.parent / "missing.pem"
        self.api.get("/api/configLocale", self.done)
        self.wait_until(lambda: self.results)
        self.assertIsNotNone(self.results[-1][1])
        self.assertFalse(self.server.calls)

    def test_each_mutation_fetches_a_csrf_token_before_sending(self):
        self.api.set_credentials("test", "test")
        for number, (method, path, body) in enumerate([
            ("POST", "/api/apps", {"name": "Native", "index": -1}),
            ("DELETE", "/api/apps/0", {}),
            ("POST", "/api/clients/update", {"uuid": "uuid-1", "enabled": False}),
        ]):
            self.api.mutate(method, path, body, self.done)
            self.wait_until(lambda: len(self.results) == number + 1)
            self.assertIsNone(self.results[-1][1])
            token_call, write = self.server.calls[-2:]
            self.assertEqual(token_call[:2], ("GET", "/api/csrf-token"))
            self.assertEqual(write[:2], (method, path))
            self.assertEqual(write[3], body)
            self.assertEqual(write[2]["x-csrf-token"], "fixture-token")
            self.assertEqual(write[2]["authorization"], "Basic dGVzdDp0ZXN0")
            self.assertEqual(write[2]["origin"], self.url.rstrip("/"))

    def test_missing_token_does_not_send_a_write(self):
        self.api.set_credentials("test", "test")
        self.server.token = {}
        self.api.mutate("POST", "/api/config", {"port": "48000"}, self.done)
        self.wait_until(lambda: self.results)
        self.assertIn("no change", self.results[-1][1])
        self.assertEqual([call[0] for call in self.server.calls], ["GET"])

    def test_failed_action_and_invalid_models_are_not_success(self):
        self.api.set_credentials("test", "test")
        self.server.result = {"status": False, "error": "Fixture rejected action"}
        self.api.mutate("POST", "/api/type", {"text": "fixture"}, self.done)
        self.wait_until(lambda: self.results)
        self.assertEqual(self.results[-1][1], "Fixture rejected action")
        self.server.models["/api/apps"] = []
        self.api.get("/api/apps", self.done)
        self.wait_until(lambda: len(self.results) == 2)
        self.assertIn("invalid API model", self.results[-1][1])
        with self.assertRaises(ValueError):
            self.api.get("https://example.com/", self.done)

    def test_cancellation_suppresses_stale_callbacks(self):
        self.api.get("/api/configLocale", self.done)
        self.api.cancel()
        self.wait_until(lambda: not self.api.pending)
        self.assertFalse(self.results)

    def test_native_window_loads_models_over_real_https_without_service_changes(self):
        options = argparse.Namespace(url=self.url, service="sunshine.service", systemctl=str(self.fake), journalctl=str(self.fake), certificate=str(self.cert))
        window = HeliosWindow(options)
        try:
            self.wait_until(lambda: window.ready)
            window.api.set_credentials("test", "test")
            window.api.get("/api/config", window.connected)
            self.wait_until(lambda: window.authenticated and window.app_table.rowCount() == 1 and window.client_table.rowCount() == 1)
            self.assertEqual(window.app_table.item(0, 0).text(), "Fixture game")
            self.assertEqual(window.client_table.item(0, 0).text(), "Fixture client")
            self.assertTrue(window.buttons["stop"].isEnabled())
            self.assertFalse(hasattr(window, "web"))
            self.assertTrue(all(call[0] == "GET" for call in self.server.calls))
        finally:
            window.close()
            self.wait_until(lambda: window.poll_process.state().name == "NotRunning")
            window.deleteLater()
            QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)


if __name__ == "__main__":
    unittest.main()
