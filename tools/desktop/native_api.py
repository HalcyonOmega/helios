"""Authenticated, pinned-TLS JSON access for the native Helios screens."""
import base64
import json
from pathlib import Path

from PySide6.QtCore import QByteArray, QObject, QUrl
from PySide6.QtNetwork import QNetworkAccessManager, QNetworkRequest, QSslCertificate

from service_core import validate_url


class HeliosApi(QObject):
    """Reuse the host API without a browser, persistent credentials, or disabled TLS verification."""

    def __init__(self, url, certificate, parent=None):
        """Create an asynchronous local API client with a pinned public-certificate path."""
        super().__init__(parent)
        self.url = QUrl(validate_url(url))
        self.certificate = Path(certificate)
        self.manager = QNetworkAccessManager(self)
        self.authorization = b""
        self.pending = set()
        self.generation = 0

    def set_credentials(self, username, password):
        """Keep login details only in this process; never put them in URLs, files, or logs."""
        self.clear_credentials()
        self.authorization = b"Basic " + base64.b64encode((username + ":" + password).encode())

    def clear_credentials(self):
        """Forget the current login on explicit sign-out."""
        self.cancel()
        self.authorization = b""
        self.manager.clearAccessCache()

    def trusted(self, certificate):
        """Accept a self-signed leaf only if it exactly matches this local host's public certificate."""
        certificates = QSslCertificate.fromPath(str(self.certificate))
        return bool(certificates and certificates[0].toDer() == certificate.toDer())

    def request(self, method, path, done, body=None, csrf=None):
        """Send a bounded local JSON request and return (data, error) exactly once."""
        if not path.startswith("/api/") or ".." in path or "?" in path or "#" in path:
            raise ValueError("Only local Helios API paths are supported")
        url = self.url.resolved(QUrl(path))
        request = QNetworkRequest(url)
        request.setTransferTimeout(5000)
        request.setAttribute(QNetworkRequest.Attribute.RedirectPolicyAttribute, QNetworkRequest.RedirectPolicy.ManualRedirectPolicy)
        request.setRawHeader(b"User-Agent", b"HeliosDesktop/1")
        request.setRawHeader(b"Accept", b"application/json")
        request.setRawHeader(b"Origin", self.url.toString().rstrip("/").encode())
        if self.authorization:
            request.setRawHeader(b"Authorization", self.authorization)
        if csrf:
            request.setRawHeader(b"X-CSRF-Token", csrf.encode())
        payload = QByteArray()
        if body is not None:
            payload = QByteArray(json.dumps(body).encode())
            request.setHeader(QNetworkRequest.KnownHeaders.ContentTypeHeader, "application/json")
        reply = self.manager.sendCustomRequest(request, method.encode(), payload)
        self.pending.add(reply)
        generation = self.generation
        reply.sslErrors.connect(lambda errors: reply.ignoreSslErrors(errors) if self.trusted(reply.sslConfiguration().peerCertificate()) else None)

        def finished():
            """Do not interpret a failed mutation or a login challenge as a successful action."""
            status = reply.attribute(QNetworkRequest.Attribute.HttpStatusCodeAttribute)
            text = bytes(reply.readAll()).decode(errors="replace")
            data, error = None, None
            try:
                data = json.loads(text) if text else {}
                if not isinstance(data, dict):
                    error = "The host returned an invalid API model."
            except json.JSONDecodeError:
                error = "The host returned an invalid API response."
            if status == 401:
                error = "Sign in with your Helios username and password."
            elif status is None:
                error = "Cannot reach the local host. Check its service, port, and certificate."
            elif not 200 <= status < 300:
                error = data.get("error", f"The host returned HTTP {status}.") if isinstance(data, dict) else f"HTTP {status}"
            elif isinstance(data, dict) and data.get("status") is False:
                error = data.get("error", "The host did not accept this action.")
            self.pending.discard(reply)
            reply.deleteLater()
            if generation == self.generation:
                done(data, error)

        reply.finished.connect(finished)

    def get(self, path, done):
        """Fetch a native screen's model."""
        self.request("GET", path, done)

    def mutate(self, method, path, body, done):
        """Fetch a fresh CSRF token before each explicit write, matching the server's protections."""
        def with_token(data, error):
            if error:
                done(None, error)
            elif not isinstance(data, dict) or not data.get("csrf_token"):
                done(None, "The host did not provide a CSRF token; no change was sent.")
            else:
                self.request(method, path, done, body, data["csrf_token"])
        self.get("/api/csrf-token", with_token)

    def cancel(self):
        """Cancel this UI's pending requests, without stopping the host."""
        self.generation += 1
        for reply in tuple(self.pending):
            reply.abort()
