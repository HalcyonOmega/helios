"""Native Qt widgets and application editor; no HTML or browser components."""
from copy import deepcopy
from pathlib import Path

from PySide6.QtCore import QByteArray, Qt
from PySide6.QtGui import QIcon, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import (
    QCheckBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout, QFrame,
    QHBoxLayout, QLabel, QLineEdit, QPlainTextEdit, QPushButton, QSpinBox,
    QTableWidget, QTableWidgetItem, QTabWidget, QVBoxLayout, QWidget,
)

STYLE = """
QMainWindow, QDialog { background: #111518; color: #F2F0EA; }
QWidget { color: #E0DFD9; font-family: 'Noto Sans', sans-serif; font-size: 13px; }
QWidget#overviewPage { background: #111518; }
QWidget#settingsForm { background: #1C2227; }
QFrame#sidebar { background: #14191D; border-right: 1px solid #343B41; }
QLabel#brand { font-size: 24px; font-weight: 700; color: #F2F0EA; }
QLabel#eyebrow { color: #ACAFAF; font-size: 10px; font-weight: 600; }
QLabel#heading { font-size: 28px; font-weight: 600; color: #F2F0EA; }
QLabel#subtitle, QLabel#muted { color: #A9AFB3; }
QLabel#metric { font-size: 23px; font-weight: 600; color: #F2F0EA; }
QLabel#badge { color: #F2F0EA; background: #252C32; border: 1px solid #454B50; padding: 6px 13px; border-radius: 5px; }
QFrame#card { background: #1C2227; border: 1px solid #343B41; border-radius: 7px; }
QPushButton { background: #252C32; border: 1px solid #454B50; border-radius: 5px; min-height: 20px; padding: 9px 14px; }
QPushButton:hover { background: #343B41; border-color: #6D7378; }
QPushButton:pressed { background: #191E22; }
QPushButton:focus { border: 2px solid #D54850; padding: 8px 13px; }
QPushButton:disabled { color: #888E92; background: #1D2226; border-color: #343B41; }
QPushButton#primary { background: #AE2B32; border-color: #C74049; color: #F2F0EA; font-weight: 600; }
QPushButton#primary:hover { background: #C43640; border-color: #DC6269; }
QPushButton#primary:pressed { background: #8C222A; }
QPushButton#primary:disabled { background: #432229; color: #AC8D91; border-color: #5D333B; }
QPushButton#danger { color: #F2CFD0; border-color: #78424A; background: #332027; }
QPushButton#danger:disabled { color: #9C8B90; border-color: #4D373E; background: #231C20; }
QPushButton#nav { text-align: left; background: transparent; border: 1px solid transparent; padding: 11px 14px; color: #B8BFC3; }
QPushButton#nav:checked { background: #352127; border-color: #78424A; color: #F2F0EA; font-weight: 600; }
QPushButton#nav:hover { background: #252C32; }
QPushButton#nav:focus { border: 1px solid #D54850; }
QLineEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #14191D; border: 1px solid #454B50; border-radius: 4px; padding: 8px; selection-background-color: #AE2B32; }
QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #D54850; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: #969DA1; background: #20262B; }
QComboBox QAbstractItemView { background: #252C32; selection-background-color: #AE2B32; }
QTableWidget { background: #1C2227; alternate-background-color: #20272C; border: 1px solid #343B41; border-radius: 5px; gridline-color: #343B41; selection-background-color: #57252D; selection-color: #F2F0EA; }
QHeaderView::section { background: #252C32; color: #B8BFC3; padding: 11px; border: 0; border-bottom: 1px solid #454B50; font-weight: 600; }
QTableWidget::item { padding: 8px; }
QTabWidget::pane { border: 1px solid #343B41; background: #1C2227; border-radius: 5px; }
QTabBar::tab { background: #20262B; color: #B8BFC3; padding: 10px 17px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #F2F0EA; background: #302027; border-bottom: 2px solid #D54850; }
QCheckBox { spacing: 9px; padding: 5px; }
QCheckBox::indicator { width: 18px; height: 18px; }
QScrollArea { border: 0; background: transparent; }
QScrollBar:vertical { background: #14191D; width: 10px; }
QScrollBar::handle:vertical { background: #555E65; border-radius: 4px; min-height: 24px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QToolTip { color: #F2F0EA; background: #252C32; border: 1px solid #78424A; padding: 7px; }
"""


def helios_icon():
    """Use the red solar-ribbon logo in the window and sidebar, even before system installation."""
    packaged = Path(__file__).with_name("helios.svg")
    source = Path(__file__).resolve().parents[2] / "src_assets/common/helios.svg"
    return QIcon(str(packaged if packaged.exists() else source))


def navigation_icon(name):
    """Draw sharp, monochrome navigation symbols rather than platform-dependent colored icons."""
    paths = {
        "overview": '<rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8M12 17v4"/>',
        "applications": '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
        "devices": '<rect x="2" y="3" width="13" height="14" rx="2"/><rect x="17" y="8" width="5" height="13" rx="1"/><path d="M6 21h6M9 17v4"/>',
        "settings": '<path d="M4 6h16M4 12h16M4 18h16"/><circle cx="9" cy="6" r="2" fill="#14191D"/><circle cx="15" cy="12" r="2" fill="#14191D"/><circle cx="8" cy="18" r="2" fill="#14191D"/>',
        "activity": '<path d="M4 3h12l4 4v14H4zM15 3v5h5M7 12h10M7 16h7"/>',
    }
    svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#C6CBCB" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">' + paths[name] + '</g></svg>'
    renderer = QSvgRenderer(QByteArray(svg.encode()))
    pixmap = QPixmap(48, 48)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    renderer.render(painter)
    painter.end()
    return QIcon(pixmap)


def label(text, name=None):
    """A plain-text label: API values must not be interpreted as rich text."""
    widget = QLabel(text)
    widget.setTextFormat(Qt.TextFormat.PlainText)
    if name:
        widget.setObjectName(name)
    widget.setWordWrap(True)
    return widget


def card(title, description=None):
    """Return a native, consistently spaced panel and its content layout."""
    frame = QFrame()
    frame.setObjectName("card")
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(22, 20, 22, 20)
    layout.setSpacing(12)
    layout.addWidget(label(title, "eyebrow"))
    if description:
        layout.addWidget(label(description, "muted"))
    return frame, layout


def button(text, callback, kind=None):
    """Create a real native action button, not a link or a web element."""
    widget = QPushButton(text)
    if kind:
        widget.setObjectName(kind)
    widget.clicked.connect(callback)
    return widget


class AppEditor(QDialog):
    """Native launcher form preserving unedited app metadata and startup command lists."""

    def __init__(self, app=None, parent=None):
        """Build native launcher, startup-command and behavior forms."""
        super().__init__(parent)
        self.original = deepcopy(app or {})
        self.setWindowTitle("Edit application" if app else "Add application")
        self.resize(640, 640)
        layout = QVBoxLayout(self)
        layout.addWidget(label("Application launcher", "heading"))
        layout.addWidget(label("Choose what Moonlight launches. Leaving the command empty streams the desktop.", "muted"))
        tabs = QTabWidget()
        general = QWidget()
        form = QFormLayout(general)
        form.setContentsMargins(20, 20, 20, 20)
        form.setSpacing(14)
        self.fields = {}
        for key, title, placeholder in [
            ("name", "Name", "How it appears in Moonlight"),
            ("cmd", "Launch command", "steam steam://rungameid/…"),
            ("working-dir", "Working directory", "Use the application's default"),
            ("image-path", "Cover image", "Local PNG path (optional)"),
            ("output", "Command output", "Log file path (optional)"),
        ]:
            field = QLineEdit(str(self.original.get(key, "")))
            field.setPlaceholderText(placeholder)
            self.fields[key] = field
            if key in {"working-dir", "image-path"}:
                row = QWidget()
                row_layout = QHBoxLayout(row)
                row_layout.setContentsMargins(0, 0, 0, 0)
                row_layout.addWidget(field)
                row_layout.addWidget(button("Browse", lambda checked=False, key=key: self.browse(key)))
                form.addRow(title, row)
            else:
                form.addRow(title, field)
        self.error = label("")
        form.addRow(self.error)
        tabs.addTab(general, "Launcher")
        startup = QWidget()
        startup_layout = QVBoxLayout(startup)
        startup_layout.setContentsMargins(20, 20, 20, 20)
        startup_layout.addWidget(label("Preparation commands", "metric"))
        startup_layout.addWidget(label("Run before launch, and undo after the session ends.", "muted"))
        self.prep = QTableWidget(0, 2)
        self.prep.setHorizontalHeaderLabels(["Before launch", "After exit"])
        self.prep.horizontalHeader().setStretchLastSection(True)
        for command in self.original.get("prep-cmd", []):
            self.add_prep(command.get("do", ""), command.get("undo", ""))
            self.prep.item(self.prep.rowCount() - 1, 0).setData(Qt.ItemDataRole.UserRole, deepcopy(command))
        startup_layout.addWidget(self.prep)
        row = QHBoxLayout()
        row.addWidget(button("Add command", lambda: self.add_prep()))
        row.addWidget(button("Remove selected", lambda: self.prep.removeRow(self.prep.currentRow()) if self.prep.currentRow() >= 0 else None))
        row.addStretch()
        startup_layout.addLayout(row)
        startup_layout.addWidget(label("Detached commands (one per line)", "muted"))
        self.detached = QPlainTextEdit("\n".join(self.original.get("detached", [])))
        self.detached.setMaximumHeight(95)
        startup_layout.addWidget(self.detached)
        tabs.addTab(startup, "Startup & cleanup")
        behavior = QWidget()
        behavior_form = QFormLayout(behavior)
        behavior_form.setContentsMargins(20, 20, 20, 20)
        self.flags = {}
        for key, title, default in [
            ("auto-detach", "Keep streaming if the launcher exits", True),
            ("wait-all", "Wait for all child processes", True),
            ("exclude-global-prep-cmd", "Skip global preparation commands", False),
            ("elevated", "Request elevated privileges", False),
        ]:
            check = QCheckBox(title)
            check.setChecked(bool(self.original.get(key, default)))
            self.flags[key] = check
            behavior_form.addRow(check)
        self.timeout = QSpinBox()
        self.timeout.setRange(0, 600)
        self.timeout.setSuffix(" seconds")
        self.timeout.setValue(int(self.original.get("exit-timeout", 5)))
        behavior_form.addRow("Graceful exit timeout", self.timeout)
        tabs.addTab(behavior, "Behavior")
        layout.addWidget(tabs)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.validate)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def browse(self, key):
        """Use native file/directory pickers; never execute the selected path."""
        if key == "working-dir":
            path = QFileDialog.getExistingDirectory(self, "Choose working directory")
        else:
            path, _ = QFileDialog.getOpenFileName(self, "Choose cover image", "", "PNG images (*.png)")
        if path:
            self.fields[key].setText(path)

    def add_prep(self, before="", after=""):
        """Append an editable pair without silently dropping existing elevated-command metadata."""
        row = self.prep.rowCount()
        self.prep.insertRow(row)
        self.prep.setItem(row, 0, QTableWidgetItem(before))
        self.prep.setItem(row, 1, QTableWidgetItem(after))

    def validate(self):
        """Require an application name before sending any API mutation."""
        if not self.fields["name"].text().strip():
            self.error.setText("Give this application a name.")
            return
        self.accept()

    def payload(self, index):
        """Serialize the same launcher schema used by the host, with its original list index."""
        result = deepcopy(self.original)
        result.update({key: field.text() for key, field in self.fields.items()})
        result.update({key: field.isChecked() for key, field in self.flags.items()})
        result["index"] = index
        result["exit-timeout"] = self.timeout.value()
        result["detached"] = [line for line in self.detached.toPlainText().splitlines() if line.strip()]
        result["prep-cmd"] = []
        for row in range(self.prep.rowCount()):
            command = deepcopy(self.prep.item(row, 0).data(Qt.ItemDataRole.UserRole) or {})
            command.update({key: self.prep.item(row, column).text() if self.prep.item(row, column) else "" for column, key in enumerate(("do", "undo"))})
            result["prep-cmd"].append(command)
        return result
