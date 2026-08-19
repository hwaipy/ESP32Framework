#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


DEVICE_PATTERN = re.compile(r"^[0-9a-f]{12}$")
MAX_LOG_BYTES = 1024 * 1024
MAX_RECORDING_BYTES = 32 * 1024 * 1024
MAX_FRAME_BYTES = 2 * 1024 * 1024


def utc_timestamp() -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


class DebugServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], storage_dir: Path):
        super().__init__(address, DebugHandler)
        self.storage_dir = storage_dir
        storage_dir.mkdir(parents=True, exist_ok=True)


class DebugHandler(BaseHTTPRequestHandler):
    server: DebugServer

    def send_json(self, status: HTTPStatus, document: dict[str, object]) -> None:
        body = json.dumps(document, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def device_route(self) -> tuple[str, str] | None:
        parts = urlsplit(self.path).path.strip("/").split("/")
        if len(parts) != 3 or parts[0] != "devices":
            return None
        device_id, resource = parts[1], parts[2]
        if not DEVICE_PATTERN.fullmatch(device_id):
            return None
        return device_id, resource

    def read_body(self, maximum: int) -> bytes | None:
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            return None
        if length < 0 or length > maximum:
            return None
        return self.rfile.read(length)

    def do_GET(self) -> None:
        if urlsplit(self.path).path == "/health":
            self.send_json(HTTPStatus.OK, {"status": "ok", "time": utc_timestamp()})
            return
        route = self.device_route()
        if route is not None and route[1] == "frame":
            frame_path = self.server.storage_dir / route[0] / "latest.jpg"
            if not frame_path.is_file():
                self.send_json(HTTPStatus.NOT_FOUND, {"error": "no frame"})
                return
            body = frame_path.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if route is not None and route[1] == "status":
            device_dir = self.server.storage_dir / route[0]
            recordings = sorted(
                (path.name for path in (device_dir / "recordings").glob("*.wav")),
                reverse=True,
            ) if device_dir.exists() else []
            self.send_json(
                HTTPStatus.OK,
                {
                    "device_id": route[0],
                    "recording_count": len(recordings),
                    "latest_recording": recordings[0] if recordings else None,
                    "log_exists": (device_dir / "events.ndjson").is_file(),
                    "frame_exists": (device_dir / "latest.jpg").is_file(),
                },
            )
            return
        self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:
        route = self.device_route()
        if route is None:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        device_id, resource = route
        device_dir = self.server.storage_dir / device_id
        device_dir.mkdir(parents=True, exist_ok=True)

        if resource == "log":
            body = self.read_body(MAX_LOG_BYTES)
            if body is None:
                self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "invalid body"})
                return
            event = {
                "received_at": utc_timestamp(),
                "remote_ip": self.client_address[0],
                "content_type": self.headers.get("Content-Type"),
                "message": body.decode("utf-8", "replace"),
            }
            with (device_dir / "events.ndjson").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(event, ensure_ascii=False) + "\n")
            self.send_json(HTTPStatus.CREATED, {"ok": True})
            return

        if resource == "recording":
            body = self.read_body(MAX_RECORDING_BYTES)
            if body is None:
                self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "invalid body"})
                return
            if len(body) < 44 or body[:4] != b"RIFF" or body[8:12] != b"WAVE":
                self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": "WAV required"})
                return
            recording_dir = device_dir / "recordings"
            recording_dir.mkdir(parents=True, exist_ok=True)
            sequence = time.time_ns()
            filename = f"{utc_timestamp()}-{sequence}.wav"
            temporary = recording_dir / f".{filename}.part"
            temporary.write_bytes(body)
            temporary.replace(recording_dir / filename)
            self.send_json(
                HTTPStatus.CREATED,
                {"ok": True, "filename": filename, "bytes": len(body)},
            )
            return

        if resource == "frame":
            body = self.read_body(MAX_FRAME_BYTES)
            if body is None:
                self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "invalid body"})
                return
            if (len(body) < 4 or body[:2] != b"\xff\xd8" or
                    body[-2:] != b"\xff\xd9"):
                self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": "JPEG required"})
                return
            temporary = device_dir / ".latest.jpg.part"
            temporary.write_bytes(body)
            temporary.replace(device_dir / "latest.jpg")
            self.send_json(HTTPStatus.CREATED, {"ok": True, "bytes": len(body)})
            return

        self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def log_message(self, message: str, *args: object) -> None:
        print(
            f"{utc_timestamp()} {self.client_address[0]} " + (message % args),
            flush=True,
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50000)
    parser.add_argument("--storage", type=Path, required=True)
    args = parser.parse_args()
    server = DebugServer((args.bind, args.port), args.storage.resolve())
    print(f"listening on {args.bind}:{args.port}, storage={server.storage_dir}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
