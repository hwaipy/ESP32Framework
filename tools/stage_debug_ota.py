#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shlex
import subprocess
import time
from pathlib import Path


REMOTE = "Code"
CONTAINER = "hwaipy-ota"
CONTROL_PATH = "/tmp/codex-esp32-code-ssh"


def ensure_master() -> None:
    check = subprocess.run(
        ["ssh", "-S", CONTROL_PATH, "-O", "check", REMOTE],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if check.returncode == 0:
        return
    for attempt in range(1, 6):
        result = subprocess.run(
            [
                "ssh",
                "-MNf",
                "-o", "ControlMaster=yes",
                "-o", f"ControlPath={CONTROL_PATH}",
                "-o", "ControlPersist=600",
                "-o", "ConnectTimeout=10",
                REMOTE,
            ]
        )
        if result.returncode == 0:
            return
        if attempt < 5:
            time.sleep(8)
    raise RuntimeError("unable to establish reusable SSH connection")


def run_remote(command: str, *, input_text: str | None = None) -> None:
    for attempt in range(1, 6):
        result = subprocess.run(
            ["ssh", "-o", f"ControlPath={CONTROL_PATH}", REMOTE, command],
            input=input_text,
            text=True,
        )
        if result.returncode == 0:
            return
        if attempt < 5:
            time.sleep(3)
    raise RuntimeError("remote command failed after 5 attempts")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", type=Path)
    parser.add_argument("version")
    parser.add_argument("build")
    parser.add_argument("--model", default="esp32-s3-supermini")
    parser.add_argument("--device-id", default="2884856b37c8")
    args = parser.parse_args()

    firmware = args.firmware.resolve()
    content = firmware.read_bytes()
    digest = hashlib.sha256(content).hexdigest()
    remote_temporary = f"/tmp/{args.device_id}-{args.version}.bin"
    target_dir = f"/data/debug/{args.model}/{args.device_id}"
    manifest = json.dumps(
        {
            "version": args.version,
            "build": args.build,
            "filename": "firmware.bin",
            "firmware_size": len(content),
            "firmware_sha256": digest,
        },
        indent=2,
    ) + "\n"

    ensure_master()
    subprocess.run(
        [
            "rsync", "-az", "--timeout=30",
            "-e", f"ssh -o ControlPath={CONTROL_PATH}",
            str(firmware), f"{REMOTE}:{remote_temporary}",
        ],
        check=True,
    )
    run_remote(
        f"docker exec {CONTAINER} mkdir -p {shlex.quote(target_dir)} && "
        f"docker cp {shlex.quote(remote_temporary)} "
        f"{CONTAINER}:{shlex.quote(target_dir + '/firmware.bin.next')}"
    )
    activation = (
        "import os,pathlib,sys; "
        f"d=pathlib.Path({target_dir!r}); "
        "os.replace(d/'firmware.bin.next',d/'firmware.bin'); "
        "p=d/'manifest.json.next'; p.write_text(sys.stdin.read(),encoding='utf-8'); "
        "os.replace(p,d/'manifest.json')"
    )
    run_remote(
        f"docker exec -i {CONTAINER} python -c {shlex.quote(activation)}",
        input_text=manifest,
    )
    print(f"staged {args.version}: {len(content)} bytes, sha256={digest}")


if __name__ == "__main__":
    main()
