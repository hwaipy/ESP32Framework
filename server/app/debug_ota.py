from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .versioning import parse_version


@dataclass(frozen=True)
class DebugTarget:
    version: str
    build: str | None
    firmware_path: Path
    firmware_size: int
    firmware_sha256: str


def load_debug_target(data_dir: Path, model: str, device_id: str) -> DebugTarget | None:
    """Load a per-device OTA target kept outside the published release database."""
    target_dir = data_dir / "debug" / model / device_id
    manifest_path = target_dir / "manifest.json"
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        version = str(document["version"])
        parse_version(version)
        build_value = document.get("build")
        build = str(build_value) if build_value is not None else None
        firmware_size = int(document["firmware_size"])
        firmware_sha256 = str(document["firmware_sha256"]).lower()
        filename = str(document.get("filename", "firmware.bin"))
    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError):
        return None

    if Path(filename).name != filename:
        return None
    if firmware_size <= 0 or len(firmware_sha256) != 64:
        return None
    if any(character not in "0123456789abcdef" for character in firmware_sha256):
        return None

    firmware_path = target_dir / filename
    try:
        if not firmware_path.is_file() or firmware_path.stat().st_size != firmware_size:
            return None
    except OSError:
        return None

    return DebugTarget(
        version=version,
        build=build,
        firmware_path=firmware_path,
        firmware_size=firmware_size,
        firmware_sha256=firmware_sha256,
    )
