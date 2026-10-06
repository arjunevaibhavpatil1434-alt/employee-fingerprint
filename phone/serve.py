#!/usr/bin/env python3
"""
Serves the Employees Access phone app and forwards API calls to the
backend, so the app and the API share one origin (the backend has no CORS).

    python3 serve.py                      # app on :8080, backend at 127.0.0.1:8000
    python3 serve.py --port 8080 --backend http://127.0.0.1:8000

Only GET /health*, GET /api/* and POST /api/* are forwarded.
"""

import argparse
import json
import os
import urllib.error
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

APP_DIR = os.path.dirname(os.path.abspath(__file__))
PROXY_PREFIXES = ("/api/", "/health")
MAX_BODY = 64 * 1024


class AppHandler(SimpleHTTPRequestHandler):
    backend = "http://127.0.0.1:8000"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=APP_DIR, **kwargs)

    def end_headers(self):
        # Let the service worker decide what to cache
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def _is_proxied(self):
        return self.path.startswith(PROXY_PREFIXES)

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _forward_headers(self):
        headers = {
            "Content-Type": self.headers.get("Content-Type", "application/json"),
            # Set (never appended) so clients cannot spoof the address the
            # backend uses to rate-limit logins
            "X-Forwarded-For": self.client_address[0],
        }
        if self.headers.get("Cookie"):
            headers["Cookie"] = self.headers["Cookie"]
        return headers

    def _proxy(self):
        length = int(self.headers.get("Content-Length") or 0)

        if length > MAX_BODY:
            self._send_json(413, {"detail": "Request too large"})
            return

        body = self.rfile.read(length) if length else None

        request = urllib.request.Request(
            self.backend + self.path,
            data=body,
            method=self.command,
            headers=self._forward_headers(),
        )

        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                status = response.status
                data = response.read()
                content_type = response.headers.get("Content-Type", "application/json")
                disposition = response.headers.get("Content-Disposition")
                cookies = response.headers.get_all("Set-Cookie") or []
        except urllib.error.HTTPError as err:
            status = err.code
            data = err.read()
            content_type = err.headers.get("Content-Type", "application/json")
            disposition = None
            cookies = []
        except (urllib.error.URLError, TimeoutError, ConnectionError) as err:
            reason = getattr(err, "reason", err)
            self._send_json(502, {"detail": f"Server unreachable ({reason}). Is the SSH tunnel running?"})
            return

        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if disposition:
            self.send_header("Content-Disposition", disposition)
        for cookie in cookies:
            self.send_header("Set-Cookie", cookie)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self._is_proxied():
            self._proxy()
        else:
            super().do_GET()

    def do_POST(self):
        if self.path.startswith("/api/"):
            self._proxy()
        else:
            self._send_json(405, {"detail": "Method not allowed"})


def main():
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--backend", default="http://127.0.0.1:8000")
    args = parser.parse_args()

    AppHandler.backend = args.backend.rstrip("/")

    server = ThreadingHTTPServer((args.bind, args.port), AppHandler)
    print(f"Serving app on http://{args.bind}:{args.port}  ->  API {AppHandler.backend}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
