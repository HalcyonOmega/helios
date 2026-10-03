"""Pure validation and status parsing for Helios's local desktop controller."""

from dataclasses import dataclass
import re
from urllib.parse import urlsplit


@dataclass(frozen=True)
class ServiceState:
    """systemd's actual state, not an inference from a tray icon."""

    load: str = "not-found"
    active: str = "inactive"
    sub: str = "dead"
    result: str = "success"
    config_managed: bool = False

    @property
    def running(self):
        """Whether systemd currently owns a running or starting host."""
        return self.active in {"active", "activating", "reloading"}


def parse_state(text):
    """Parse the explicitly named properties returned by systemctl show."""
    properties = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    return ServiceState(
        properties.get("LoadState", "not-found"),
        properties.get("ActiveState", "inactive"),
        properties.get("SubState", "dead"),
        properties.get("Result", "success"),
        bool(re.search(r"/nix/store/[^\s;\"'}]+\.conf", properties.get("ExecStart", ""))),
    )


def validate_service(service):
    """Reject options and malformed units before passing arguments to systemctl."""
    if not re.fullmatch(r"[A-Za-z0-9_@][A-Za-z0-9_.@-]*\.service", service):
        raise ValueError("Expected a systemd user service name, such as sunshine.service")
    return service


def service_arguments(action, service):
    """Construct argument vectors without invoking a shell or killing unrelated processes."""
    validate_service(service)
    if action == "status":
        return ["--user", "show", service, "--property=LoadState,ActiveState,SubState,Result,ExecStart"]
    if action not in {"start", "stop", "restart"}:
        raise ValueError("Unsupported service action")
    return ["--user", action, service]


def validate_url(url):
    """Limit embedded credentials and TLS exceptions to an HTTPS loopback origin."""
    parsed = urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname not in {"127.0.0.1", "localhost", "::1"}
            or parsed.username or parsed.password or parsed.path not in {"", "/"}
            or parsed.query or parsed.fragment):
        raise ValueError("The desktop UI requires an HTTPS loopback URL with no credentials")
    if parsed.port is None or not 1024 <= parsed.port <= 65535:
        raise ValueError("The desktop UI URL must specify a valid unprivileged port")
    return url.rstrip("/") + "/"
