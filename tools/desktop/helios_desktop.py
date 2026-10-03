#!/usr/bin/env python3
"""Helios: a native Qt control application for the local Moonlight streaming host."""
import argparse
from pathlib import Path
import os
import sys

from PySide6.QtCore import QProcess, QSize, QTimer, Qt
from PySide6.QtGui import QColor, QKeySequence, QPalette, QShortcut
from PySide6.QtWidgets import (
    QAbstractItemView, QApplication, QCheckBox, QComboBox, QDialog,
    QDialogButtonBox, QDoubleSpinBox, QFormLayout, QFrame, QGridLayout,
    QHBoxLayout, QHeaderView, QLineEdit, QMainWindow, QMessageBox,
    QPlainTextEdit, QScrollArea, QSpinBox, QStackedWidget,
    QTableWidget, QTableWidgetItem, QTabWidget, QVBoxLayout, QWidget,
)

from native_api import HeliosApi
from native_widgets import AppEditor, STYLE, button, card, helios_icon, label, navigation_icon
from service_core import ServiceState, parse_state, service_arguments, validate_service, validate_url


class HeliosWindow(QMainWindow):
    """Native models, forms and actions; opening or closing the UI never starts/stops the host."""

    def __init__(self, options):
        """Build the native application and begin read-only service status checks."""
        super().__init__()
        self.options = options
        self.state = ServiceState()
        self.busy = False
        self.ready = False
        self.authenticated = False
        self.probing = False
        self.config = {}
        self.apps = []
        self.clients = []
        self.pairings = []
        self.managed_settings = False
        self.username = ""
        self.saved_setting_values = {}
        self.api_controls = []
        self.shortcuts = []
        self.setWindowTitle("Helios")
        self.setWindowIcon(helios_icon())
        self.resize(1180, 820)
        self.setMinimumSize(920, 650)
        self.setStyleSheet(STYLE)
        palette = self.palette()
        for role, color in [
            (QPalette.ColorRole.Window, "#1C2227"),
            (QPalette.ColorRole.WindowText, "#E0DFD9"),
            (QPalette.ColorRole.Base, "#14191D"),
            (QPalette.ColorRole.AlternateBase, "#20272C"),
            (QPalette.ColorRole.Text, "#E0DFD9"),
            (QPalette.ColorRole.Button, "#252C32"),
            (QPalette.ColorRole.ButtonText, "#F2F0EA"),
            (QPalette.ColorRole.Highlight, "#AE2B32"),
            (QPalette.ColorRole.HighlightedText, "#F2F0EA"),
        ]:
            palette.setColor(role, QColor(color))
        self.setPalette(palette)
        QApplication.instance().setPalette(palette)
        QApplication.instance().setWindowIcon(self.windowIcon())
        self.api = HeliosApi(options.url, options.certificate, self)
        root = QWidget()
        root_layout = QHBoxLayout(root)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)
        sidebar = QFrame()
        sidebar.setObjectName("sidebar")
        sidebar.setFixedWidth(210)
        side = QVBoxLayout(sidebar)
        side.setContentsMargins(16, 28, 16, 22)
        side.setSpacing(8)
        brand = QHBoxLayout()
        logo = label("")
        logo.setPixmap(self.windowIcon().pixmap(38, 38))
        logo.setFixedSize(40, 40)
        logo.setAccessibleName("Helios red solar-ribbon logo")
        brand.addWidget(logo)
        brand.addWidget(label("HELIOS", "brand"))
        brand.addStretch()
        side.addLayout(brand)
        side.addWidget(label("MOONLIGHT HOST", "eyebrow"))
        side.addSpacing(32)
        self.nav = []
        for index, (title, icon) in enumerate([
            ("Overview", "overview"), ("Applications", "applications"),
            ("Devices", "devices"), ("Settings", "settings"), ("Activity", "activity"),
        ]):
            action = button(title, lambda checked=False, index=index: self.navigate(index), "nav")
            action.setCheckable(True)
            action.setIcon(navigation_icon(icon))
            action.setIconSize(QSize(20, 20))
            action.setToolTip(title + " · Alt+" + str(index + 1))
            self.shortcuts.append(QShortcut(QKeySequence("Alt+" + str(index + 1)), self, activated=lambda index=index: self.navigate(index)))
            self.nav.append(action)
            side.addWidget(action)
        side.addStretch()
        side.addWidget(label("LOCAL CONTROL", "eyebrow"))
        side.addWidget(label(options.url.replace("https://", "").rstrip("/"), "muted"))
        side.addSpacing(12)
        side.addWidget(label("Closing this window leaves the host running.", "muted"))
        root_layout.addWidget(sidebar)
        content = QWidget()
        content_layout = QVBoxLayout(content)
        content_layout.setContentsMargins(32, 28, 32, 24)
        content_layout.setSpacing(22)
        top = QHBoxLayout()
        heading = QVBoxLayout()
        self.heading = label("Overview", "heading")
        self.subtitle = label("Your streaming host, without taking over your desktop.", "subtitle")
        heading.addWidget(self.heading)
        heading.addWidget(self.subtitle)
        top.addLayout(heading, 1)
        self.badge = label("Checking host", "badge")
        self.badge.setWordWrap(False)
        self.badge.setFixedHeight(36)
        self.badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
        top.addWidget(self.badge)
        self.login_button = button("Sign in", self.sign_in)
        top.addWidget(self.login_button)
        content_layout.addLayout(top)
        self.status = label("Checking the local service…", "muted")
        content_layout.addWidget(self.status)
        self.stack = QStackedWidget()
        self.stack.addWidget(self.overview_page())
        self.stack.addWidget(self.apps_page())
        self.stack.addWidget(self.devices_page())
        self.stack.addWidget(self.settings_page())
        self.stack.addWidget(self.activity_page())
        content_layout.addWidget(self.stack, 1)
        root_layout.addWidget(content, 1)
        self.setCentralWidget(root)
        self.poll_process = QProcess(self)
        self.poll_process.finished.connect(self.polled)
        self.poll_process.errorOccurred.connect(lambda error: self.notice("Cannot query the service: " + self.poll_process.errorString()))
        self.command = QProcess(self)
        self.command.finished.connect(self.control_finished)
        self.command.errorOccurred.connect(self.control_error)
        self.command_deadline = QTimer(self)
        self.command_deadline.setSingleShot(True)
        self.command_deadline.timeout.connect(self.command.kill)
        self.poll_deadline = QTimer(self)
        self.poll_deadline.setSingleShot(True)
        self.poll_deadline.timeout.connect(self.poll_process.kill)
        self.log_process = QProcess(self)
        self.log_process.finished.connect(self.logs_loaded)
        self.log_process.errorOccurred.connect(lambda error: self.log_text.setPlainText(self.log_process.errorString()))
        self.log_deadline = QTimer(self)
        self.log_deadline.setSingleShot(True)
        self.log_deadline.timeout.connect(self.log_process.kill)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(2500)
        self.navigate(0)
        self.update_buttons()
        self.poll()

    def overview_page(self):
        """A native overview with actual service/API state and quick control actions."""
        page = QWidget()
        page.setObjectName("overviewPage")
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(20)
        hero, hero_layout = card("STREAMING HOST")
        self.host_title = label("Ready when you are.", "heading")
        hero_layout.addWidget(self.host_title)
        self.host_detail = label("Start the host to make this PC available to Moonlight. Controls stay available even if the API cannot start.", "muted")
        hero_layout.addWidget(self.host_detail)
        controls = QHBoxLayout()
        self.buttons = {}
        for title, action, kind in [("Start host", "start", "primary"), ("Stop host", "stop", "danger"), ("Restart", "restart", None)]:
            widget = button(title, lambda checked=False, action=action: self.control(action), kind)
            widget.setEnabled(False)
            self.buttons[action] = widget
            controls.addWidget(widget)
        controls.addStretch()
        controls.addWidget(button("View activity", lambda: self.navigate(4)))
        hero_layout.addLayout(controls)
        layout.addWidget(hero)
        metrics = QGridLayout()
        self.metrics = {}
        for column, (key, title, value) in enumerate([
            ("service", "SERVICE", "Checking"), ("applications", "APPLICATIONS", "—"),
            ("devices", "PAIRED DEVICES", "—"),
        ]):
            frame, inner = card(title)
            metric = label(value, "metric")
            self.metrics[key] = metric
            inner.addWidget(metric)
            metrics.addWidget(frame, 0, column)
        layout.addLayout(metrics)
        lower = QHBoxLayout()
        screen, screen_layout = card("DISPLAY & AUDIO", "A dedicated virtual output can keep the Moonlight session separate from your host desktop.")
        self.isolation_detail = label("Sign in to inspect capture and isolation settings.", "muted")
        screen_layout.addWidget(self.isolation_detail)
        screen_layout.addWidget(button("Streaming settings", lambda: self.navigate(3)))
        lower.addWidget(screen, 1)
        clients, clients_layout = card("CONNECT A DEVICE", "Open Moonlight on your client, add this host, then approve its pairing request here.")
        clients_layout.addWidget(button("Manage devices", lambda: self.navigate(2)))
        self.version_label = label("Host version · Sign in to read", "muted")
        clients_layout.addWidget(self.version_label)
        lower.addWidget(clients, 1)
        layout.addLayout(lower)
        layout.addStretch()
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(page)
        return scroll

    def new_table(self, columns):
        """A native single-row selector, with model identifiers independent of display sorting."""
        table = QTableWidget(0, len(columns))
        table.setHorizontalHeaderLabels(columns)
        table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setAlternatingRowColors(True)
        table.verticalHeader().hide()
        table.verticalHeader().setDefaultSectionSize(46)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        return table

    def apps_page(self):
        """Native app library, launcher editor and detected Steam entries."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        toolbar = QHBoxLayout()
        self.app_search = QLineEdit()
        self.app_search.setPlaceholderText("Search your application library")
        self.app_search.textChanged.connect(self.render_apps)
        toolbar.addWidget(self.app_search, 1)
        toolbar.addWidget(self.api_button("Refresh", self.refresh_apps))
        toolbar.addWidget(self.api_button("Add application", self.edit_app, "primary"))
        layout.addLayout(toolbar)
        self.app_table = self.new_table(["Application", "Source", "Launch command"])
        self.app_table.itemDoubleClicked.connect(lambda item: self.edit_app(selected=True))
        self.app_table.itemSelectionChanged.connect(self.update_buttons)
        layout.addWidget(self.app_table, 1)
        actions = QHBoxLayout()
        actions.addWidget(self.api_button("Edit selected", lambda: self.edit_app(selected=True), enabled_when=lambda: self.selected_id(self.app_table) is not None))
        actions.addWidget(self.api_button("Remove", self.remove_app, "danger", enabled_when=lambda: self.selected_id(self.app_table) is not None))
        actions.addStretch()
        actions.addWidget(self.api_button("End running app", self.close_app))
        layout.addLayout(actions)
        layout.addWidget(label("Steam games are detected automatically. Custom launchers can be edited; detected games are read-only.", "muted"))
        return page

    def devices_page(self):
        """Pairing approvals and access controls, not a recreated HTML table."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        pairing, pairing_layout = card("PAIR A NEW DEVICE", "Select a pending request and enter the four-digit PIN shown by Moonlight.")
        row = QHBoxLayout()
        self.pair_select = QComboBox()
        self.pair_select.setMinimumWidth(210)
        self.pair_select.setAccessibleName("Pending Moonlight pairing request")
        self.pair_select.currentIndexChanged.connect(self.update_buttons)
        self.pair_name = QLineEdit()
        self.pair_name.setPlaceholderText("Device name")
        self.pair_name.setAccessibleName("Device name")
        self.pair_name.textChanged.connect(self.update_buttons)
        self.pin = QLineEdit()
        self.pin.setPlaceholderText("PIN")
        self.pin.setMaxLength(4)
        self.pin.setMaximumWidth(90)
        self.pin.setAccessibleName("Four-digit Moonlight PIN")
        self.pin.setToolTip("Enter the four-digit PIN displayed on the selected Moonlight client.")
        self.pin.textChanged.connect(self.update_buttons)
        request_row = QHBoxLayout()
        request_row.addWidget(self.pair_select, 1)
        request_row.addWidget(self.pair_name, 1)
        pairing_layout.addLayout(request_row)
        row.addWidget(label("Moonlight PIN", "muted"))
        row.addWidget(self.pin)
        row.addWidget(self.api_button("Pair", self.approve_pairing, "primary", enabled_when=lambda: bool(self.pair_select.currentData()) and len(self.pin.text()) == 4 and self.pin.text().isascii() and self.pin.text().isdigit() and 1 <= len(self.pair_name.text().strip().encode()) <= 128))
        row.addWidget(self.api_button("Cancel request", self.cancel_pairing, enabled_when=lambda: bool(self.pair_select.currentData())))
        row.addStretch()
        pairing_layout.addLayout(row)
        layout.addWidget(pairing)
        toolbar = QHBoxLayout()
        toolbar.addWidget(label("Paired devices", "metric"))
        toolbar.addStretch()
        toolbar.addWidget(self.api_button("Refresh", self.refresh_clients))
        layout.addLayout(toolbar)
        self.client_table = self.new_table(["Device", "Access", "Identifier"])
        self.client_table.itemSelectionChanged.connect(self.update_buttons)
        layout.addWidget(self.client_table, 1)
        actions = QHBoxLayout()
        actions.addWidget(self.api_button("Toggle access", self.toggle_client, enabled_when=lambda: self.selected_id(self.client_table) is not None))
        actions.addWidget(self.api_button("Unpair selected", self.unpair_client, "danger", enabled_when=lambda: self.selected_id(self.client_table) is not None))
        actions.addStretch()
        layout.addLayout(actions)
        return page

    def settings_page(self):
        """Native typed streaming settings plus an advanced key/value form."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        self.settings_notice = label("Sign in to load the host configuration.", "muted")
        layout.addWidget(self.settings_notice)
        tabs = QTabWidget()
        self.setting_fields = {}
        definitions = {
            "Streaming": [
                ("capture", "Capture method", ["", "kwin", "portal", "kms", "x11"], ""),
                ("encoder", "Encoder", ["", "vulkan", "vaapi", "software", "nvenc"], ""),
                ("adapter_name", "GPU render device", "text", ""),
                ("output_name", "Capture output", "text", ""),
                ("virtual_display", "Dedicated virtual display", ["enabled", "disabled"], "enabled"),
                ("virtual_display_scale", "Virtual display scale", "scale", "1"),
                ("virtual_display_move_windows", "Move game windows to virtual output", ["enabled", "disabled"], "enabled"),
                ("isolate_app_audio", "Stream only the game's audio", ["enabled", "disabled"], "enabled"),
                ("steam_library", "Discover installed Steam games", ["enabled", "disabled"], "enabled"),
            ],
            "Network & input": [
                ("port", "Base port", "port", "47989"),
                ("address_family", "Address family", ["ipv4", "both"], "ipv4"),
                ("origin_web_ui_allowed", "Management access", ["pc", "lan", "wan"], "lan"),
                ("upnp", "Automatic router port mapping", ["disabled", "enabled"], "disabled"),
                ("controller", "Gamepad input", ["enabled", "disabled"], "enabled"),
                ("keyboard", "Keyboard input", ["enabled", "disabled"], "enabled"),
                ("mouse", "Mouse input", ["enabled", "disabled"], "enabled"),
                ("key_rightalt_to_key_win", "Map Right Alt to Super", ["enabled", "disabled"], "enabled"),
                ("audio_sink", "Audio sink", "text", ""),
            ],
        }
        for title, fields in definitions.items():
            panel = QWidget()
            panel.setObjectName("settingsForm")
            form = QFormLayout(panel)
            form.setContentsMargins(24, 24, 24, 24)
            form.setSpacing(15)
            for key, caption, kind, default in fields:
                if isinstance(kind, list):
                    editor = QComboBox()
                    for value in kind:
                        editor.addItem("Automatic" if not value else value.replace("_", " ").title(), value)
                elif kind == "scale":
                    editor = QDoubleSpinBox()
                    editor.setRange(0.5, 4.0)
                    editor.setSingleStep(0.25)
                elif kind == "port":
                    editor = QSpinBox()
                    editor.setRange(1029, 65514)
                else:
                    editor = QLineEdit()
                    editor.setPlaceholderText("Use default")
                editor.setToolTip(key)
                editor.setAccessibleName(caption)
                editor.setEnabled(False)
                self.setting_fields[key] = (editor, default)
                form.addRow(caption, editor)
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(panel)
            tabs.addTab(scroll, title.replace("&", "&&"))
        advanced = QWidget()
        advanced_layout = QVBoxLayout(advanced)
        advanced_layout.setContentsMargins(20, 20, 20, 20)
        advanced_layout.addWidget(label("Additional host options", "metric"))
        advanced_layout.addWidget(label("Options not shown in the typed forms are preserved here. Empty values use the host default.", "muted"))
        self.advanced_table = QTableWidget(0, 2)
        self.advanced_table.setEnabled(False)
        self.advanced_table.setHorizontalHeaderLabels(["Option", "Value"])
        self.advanced_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        advanced_layout.addWidget(self.advanced_table)
        self.add_option = button("Add option", self.add_setting)
        self.add_option.setEnabled(False)
        advanced_layout.addWidget(self.add_option)
        tabs.addTab(advanced, "Advanced")
        layout.addWidget(tabs, 1)
        controls = QHBoxLayout()
        controls.addWidget(button("Reload", self.refresh_config))
        controls.addStretch()
        self.save_settings_button = button("Save settings", self.save_settings, "primary")
        self.save_settings_button.setEnabled(False)
        controls.addWidget(self.save_settings_button)
        layout.addLayout(controls)
        layout.addWidget(label("Saving does not restart the host. Use Restart when you are ready to apply changes.", "muted"))
        return page

    def activity_page(self):
        """A native journal viewer and explicit host-input utility."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        row = QHBoxLayout()
        row.addWidget(label("Service activity", "metric"))
        row.addStretch()
        row.addWidget(button("Refresh logs", self.show_logs))
        layout.addLayout(row)
        self.log_text = QPlainTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setPlaceholderText("Logs are available even when the host API is offline.")
        layout.addWidget(self.log_text, 1)
        typing, inner = card("TYPE ON THE HOST", "Sends text to the focused host application. Nothing is sent until you press Type text.")
        self.type_text = QLineEdit()
        self.type_text.setPlaceholderText("Text to send (up to 2000 UTF-8 bytes)")
        self.type_text.setAccessibleName("Text to type on the host")
        self.type_text.textChanged.connect(self.update_buttons)
        inner.addWidget(self.type_text)
        actions = QHBoxLayout()
        self.type_enter = QCheckBox("Press Enter afterwards")
        self.type_enter.toggled.connect(self.update_buttons)
        actions.addWidget(self.type_enter)
        actions.addStretch()
        actions.addWidget(self.api_button("Type text", self.send_text, enabled_when=lambda: bool(self.type_text.text()) and len((self.type_text.text() + ("\n" if self.type_enter.isChecked() else "")).encode()) <= 2000))
        inner.addLayout(actions)
        layout.addWidget(typing)
        return page

    def navigate(self, index):
        """Switch native pages and refresh their models without reloading a browser."""
        self.stack.setCurrentIndex(index)
        titles = ["Overview", "Applications", "Devices", "Settings", "Activity"]
        descriptions = ["Your streaming host, without taking over your desktop.", "Choose what appears in your Moonlight library.", "Approve pairing and manage who can connect.", "Tune capture, isolation, audio, and input.", "See what happened and diagnose a failed startup."]
        self.heading.setText(titles[index])
        self.subtitle.setText(descriptions[index])
        for current, action in enumerate(self.nav):
            action.setChecked(current == index)
        if self.isVisible():
            self.nav[index].setFocus(Qt.FocusReason.ShortcutFocusReason)
        if index == 4 and hasattr(self, "log_process"):
            self.show_logs()
        elif self.authenticated:
            [self.refresh_overview, self.refresh_apps, self.refresh_clients, self.refresh_config, lambda: None][index]()

    def notice(self, text):
        """Use a non-modal native status line for asynchronous errors."""
        self.status.setText(text)

    def poll(self):
        """Query only the existing user service, asynchronously and with a deadline."""
        if self.busy or self.poll_process.state() != QProcess.ProcessState.NotRunning:
            return
        self.poll_process.start(self.options.systemctl, service_arguments("status", self.options.service))
        self.poll_deadline.start(5000)

    def polled(self, code, exit_status):
        """Keep service liveness separate from API readiness and authentication."""
        self.poll_deadline.stop()
        text = bytes(self.poll_process.readAllStandardOutput()).decode(errors="replace")
        self.state = parse_state(text)
        self.managed_settings = self.state.config_managed
        self.metrics["service"].setText("Running" if self.state.running else self.state.active.title())
        if self.state.load != "loaded":
            self.notice("Helios's user service is not installed. Activate your NixOS configuration first.")
            self.badge.setText("Not installed")
        elif not self.state.running:
            self.ready = False
            self.badge.setText("Host stopped" if self.state.active != "failed" else "Startup failed")
            self.host_title.setText("Ready when you are." if self.state.active != "failed" else "Let's get you running again.")
            self.host_detail.setText("Start the host to make this PC available to Moonlight." if self.state.active != "failed" else "Open Activity to see the startup error. Start will retry without changing your display settings.")
            self.notice("Host " + self.state.active + (" · " + self.state.result if self.state.active == "failed" else ""))
        elif not self.ready:
            self.badge.setText("Connecting")
            self.notice("Host process is running · Waiting for its local API")
        self.update_buttons()
        if self.state.running and not self.probing:
            self.probe()
        if self.authenticated and self.ready and self.stack.currentIndex() == 2:
            self.refresh_pairings()

    def probe(self):
        """Use the public JSON locale endpoint: no login dialog or browser is needed for readiness."""
        self.probing = True
        self.api.get("/api/configLocale", self.probed)

    def probed(self, data, error):
        """Only the real API response marks the host as ready."""
        self.probing = False
        previously_ready = self.ready
        self.ready = not error and self.state.running and not self.busy
        if self.ready:
            self.badge.setText("Host online")
            self.metrics["service"].setText("Running")
            self.host_title.setText("Your host is online.")
            self.host_detail.setText("Open Moonlight on your client to connect. Host controls remain local and available independently of the API.")
            if not previously_ready:
                self.notice("Local API connected" + (" · Signed in as " + self.username if self.authenticated else " · Sign in to manage applications and devices"))
        elif self.state.running:
            self.notice(error or "The API is not ready. Check Activity for a port conflict.")
        self.update_buttons()

    def api_button(self, text, callback, kind=None, enabled_when=None):
        """Keep API actions unavailable until signed in, connected and their selection is valid."""
        widget = button(text, callback, kind)
        widget.setEnabled(False)
        widget.setToolTip("Sign in to an online host; select an entry for selection-based actions.")
        self.api_controls.append((widget, enabled_when or (lambda: True)))
        return widget

    def update_buttons(self):
        """Service controls work even if authentication or HTTPS startup fails."""
        available = self.state.load == "loaded" and not self.busy
        self.buttons["start"].setEnabled(available and not self.state.running)
        self.buttons["stop"].setEnabled(available and self.state.running)
        self.buttons["restart"].setEnabled(available)
        self.login_button.setEnabled(self.ready and not self.busy)
        online = self.ready and self.authenticated and not self.busy
        for widget, enabled_when in self.api_controls:
            widget.setEnabled(online and enabled_when())
        pairing_available = online and bool(self.pair_select.currentData())
        self.pair_select.setEnabled(online and bool(self.pairings))
        self.pair_name.setEnabled(pairing_available)
        self.pin.setEnabled(pairing_available)
        self.type_text.setEnabled(online)
        self.type_enter.setEnabled(online)
        editable = online and not self.managed_settings
        self.save_settings_button.setEnabled(editable)
        self.add_option.setEnabled(editable)
        self.advanced_table.setEnabled(editable)
        for editor, default in self.setting_fields.values():
            editor.setEnabled(editable)

    def control(self, action):
        """Use systemd's managed cgroup; never guess a PID or spawn duplicate host instances."""
        if self.busy:
            return
        if action in {"stop", "restart"} and self.state.running and not self.confirm(action.capitalize() + " host", "This will disconnect active Moonlight sessions. Continue?"):
            return
        self.busy = True
        self.ready = False
        self.probing = False
        self.api.cancel()
        self.notice(action.capitalize() + " requested…")
        self.badge.setText(action.capitalize() + "…")
        self.update_buttons()
        self.command.start(self.options.systemctl, service_arguments(action, self.options.service))
        self.command_deadline.start(30000)

    def control_finished(self, code, exit_status):
        """A successful service command is not proof that the API started successfully."""
        self.command_deadline.stop()
        self.busy = False
        if code or exit_status != QProcess.ExitStatus.NormalExit:
            error = bytes(self.command.readAllStandardError()).decode(errors="replace").strip()
            self.notice(error or "Service control failed. Check Activity.")
        self.update_buttons()
        self.poll()

    def control_error(self, error):
        """Recover usable buttons after a missing executable or a crashed control client."""
        self.command_deadline.stop()
        self.busy = False
        self.notice("Cannot control the host: " + self.command.errorString())
        self.update_buttons()

    def sign_in(self):
        """Native login form, with credentials kept only in memory."""
        if self.authenticated:
            self.api.clear_credentials()
            self.authenticated = False
            self.probing = False
            self.save_settings_button.setEnabled(False)
            self.add_option.setEnabled(False)
            self.advanced_table.setEnabled(False)
            for editor, default in self.setting_fields.values():
                editor.setEnabled(False)
            self.login_button.setText("Sign in")
            self.notice("Signed out. Service controls remain available.")
            self.update_buttons()
            return
        dialog = QDialog(self)
        dialog.setWindowTitle("Connect to Helios")
        dialog.setMinimumWidth(420)
        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(26, 26, 26, 26)
        layout.setSpacing(16)
        layout.addWidget(label("Connect to your host", "metric"))
        layout.addWidget(label("Use your existing Helios username and password. They won't be saved to disk.", "muted"))
        user = QLineEdit(self.username)
        user.setPlaceholderText("Username")
        user.setAccessibleName("Helios username")
        password = QLineEdit()
        password.setPlaceholderText("Password")
        password.setAccessibleName("Helios password")
        password.setEchoMode(QLineEdit.EchoMode.Password)
        layout.addWidget(user)
        layout.addWidget(password)
        controls = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        controls.accepted.connect(dialog.accept)
        controls.rejected.connect(dialog.reject)
        layout.addWidget(controls)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self.username = user.text()
            self.probing = False
            self.api.set_credentials(self.username, password.text())
            self.api.get("/api/config", self.connected)
        password.clear()

    def connected(self, data, error):
        """Load native page models after the host accepts credentials."""
        if error:
            self.api.clear_credentials()
            self.notice(error)
            return
        self.authenticated = True
        self.login_button.setText("Sign out")
        self.loaded_config(data, None)
        self.refresh_apps()
        self.refresh_clients()
        self.notice("Connected · " + self.username)
        self.update_buttons()

    def require_login(self):
        """Never send mutations from a signed-out or offline UI."""
        if not self.ready or not self.authenticated:
            self.notice("Sign in to the online host first.")
            return False
        return True

    def mutate(self, method, path, body, refresh=None, message="Saved"):
        """Common authenticated write path; all native actions preserve CSRF protection."""
        if not self.require_login():
            return
        def completed(data, error):
            if error:
                self.notice(error)
            else:
                self.notice(message)
                if refresh:
                    refresh()
        self.api.mutate(method, path, body, completed)

    def refresh_overview(self):
        """Refresh the overview's configuration and library/device counts."""
        self.refresh_config()
        self.refresh_apps()
        self.refresh_clients()

    def refresh_apps(self):
        """Fetch the custom and automatically detected launcher lists."""
        if self.require_login():
            self.api.get("/api/apps", self.loaded_apps)

    def loaded_apps(self, data, error):
        """Replace the application model only after a successful API read."""
        if error:
            self.notice(error)
            return
        self.apps = data.get("apps", [])
        self.detected_apps = data.get("detected_apps", [])
        self.metrics["applications"].setText(str(len(self.apps) + len(self.detected_apps)))
        self.render_apps()

    def render_apps(self):
        """Filter the library while retaining each editable launcher's original API index."""
        if not hasattr(self, "app_table"):
            return
        query = self.app_search.text().casefold()
        self.app_table.setRowCount(0)
        entries = [(index, app, "Custom") for index, app in enumerate(self.apps)]
        entries += [(None, app, "Steam") for app in getattr(self, "detected_apps", [])]
        for index, app, source in entries:
            if query not in str(app.get("name", "")).casefold():
                continue
            row = self.app_table.rowCount()
            self.app_table.insertRow(row)
            for column, value in enumerate([app.get("name", "Unnamed"), source, app.get("cmd", "Automatically detected" if source == "Steam" else "Desktop")]):
                item = QTableWidgetItem(str(value))
                item.setData(Qt.ItemDataRole.UserRole, index)
                self.app_table.setItem(row, column, item)

    def selected_id(self, table):
        """Return the row's stable model identifier, rather than its visible position."""
        row = table.currentRow()
        return table.item(row, 0).data(Qt.ItemDataRole.UserRole) if row >= 0 and table.item(row, 0) else None

    def edit_app(self, checked=False, selected=False):
        """Open a native launcher form and submit it only after Save."""
        if not self.require_login():
            return
        index = self.selected_id(self.app_table) if selected else -1
        if index is None:
            self.notice("Select a custom application. Steam entries are managed automatically.")
            return
        dialog = AppEditor(self.apps[index] if index >= 0 else None, self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self.mutate("POST", "/api/apps", dialog.payload(index), self.refresh_apps, "Application saved")

    def confirm(self, title, text):
        """Require explicit approval for destructive actions, defaulting to No."""
        return QMessageBox.question(self, title, text, QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No) == QMessageBox.StandardButton.Yes

    def remove_app(self):
        """Remove a custom launcher without deleting game files."""
        index = self.selected_id(self.app_table)
        if index is None:
            self.notice("Select a custom application to remove.")
        elif self.confirm("Remove application", "Remove " + str(self.apps[index].get("name", "this application")) + " from Moonlight? Its files will not be deleted."):
            self.mutate("DELETE", "/api/apps/" + str(index), {}, self.refresh_apps, "Application removed")

    def close_app(self):
        """Ask before terminating an application used by an active stream."""
        if self.require_login() and self.confirm("End running app", "Terminate the running streamed application? This may end an active session."):
            self.mutate("POST", "/api/apps/close", {}, message="Running application closed")

    def refresh_clients(self):
        """Fetch device access state and pending pairing requests."""
        if self.require_login():
            self.api.get("/api/clients/list", self.loaded_clients)
            self.refresh_pairings()

    def loaded_clients(self, data, error):
        """Populate native paired-device rows from the host model."""
        if error:
            self.notice(error)
            return
        self.clients = data.get("named_certs", [])
        selected = self.selected_id(self.client_table)
        self.metrics["devices"].setText(str(len(self.clients)))
        self.client_table.setRowCount(0)
        self.client_table.setRowCount(len(self.clients))
        for row, client in enumerate(self.clients):
            for column, value in enumerate([client.get("name", "Unnamed device"), "Allowed" if client.get("enabled", True) else "Blocked", client.get("uuid", "")]):
                item = QTableWidgetItem(str(value))
                item.setToolTip(str(value))
                item.setData(Qt.ItemDataRole.UserRole, client.get("uuid"))
                self.client_table.setItem(row, column, item)
            if client.get("uuid") == selected:
                self.client_table.selectRow(row)

    def toggle_client(self):
        """Enable or block the selected client, warning before disconnecting it."""
        uuid = self.selected_id(self.client_table)
        client = next((client for client in self.clients if client.get("uuid") == uuid), None)
        if not client:
            self.notice("Select a paired device.")
            return
        enabled = not client.get("enabled", True)
        if not enabled and not self.confirm("Block device", "Blocking this device will terminate its active sessions. Continue?"):
            return
        self.mutate("POST", "/api/clients/update", {"uuid": uuid, "enabled": enabled}, self.refresh_clients, "Device access updated")

    def unpair_client(self):
        """Ask before removing a client's pairing certificate."""
        uuid = self.selected_id(self.client_table)
        if uuid and self.confirm("Unpair device", "This device will need to pair again before starting another session. Continue?"):
            self.mutate("POST", "/api/clients/unpair", {"uuid": uuid}, self.refresh_clients, "Device unpaired")

    def refresh_pairings(self):
        """Refresh pairing requests only while signed in and connected."""
        if self.authenticated and self.ready:
            self.api.get("/api/pin", self.loaded_pairings)

    def loaded_pairings(self, data, error):
        """Preserve the selected pending request across background refreshes."""
        if error:
            self.notice(error)
            return
        selected = self.pair_select.currentData()
        self.pairings = data.get("pairings", [])
        self.pair_select.clear()
        for pairing in self.pairings:
            self.pair_select.addItem(str(pairing.get("name", "Device")) + " · " + str(pairing.get("address", "")), pairing.get("id"))
        if not self.pairings:
            self.pair_select.addItem("No pending requests", None)
        else:
            index = self.pair_select.findData(selected)
            if index >= 0:
                self.pair_select.setCurrentIndex(index)

    def approve_pairing(self):
        """Validate the PIN and UTF-8 device-name size before approving a request."""
        pairing = self.pair_select.currentData()
        if not pairing or len(self.pin.text()) != 4 or not self.pin.text().isascii() or not self.pin.text().isdigit() or not 1 <= len(self.pair_name.text().strip().encode()) <= 128:
            self.notice("Select a request, name the device, and enter its four-digit PIN.")
            return
        def refreshed():
            self.pin.clear()
            self.pair_name.clear()
            self.refresh_clients()
        self.mutate("POST", "/api/pin", {"pairing_id": pairing, "pin": self.pin.text(), "name": self.pair_name.text().strip()}, refreshed, "Device paired")

    def cancel_pairing(self):
        """Cancel only the selected pending pairing request."""
        pairing = self.pair_select.currentData()
        if pairing:
            self.mutate("DELETE", "/api/pin", {"pairing_id": pairing}, self.refresh_pairings, "Pairing request cancelled")

    def refresh_config(self):
        """Load host configuration without writing or restarting the service."""
        if self.require_login():
            self.api.get("/api/config", self.loaded_config)

    def loaded_config(self, data, error):
        """Populate typed settings, preserving unknown options and NixOS read-only state."""
        if error:
            self.notice(error)
            return
        self.config = {key: value for key, value in data.items() if key not in {"status", "version", "platform"}}
        self.version_label.setText("Host " + str(data.get("version", "unknown")) + " · " + str(data.get("platform", "")))
        virtual = self.config.get("virtual_display", "disabled") == "enabled"
        audio = self.config.get("isolate_app_audio", "disabled") == "enabled"
        self.isolation_detail.setText(("Virtual output enabled" if virtual else "Physical output capture") + "\n" + ("Per-application audio" if audio else "Desktop audio"))
        self.settings_notice.setText("Managed by NixOS · Edit services.helios.settings and rebuild to change these values." if self.managed_settings else "Changes are saved to the host configuration. Restart separately to apply them.")
        editable = self.authenticated and not self.managed_settings
        for key, (editor, default) in self.setting_fields.items():
            value = str(self.config.get(key, default))
            if isinstance(editor, QComboBox):
                index = editor.findData(value)
                if index < 0:
                    editor.addItem(value, value)
                    index = editor.count() - 1
                editor.setCurrentIndex(index)
            elif isinstance(editor, QDoubleSpinBox):
                try:
                    editor.setValue(float(value))
                except ValueError:
                    editor.setValue(float(default))
            elif isinstance(editor, QSpinBox):
                try:
                    editor.setValue(int(value))
                except ValueError:
                    editor.setValue(int(default))
            else:
                editor.setText(value)
            editor.setEnabled(editable)
            self.saved_setting_values[key] = self.setting_value(editor)
        advanced = {key: value for key, value in self.config.items() if key not in self.setting_fields}
        self.advanced_table.setRowCount(len(advanced))
        for row, (key, value) in enumerate(sorted(advanced.items())):
            self.advanced_table.setItem(row, 0, QTableWidgetItem(key))
            self.advanced_table.setItem(row, 1, QTableWidgetItem(str(value)))
        self.advanced_table.setEnabled(editable)
        self.add_option.setEnabled(editable)
        self.save_settings_button.setEnabled(editable)

    def add_setting(self):
        """Add an editable native option row without sending it to the host."""
        row = self.advanced_table.rowCount()
        self.advanced_table.insertRow(row)
        self.advanced_table.setItem(row, 0, QTableWidgetItem(""))
        self.advanced_table.setItem(row, 1, QTableWidgetItem(""))
        self.advanced_table.editItem(self.advanced_table.item(row, 0))

    def setting_value(self, editor):
        """Normalize native input values without forcing defaults into the host's saved configuration."""
        if isinstance(editor, QComboBox):
            return editor.currentData()
        if isinstance(editor, (QSpinBox, QDoubleSpinBox)):
            return str(editor.value())
        return editor.text()

    def save_settings(self):
        """Save explicit changes while preserving omitted defaults and validating advanced rows."""
        if self.managed_settings:
            self.notice("These settings are generated by NixOS and cannot be edited here.")
            return
        result = {key: value for key, value in self.config.items() if key in self.setting_fields}
        for key, (editor, default) in self.setting_fields.items():
            value = self.setting_value(editor)
            if value != self.saved_setting_values.get(key):
                if value != "":
                    result[key] = value
                else:
                    result.pop(key, None)
        seen = set(self.setting_fields)
        for row in range(self.advanced_table.rowCount()):
            key = self.advanced_table.item(row, 0).text().strip() if self.advanced_table.item(row, 0) else ""
            value = self.advanced_table.item(row, 1).text() if self.advanced_table.item(row, 1) else ""
            if key:
                if key in seen:
                    self.notice("Each option must be unique. Use the typed form for options already shown there.")
                    return
                seen.add(key)
                if any(char in key for char in "\n\r=") or any(char in value for char in "\n\r"):
                    self.notice("Option names and values must be single lines.")
                    return
                result[key] = value
        self.mutate("POST", "/api/config", result, self.refresh_config, "Settings saved · Restart when ready to apply")

    def show_logs(self):
        """Native journal text remains available when every host API has shut down."""
        if self.log_process.state() != QProcess.ProcessState.NotRunning:
            return
        self.log_process.start(self.options.journalctl, ["--user", "-u", self.options.service, "-n", "150", "--no-pager"])
        self.log_deadline.start(5000)

    def logs_loaded(self, code, exit_status):
        """Display bounded journal output, including command errors, as plain native text."""
        self.log_deadline.stop()
        content = bytes(self.log_process.readAllStandardOutput()).decode(errors="replace")
        error = bytes(self.log_process.readAllStandardError()).decode(errors="replace")
        self.log_text.setPlainText(content or error or "No service log entries yet.")
        self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())

    def send_text(self):
        """Validate UTF-8 size and submit host typing only after the explicit action."""
        text = self.type_text.text()
        if not text or len((text + ("\n" if self.type_enter.isChecked() else "")).encode()) > 2000:
            self.notice("Enter text of at most 2000 UTF-8 bytes.")
            return
        self.mutate("POST", "/api/type", {"text": text, "enter": self.type_enter.isChecked()}, self.type_text.clear, "Text sent to the host")

    def closeEvent(self, event):
        """Closing the native controller must not stop a stream or kill the host."""
        self.timer.stop()
        self.api.cancel()
        super().closeEvent(event)


def main():
    """Launch native control screens without automatically changing the host or any display."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="https://127.0.0.1:47990/")
    parser.add_argument("--service", default="sunshine.service")
    parser.add_argument("--systemctl", default="systemctl")
    parser.add_argument("--journalctl", default="journalctl")
    parser.add_argument("--certificate", default=str(Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "sunshine/credentials/cacert.pem"))
    options = parser.parse_args()
    try:
        options.url = validate_url(options.url)
        validate_service(options.service)
    except ValueError as error:
        parser.error(str(error))
    app = QApplication(sys.argv[:1])
    app.setStyle("Fusion")
    app.setApplicationName("helios-desktop")
    app.setApplicationDisplayName("Helios")
    app.setDesktopFileName("io.github.HalcyonOmega.Helios")
    window = HeliosWindow(options)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
