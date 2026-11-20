"""Serve one OTA fixture on loopback for local_update_test.ps1."""

import argparse
import hashlib
import json
import re
import shutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("version")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("version must be major.minor.patch")
    package = args.package.resolve(strict=True)
    digest = hashlib.sha256()
    with package.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/stable.json":
                content = json.dumps(manifest).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            elif path == "/package.zip":
                with package.open("rb") as stream:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/zip")
                    self.send_header("Content-Length", str(package.stat().st_size))
                    self.end_headers()
                    shutil.copyfileobj(stream, self.wfile)
            else:
                self.send_error(404)

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        origin = f"http://127.0.0.1:{server.server_port}"
        manifest = {
            "schema": 1,
            "product": "QQCom-plus",
            "channel": "stable",
            "version": args.version,
            "mandatory": False,
            "download": {
                "type": "zip",
                "url": origin + "/package.zip",
                "size": package.stat().st_size,
                "sha256": digest.hexdigest(),
            },
        }
        print("ORIGIN " + origin, flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
