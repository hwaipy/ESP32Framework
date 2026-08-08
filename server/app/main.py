from __future__ import annotations

import hashlib
import os
import re
import secrets
from contextlib import asynccontextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import Annotated

from fastapi import (
    Depends,
    FastAPI,
    File,
    Form,
    HTTPException,
    Query,
    Request,
    UploadFile,
    status,
)
from fastapi.responses import FileResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .config import settings
from .db import connect, init_db, utc_now
from .versioning import (
    ota_action,
    parse_version,
    validate_device_id,
    validate_model,
)


STATIC_DIR = Path(__file__).parent / "static"
BUILD_PATTERN = re.compile(r"^[0-9A-Za-z._-]{1,64}$")
STATUS_PATTERN = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
HEARTBEAT_INTERVAL_SECONDS = 20


@asynccontextmanager
async def lifespan(_: FastAPI):
    init_db()
    yield


app = FastAPI(title="Hwaipy ESP32 OTA", version="0.1.0", lifespan=lifespan)
app.mount("/assets", StaticFiles(directory=STATIC_DIR), name="assets")
basic_security = HTTPBasic(auto_error=False)


class TargetUpdate(BaseModel):
    target_version: str | None


class DeviceUpdate(BaseModel):
    name: str | None


def require_admin(
    credentials: Annotated[HTTPBasicCredentials | None, Depends(basic_security)],
) -> str:
    if credentials is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="authentication required",
            headers={"WWW-Authenticate": "Basic"},
        )
    username_ok = secrets.compare_digest(credentials.username, settings.manage_username)
    password_ok = secrets.compare_digest(credentials.password, settings.manage_password)
    if not (username_ok and password_ok):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid credentials",
            headers={"WWW-Authenticate": "Basic"},
        )
    return credentials.username


def validated_identity(model: str, device_id: str) -> tuple[str, str]:
    try:
        return validate_model(model), validate_device_id(device_id)
    except ValueError as error:
        raise HTTPException(status_code=422, detail=str(error)) from error


def validated_version(version: str) -> str:
    try:
        parse_version(version)
    except ValueError as error:
        raise HTTPException(status_code=422, detail=str(error)) from error
    return version


def validate_runtime_value(value: str, field: str) -> str:
    if not STATUS_PATTERN.fullmatch(value):
        raise HTTPException(status_code=422, detail=f"invalid {field}")
    return value


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.get("/{model}/{device_id}/hb")
def heartbeat(
    request: Request,
    model: str,
    device_id: str,
    version: Annotated[str, Query(alias="v")],
    build: Annotated[str | None, Query(max_length=64)] = None,
    uptime: Annotated[int | None, Query(ge=0)] = None,
    device_status: Annotated[str, Query(alias="status")] = "ok",
    rssi: Annotated[int | None, Query(ge=-127, le=20)] = None,
    heap: Annotated[int | None, Query(ge=0)] = None,
    reset: Annotated[str | None, Query(max_length=32)] = None,
    ota: Annotated[str, Query()] = "idle",
) -> dict[str, object]:
    model, device_id = validated_identity(model, device_id)
    version = validated_version(version)
    device_status = validate_runtime_value(device_status, "status")
    ota = validate_runtime_value(ota, "ota status")
    if build is not None and not BUILD_PATTERN.fullmatch(build):
        raise HTTPException(status_code=422, detail="invalid build")
    if reset is not None and not STATUS_PATTERN.fullmatch(reset):
        raise HTTPException(status_code=422, detail="invalid reset reason")

    now = utc_now()
    remote_ip = request.client.host if request.client else None
    with connect() as database:
        database.execute(
            """
            INSERT INTO devices (
                model, device_id, current_version, current_build, status, ota_status,
                uptime_seconds, rssi, free_heap, reset_reason, remote_ip,
                first_seen, last_seen
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(model, device_id) DO UPDATE SET
                current_version = excluded.current_version,
                current_build = excluded.current_build,
                status = excluded.status,
                ota_status = excluded.ota_status,
                uptime_seconds = excluded.uptime_seconds,
                rssi = excluded.rssi,
                free_heap = excluded.free_heap,
                reset_reason = excluded.reset_reason,
                remote_ip = excluded.remote_ip,
                last_seen = excluded.last_seen
            """,
            (
                model,
                device_id,
                version,
                build,
                device_status,
                ota,
                uptime,
                rssi,
                heap,
                reset,
                remote_ip,
                now,
                now,
            ),
        )
        database.execute(
            """
            INSERT INTO heartbeats (
                model, device_id, version, build, status, ota_status,
                uptime_seconds, rssi, free_heap, reset_reason, remote_ip, received_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                model,
                device_id,
                version,
                build,
                device_status,
                ota,
                uptime,
                rssi,
                heap,
                reset,
                remote_ip,
                now,
            ),
        )
        device = database.execute(
            "SELECT target_version FROM devices WHERE model = ? AND device_id = ?",
            (model, device_id),
        ).fetchone()
        target_version = device["target_version"]
        release = None
        if target_version is not None:
            release = database.execute(
                """
                SELECT version, file_size, sha256
                FROM releases
                WHERE model = ? AND version = ?
                """,
                (model, target_version),
            ).fetchone()

    action = ota_action(version, target_version)
    if action != "none" and release is None:
        action = "none"
        target_version = None

    ota_url = None
    firmware_size = None
    firmware_sha256 = None
    if action != "none" and release is not None:
        ota_url = f"{settings.public_base_url}/{model}/{device_id}/bin/{target_version}"
        firmware_size = release["file_size"]
        firmware_sha256 = release["sha256"]

    return {
        "ok": True,
        "server_time": now,
        "current_version": version,
        "target_version": target_version,
        "action": action,
        "ota_url": ota_url,
        "firmware_size": firmware_size,
        "firmware_sha256": firmware_sha256,
        "heartbeat_interval": HEARTBEAT_INTERVAL_SECONDS,
    }


@app.get("/{model}/{device_id}/bin/{version}")
def download_firmware(model: str, device_id: str, version: str) -> FileResponse:
    model, device_id = validated_identity(model, device_id)
    version = validated_version(version)
    with connect() as database:
        device = database.execute(
            "SELECT target_version FROM devices WHERE model = ? AND device_id = ?",
            (model, device_id),
        ).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="unknown device")
        if device["target_version"] != version:
            raise HTTPException(status_code=403, detail="version is not assigned to this device")
        release = database.execute(
            """
            SELECT file_path, file_size, sha256, build
            FROM releases
            WHERE model = ? AND version = ?
            """,
            (model, version),
        ).fetchone()
    if release is None:
        raise HTTPException(status_code=404, detail="firmware not found")
    firmware_path = Path(release["file_path"])
    if not firmware_path.is_file():
        raise HTTPException(status_code=410, detail="firmware file is unavailable")

    headers = {
        "Cache-Control": "no-store",
        "X-Firmware-Version": version,
        "X-Firmware-SHA256": release["sha256"],
    }
    if release["build"]:
        headers["X-Firmware-Build"] = release["build"]
    return FileResponse(
        firmware_path,
        media_type="application/octet-stream",
        filename=f"{model}-{version}.bin",
        headers=headers,
    )


@app.get("/manage", include_in_schema=False)
def manage_page(_: Annotated[str, Depends(require_admin)]) -> FileResponse:
    return FileResponse(
        STATIC_DIR / "manage.html",
        media_type="text/html",
        headers={"Cache-Control": "no-store"},
    )


@app.get("/api/manage/snapshot")
def manage_snapshot(_: Annotated[str, Depends(require_admin)]) -> dict[str, object]:
    with connect() as database:
        devices = [
            dict(row)
            for row in database.execute(
                """
                SELECT * FROM devices
                ORDER BY last_seen DESC, model, device_id
                """
            ).fetchall()
        ]
        releases = [
            dict(row)
            for row in database.execute(
                """
                SELECT model, version, build, file_size, sha256, notes, created_at
                FROM releases
                ORDER BY model, created_at DESC
                """
            ).fetchall()
        ]

    now = datetime.now(UTC)
    counts = {"total": len(devices), "online": 0, "delayed": 0, "offline": 0, "pending": 0}
    for device in devices:
        age = (now - datetime.fromisoformat(device["last_seen"])).total_seconds()
        if age <= 150:
            presence = "online"
        elif age <= 3600:
            presence = "delayed"
        else:
            presence = "offline"
        device["presence"] = presence
        device["last_seen_age_seconds"] = max(0, int(age))
        device["version_matches"] = (
            device["target_version"] is None
            or device["target_version"] == device["current_version"]
        )
        counts[presence] += 1
        if not device["version_matches"]:
            counts["pending"] += 1

    return {"generated_at": utc_now(), "counts": counts, "devices": devices, "releases": releases}


@app.post("/api/manage/releases", status_code=201)
def publish_release(
    _: Annotated[str, Depends(require_admin)],
    model: Annotated[str, Form()],
    version: Annotated[str, Form()],
    build: Annotated[str | None, Form()] = None,
    notes: Annotated[str | None, Form(max_length=1000)] = None,
    firmware: UploadFile = File(),
) -> dict[str, object]:
    try:
        model = validate_model(model)
    except ValueError as error:
        raise HTTPException(status_code=422, detail=str(error)) from error
    version = validated_version(version)
    if build is not None and not BUILD_PATTERN.fullmatch(build):
        raise HTTPException(status_code=422, detail="invalid build")

    with connect() as database:
        existing = database.execute(
            "SELECT 1 FROM releases WHERE model = ? AND version = ?",
            (model, version),
        ).fetchone()
    if existing is not None:
        raise HTTPException(status_code=409, detail="this model and version already exists")

    release_dir = settings.firmware_dir / model / version
    release_dir.mkdir(parents=True, exist_ok=True)
    temporary_path = release_dir / "firmware.bin.upload"
    final_path = release_dir / "firmware.bin"
    digest = hashlib.sha256()
    file_size = 0
    with temporary_path.open("wb") as output:
        while chunk := firmware.file.read(1024 * 1024):
            file_size += len(chunk)
            if file_size > 16 * 1024 * 1024:
                output.close()
                temporary_path.unlink(missing_ok=True)
                raise HTTPException(status_code=413, detail="firmware exceeds 16 MiB")
            digest.update(chunk)
            output.write(chunk)
    if file_size == 0:
        temporary_path.unlink(missing_ok=True)
        raise HTTPException(status_code=422, detail="firmware is empty")
    os.replace(temporary_path, final_path)
    sha256 = digest.hexdigest()
    created_at = utc_now()

    try:
        with connect() as database:
            database.execute(
                """
                INSERT INTO releases (
                    model, version, build, file_path, file_size, sha256, notes, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    model,
                    version,
                    build,
                    str(final_path),
                    file_size,
                    sha256,
                    notes,
                    created_at,
                ),
            )
    except Exception:
        final_path.unlink(missing_ok=True)
        raise

    return {
        "model": model,
        "version": version,
        "build": build,
        "file_size": file_size,
        "sha256": sha256,
        "created_at": created_at,
    }


@app.put("/api/manage/devices/{model}/{device_id}/target")
def set_device_target(
    model: str,
    device_id: str,
    update: TargetUpdate,
    _: Annotated[str, Depends(require_admin)],
) -> dict[str, object]:
    model, device_id = validated_identity(model, device_id)
    target_version = update.target_version
    if target_version is not None:
        target_version = validated_version(target_version)
    with connect() as database:
        device = database.execute(
            "SELECT 1 FROM devices WHERE model = ? AND device_id = ?",
            (model, device_id),
        ).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="device not found")
        if target_version is not None:
            release = database.execute(
                "SELECT 1 FROM releases WHERE model = ? AND version = ?",
                (model, target_version),
            ).fetchone()
            if release is None:
                raise HTTPException(status_code=404, detail="release not found for this model")
        database.execute(
            "UPDATE devices SET target_version = ? WHERE model = ? AND device_id = ?",
            (target_version, model, device_id),
        )
    return {"ok": True, "model": model, "device_id": device_id, "target_version": target_version}


@app.patch("/api/manage/devices/{model}/{device_id}")
def update_device(
    model: str,
    device_id: str,
    update: DeviceUpdate,
    _: Annotated[str, Depends(require_admin)],
) -> dict[str, object]:
    model, device_id = validated_identity(model, device_id)
    stripped_name = update.name.strip() if update.name else ""
    name = stripped_name or None
    if name is not None and len(name) > 80:
        raise HTTPException(status_code=422, detail="name is too long")
    with connect() as database:
        cursor = database.execute(
            "UPDATE devices SET name = ? WHERE model = ? AND device_id = ?",
            (name, model, device_id),
        )
        if cursor.rowcount == 0:
            raise HTTPException(status_code=404, detail="device not found")
    return {"ok": True, "model": model, "device_id": device_id, "name": name}
