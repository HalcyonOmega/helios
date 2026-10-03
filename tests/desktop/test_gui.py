"""Offscreen native Qt checks; service commands are redirected to a temporary fake."""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from PySide6.QtCore import QCoreApplication, QEvent, QPoint, QRect, Qt
from PySide6.QtNetwork import QSslCertificate
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QPushButton, QScrollArea
from helios_desktop import HeliosWindow
from native_widgets import AppEditor
from service_core import ServiceState


class DesktopGuiTest(unittest.TestCase):
    """Exercise native screen models and explicit controls without touching a real host."""

    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setStyle("Fusion")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="helios-gui-test-")
        root = Path(self.temp.name)
        self.calls = root / "calls"
        self.state = root / "state"
        fake = root / "fake-systemctl"
        fake.write_text(
            f"#!{sys.executable}\n"
            "from pathlib import Path\nimport sys\n"
            f"calls = Path({str(self.calls)!r})\nstate = Path({str(self.state)!r})\n"
            "action = sys.argv[2]\n"
            "with calls.open('a') as stream: stream.write(action + '\\n')\n"
            "if '-n' in sys.argv:\n"
            "    print('Fixture log: HTTPS startup failed on port 47990')\n"
            "elif action == 'show':\n"
            "    active = state.read_text() if state.exists() else 'inactive'\n"
            "    print('LoadState=loaded\\nActiveState=' + active + '\\nSubState=running\\nResult=success')\n"
            "else:\n"
            "    state.write_text('inactive' if action == 'stop' else 'active')\n"
        )
        fake.chmod(0o755)
        options = argparse.Namespace(url="https://127.0.0.1:54999/", service="sunshine.service", systemctl=str(fake), journalctl=str(fake), certificate=str(root / "no-certificate.pem"))
        self.window = HeliosWindow(options)
        self.window.probe = lambda: None
        self.wait_until(lambda: self.window.state.load == "loaded")
        self.window.timer.stop()
        self.writes = []
        self.models = {
            "/api/config": {"status": True, "platform": "linux", "version": "2026.1003", "virtual_display": "enabled", "isolate_app_audio": "enabled", "adapter_name": "/dev/dri/renderD128", "port": "47989", "custom_option": "preserved"},
            "/api/apps": {"apps": [{"name": "Desktop", "cmd": ""}, {"name": "Hollow Knight", "cmd": "steam steam://rungameid/367520"}], "detected_apps": [{"name": "Hades", "steam_appid": "1145360"}]},
            "/api/clients/list": {"named_certs": [{"uuid": "device-1", "name": "Living room", "enabled": True}]},
            "/api/pin": {"pairings": [{"id": "a" * 32, "name": "Moonlight", "address": "192.0.2.2"}]},
        }
        self.window.api.get = lambda path, done: done(self.models.get(path, {"status": True}), None)
        def mutate(method, path, body, done):
            self.writes.append((method, path, body))
            done({"status": True}, None)
        self.window.api.mutate = mutate
        self.window.confirm = lambda title, text: True

    def tearDown(self):
        self.window.close()
        self.wait_until(lambda: all(process.state().name == "NotRunning" for process in (self.window.poll_process, self.window.command, self.window.log_process)))
        self.window.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        self.temp.cleanup()

    def wait_until(self, predicate):
        """Pump only the offscreen Qt loop, with a finite deadline."""
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not predicate():
            self.app.processEvents()
            time.sleep(0.01)
        self.assertTrue(predicate(), "Qt operation did not finish")

    def connect_fixture(self):
        """Connect native models, not the user's service or configuration."""
        self.window.state = ServiceState("loaded", "active", "running")
        self.window.probed({"status": True}, None)
        self.window.username = "fixture"
        self.window.connected(self.models["/api/config"], None)

    def test_start_stop_restart_and_close(self):
        self.assertEqual(self.window.windowTitle(), "Helios")
        self.assertTrue(self.window.buttons["start"].isEnabled())
        self.assertFalse(self.window.buttons["stop"].isEnabled())
        self.assertEqual(self.calls.read_text().splitlines(), ["show"])
        self.window.control("start")
        self.wait_until(lambda: self.window.state.running and not self.window.busy)
        self.assertFalse(self.window.ready)
        self.assertTrue(self.window.buttons["stop"].isEnabled())
        self.window.control("restart")
        self.wait_until(lambda: not self.window.busy)
        self.wait_until(lambda: self.window.poll_process.state().name == "NotRunning")
        self.window.control("stop")
        self.wait_until(lambda: not self.window.state.running and not self.window.busy)
        self.window.close()
        actions = [action for action in self.calls.read_text().splitlines() if action != "show"]
        self.assertEqual(actions, ["start", "restart", "stop"])

    def test_native_screens_render_without_a_browser(self):
        self.connect_fixture()
        self.assertFalse(hasattr(self.window, "web"))
        self.assertFalse(any(module.startswith("PySide6.QtWebEngine") for module in sys.modules))
        self.assertEqual(self.window.metrics["applications"].text(), "3")
        self.assertEqual(self.window.metrics["devices"].text(), "1")
        self.window.show()
        directory = os.environ.get("HELIOS_TEST_SNAPSHOTS")
        if directory:
            Path(directory).mkdir(parents=True, exist_ok=True)
        for index, name in enumerate(("overview", "applications", "devices", "settings", "activity")):
            self.window.navigate(index)
            self.app.processEvents()
            self.assertEqual(self.window.stack.currentIndex(), index)
            image = self.window.grab()
            self.assertFalse(image.isNull())
            if directory:
                self.assertTrue(image.save(str(Path(directory) / (name + ".png"))))

    def test_red_logo_and_readable_black_red_palette(self):
        image = self.window.windowIcon().pixmap(128, 128).toImage()
        self.assertFalse(image.isNull())
        pixels = [image.pixelColor(x, y) for x in range(0, 128, 4) for y in range(0, 128, 4)]
        visible = [color for color in pixels if color.alpha() > 128]
        self.assertGreater(len(visible), 100)
        self.assertTrue(all(color.red() > color.green() and color.red() > color.blue() for color in visible))
        def luminance(color):
            """WCAG relative luminance for the reference palette's sRGB hex values."""
            channels = [int(color[index:index + 2], 16) / 255 for index in (1, 3, 5)]
            linear = [channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4 for channel in channels]
            return sum(channel * weight for channel, weight in zip(linear, (0.2126, 0.7152, 0.0722)))
        for foreground, background in [("#F2F0EA", "#AE2B32"), ("#E0DFD9", "#1C2227"), ("#A9AFB3", "#111518")]:
            light, dark = sorted((luminance(foreground), luminance(background)), reverse=True)
            self.assertGreaterEqual((light + 0.05) / (dark + 0.05), 4.5)

    def test_context_actions_are_disabled_until_usable(self):
        actions = {widget.text(): widget for widget, predicate in self.window.api_controls}
        self.assertTrue(all(not widget.isEnabled() for widget in actions.values()))
        self.connect_fixture()
        self.assertTrue(actions["Add application"].isEnabled())
        self.assertFalse(actions["Edit selected"].isEnabled())
        self.assertFalse(actions["Pair"].isEnabled())
        self.window.app_table.selectRow(0)
        self.assertTrue(actions["Edit selected"].isEnabled())
        self.window.app_search.setText("Hades")
        self.window.app_table.selectRow(0)
        self.assertFalse(actions["Edit selected"].isEnabled())
        self.assertFalse(actions["Remove"].isEnabled())
        self.window.pair_name.setText("TV")
        self.window.pin.setText("1234")
        self.assertTrue(actions["Pair"].isEnabled())
        self.window.sign_in()
        self.assertTrue(all(not widget.isEnabled() for widget in actions.values()))

    def test_minimum_layout_keyboard_navigation_and_action_feedback(self):
        self.connect_fixture()
        self.window.resize(920, 650)
        self.window.show()
        self.window.activateWindow()
        self.app.processEvents()
        QTest.keyClick(self.window, Qt.Key.Key_2, Qt.KeyboardModifier.AltModifier)
        self.wait_until(lambda: self.window.stack.currentIndex() == 1)
        self.window.app_search.setFocus()
        QTest.keyClicks(self.window.app_search, "Hades")
        self.assertEqual(self.window.app_table.rowCount(), 1)
        QTest.keyClick(self.window.app_search, Qt.Key.Key_Tab)
        self.assertNotEqual(self.app.focusWidget(), self.window.app_search)
        self.window.notice("Action failed: fixture diagnostic")
        self.window.probed({"status": True}, None)
        self.assertEqual(self.window.status.text(), "Action failed: fixture diagnostic")
        self.window.app_search.clear()
        for index in range(5):
            self.window.navigate(index)
            self.app.processEvents()
            page = self.window.stack.currentWidget()
            for widget in page.findChildren(QPushButton):
                if not widget.isVisible() or not widget.isEnabled():
                    continue
                parent = widget.parentWidget()
                while parent and not isinstance(parent, QScrollArea):
                    parent = parent.parentWidget()
                if parent:
                    parent.ensureWidgetVisible(widget)
                    self.app.processEvents()
                bounds = QRect(widget.mapTo(self.window, QPoint(0, 0)), widget.size())
                self.assertTrue(self.window.rect().contains(bounds), f"Clipped action on page {index}: {widget.text()} {bounds}")
        directory = os.environ.get("HELIOS_TEST_SNAPSHOTS")
        if directory:
            Path(directory).mkdir(parents=True, exist_ok=True)
            self.window.navigate(2)
            self.app.processEvents()
            self.assertTrue(self.window.grab().save(str(Path(directory) / "devices-compact.png")))

    def test_stopping_or_restarting_running_host_requires_confirmation(self):
        self.connect_fixture()
        self.window.confirm = lambda title, text: False
        self.window.control("stop")
        self.window.control("restart")
        self.assertFalse(self.window.busy)
        self.assertTrue(self.window.state.running)
        self.assertEqual(self.calls.read_text().splitlines(), ["show"])

    def test_advanced_options_can_be_renamed_but_cannot_override_typed_fields(self):
        self.connect_fixture()
        self.window.advanced_table.item(0, 0).setText("renamed_option")
        self.window.save_settings()
        payload = self.writes[-1][2]
        self.assertNotIn("custom_option", payload)
        self.assertEqual(payload["renamed_option"], "preserved")
        self.window.advanced_table.item(0, 0).setText("port")
        self.window.save_settings()
        self.assertEqual(len(self.writes), 1)
        self.assertIn("unique", self.window.status.text())

    def test_filtered_apps_keep_original_indices_and_detected_games_are_read_only(self):
        self.connect_fixture()
        self.window.app_search.setText("Hollow")
        self.window.app_table.selectRow(0)
        self.window.remove_app()
        self.assertEqual(self.writes[0], ("DELETE", "/api/apps/1", {}))
        self.window.app_search.setText("Hades")
        self.window.app_table.selectRow(0)
        self.window.edit_app(selected=True)
        self.window.remove_app()
        self.assertEqual(len(self.writes), 1)
        self.assertIn("custom", self.window.status.text())

    def test_app_editor_preserves_metadata_and_validates_name(self):
        original = {"name": "Game", "env": {"CUSTOM": "keep"}, "prep-cmd": [{"do": "first", "elevated": True}, {"do": "second", "elevated": False}], "detached": ["helper"]}
        dialog = AppEditor(original)
        try:
            dialog.fields["name"].clear()
            dialog.validate()
            self.assertEqual(dialog.result(), 0)
            self.assertIn("name", dialog.error.text())
            dialog.fields["name"].setText("Edited")
            dialog.prep.removeRow(0)
            dialog.prep.item(0, 0).setText("changed")
            result = dialog.payload(7)
            self.assertEqual(result["index"], 7)
            self.assertEqual(result["env"], original["env"])
            self.assertFalse(result["prep-cmd"][0]["elevated"])
            self.assertEqual(result["prep-cmd"][0]["do"], "changed")
            self.assertEqual(original["prep-cmd"][1]["do"], "second")
            dialog.validate()
            self.assertEqual(dialog.result(), 1)
        finally:
            dialog.deleteLater()

    def test_pairing_and_client_access_use_native_models(self):
        self.connect_fixture()
        self.window.approve_pairing()
        self.assertFalse(self.writes)
        self.window.pair_name.setText("TV")
        self.window.pin.setText("1234")
        self.window.approve_pairing()
        self.assertEqual(self.writes[-1], ("POST", "/api/pin", {"pairing_id": "a" * 32, "pin": "1234", "name": "TV"}))
        self.assertEqual(self.window.pin.text(), "")
        self.window.cancel_pairing()
        self.assertEqual(self.writes[-1], ("DELETE", "/api/pin", {"pairing_id": "a" * 32}))
        self.window.client_table.selectRow(0)
        self.window.toggle_client()
        self.assertEqual(self.writes[-1], ("POST", "/api/clients/update", {"uuid": "device-1", "enabled": False}))
        self.window.client_table.selectRow(0)
        self.window.unpair_client()
        self.assertEqual(self.writes[-1], ("POST", "/api/clients/unpair", {"uuid": "device-1"}))

    def test_settings_preserve_absent_defaults_and_unknown_options(self):
        self.connect_fixture()
        self.window.setting_fields["port"][0].setValue(48000)
        self.window.save_settings()
        payload = self.writes[-1][2]
        self.assertEqual(payload["port"], "48000")
        self.assertEqual(payload["custom_option"], "preserved")
        self.assertNotIn("capture", payload)
        self.assertNotIn("virtual_display_scale", payload)
        self.assertNotIn("version", payload)
        self.assertNotIn("status", payload)
        self.window.managed_settings = True
        self.window.loaded_config(self.models["/api/config"], None)
        self.assertFalse(self.window.save_settings_button.isEnabled())
        self.assertFalse(self.window.setting_fields["port"][0].isEnabled())
        self.window.save_settings()
        self.assertEqual(len(self.writes), 1)
        self.assertIn("NixOS", self.window.status.text())

    def test_sign_out_and_offline_state_prevent_writes(self):
        self.connect_fixture()
        self.window.sign_in()  # Signed-in button signs out without opening a dialog.
        self.window.close_app()
        self.window.save_settings()
        self.assertFalse(self.writes)
        self.assertFalse(self.window.authenticated)
        self.assertFalse(self.window.save_settings_button.isEnabled())
        self.assertFalse(self.window.setting_fields["port"][0].isEnabled())
        self.assertEqual(self.window.api.authorization, b"")

    def test_native_logs_and_typing_validation(self):
        self.window.show_logs()
        self.wait_until(lambda: "Fixture log" in self.window.log_text.toPlainText())
        self.connect_fixture()
        self.window.type_text.setText("é" * 1001)
        self.window.send_text()
        self.assertFalse(self.writes)
        self.window.type_text.setText("hello")
        self.window.type_enter.setChecked(True)
        self.window.send_text()
        self.assertEqual(self.writes[-1], ("POST", "/api/type", {"text": "hello", "enter": True}))
        self.assertEqual(self.window.type_text.text(), "")

    def test_missing_certificate_is_not_trusted(self):
        self.assertFalse(self.window.api.trusted(QSslCertificate()))


if __name__ == "__main__":
    unittest.main()
