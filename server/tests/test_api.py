from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path

from fastapi.testclient import TestClient


TEST_DATA_DIR = Path(tempfile.mkdtemp(prefix="hwaipy-ota-test-"))
os.environ["DATA_DIR"] = str(TEST_DATA_DIR)
os.environ["PUBLIC_BASE_URL"] = "https://ota.hwaipy.cn"
os.environ["MANAGE_USERNAME"] = "tester"
os.environ["MANAGE_PASSWORD"] = "test-password"

from app.main import app  # noqa: E402


AUTH = ("tester", "test-password")
MODEL = "esp32-s3-supermini"
DEVICE_ID = "2884856b37c8"


def heartbeat(client: TestClient, version: str = "0.1.0"):
    return client.get(
        f"/{MODEL}/{DEVICE_ID}/hb",
        params={
            "v": version,
            "build": "20260806.1",
            "uptime": 120,
            "status": "ok",
            "rssi": -48,
            "heap": 372312,
            "reset": "power-on",
            "ota": "idle",
        },
    )


def publish(client: TestClient, version: str, content: bytes):
    return client.post(
        "/api/manage/releases",
        auth=AUTH,
        data={
            "model": MODEL,
            "version": version,
            "build": f"build-{version}",
            "notes": f"Release {version}",
        },
        files={"firmware": ("firmware.bin", content, "application/octet-stream")},
    )


def test_complete_update_and_downgrade_flow():
    with TestClient(app) as client:
        first_heartbeat = heartbeat(client)
        assert first_heartbeat.status_code == 200
        assert first_heartbeat.json()["action"] == "none"
        assert first_heartbeat.json()["ota_url"] is None
        assert first_heartbeat.json()["heartbeat_interval"] == 20

        unauthorized = client.get("/api/manage/snapshot")
        assert unauthorized.status_code == 401

        firmware_011 = b"ESP32-S3 firmware version 0.1.1"
        release = publish(client, "0.1.1", firmware_011)
        assert release.status_code == 201
        assert release.json()["sha256"] == hashlib.sha256(firmware_011).hexdigest()

        duplicate = publish(client, "0.1.1", b"different firmware")
        assert duplicate.status_code == 409

        target = client.put(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}/target",
            auth=AUTH,
            json={"target_version": "0.1.1"},
        )
        assert target.status_code == 200

        update_heartbeat = heartbeat(client, "0.1.0")
        update_body = update_heartbeat.json()
        assert update_body["action"] == "update"
        assert update_body["target_version"] == "0.1.1"
        assert update_body["ota_url"] == (
            f"https://ota.hwaipy.cn/{MODEL}/{DEVICE_ID}/bin/0.1.1"
        )
        assert update_body["firmware_sha256"] == hashlib.sha256(firmware_011).hexdigest()

        firmware_download = client.get(f"/{MODEL}/{DEVICE_ID}/bin/0.1.1")
        assert firmware_download.status_code == 200
        assert firmware_download.content == firmware_011
        assert firmware_download.headers["x-firmware-version"] == "0.1.1"

        firmware_009 = b"ESP32-S3 firmware version 0.0.9"
        assert publish(client, "0.0.9", firmware_009).status_code == 201
        assert client.put(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}/target",
            auth=AUTH,
            json={"target_version": "0.0.9"},
        ).status_code == 200

        downgrade_heartbeat = heartbeat(client, "0.1.0")
        assert downgrade_heartbeat.json()["action"] == "downgrade"
        assert downgrade_heartbeat.json()["ota_url"].endswith("/bin/0.0.9")

        snapshot = client.get("/api/manage/snapshot", auth=AUTH)
        assert snapshot.status_code == 200
        assert snapshot.json()["counts"]["total"] == 1
        assert snapshot.json()["counts"]["pending"] == 1
        assert len(snapshot.json()["releases"]) == 2


def test_input_validation_and_assignment_guards():
    with TestClient(app) as client:
        invalid_id = client.get(f"/{MODEL}/not-a-mac/hb", params={"v": "0.1.0"})
        assert invalid_id.status_code == 422

        invalid_version = client.get(f"/{MODEL}/{DEVICE_ID}/hb", params={"v": "latest"})
        assert invalid_version.status_code == 422

        heartbeat(client)
        missing_release = client.put(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}/target",
            auth=AUTH,
            json={"target_version": "9.9.9"},
        )
        assert missing_release.status_code == 404

        unassigned_download = client.get(f"/{MODEL}/{DEVICE_ID}/bin/9.9.9")
        assert unassigned_download.status_code == 403


def test_device_alias_is_persistent_and_can_be_cleared():
    with TestClient(app) as client:
        manage_page = client.get("/manage", auth=AUTH)
        assert manage_page.status_code == 200
        assert manage_page.headers["cache-control"] == "no-store"
        assert "/assets/manage.js?v=20260808-alias-icon" in manage_page.text

        manage_script = client.get("/assets/manage.js?v=20260808-alias-icon")
        assert manage_script.status_code == 200
        assert 'class="alias-icon-button edit-alias"' in manage_script.text
        assert "设置设备别名" in manage_script.text

        assert heartbeat(client).status_code == 200

        unauthorized = client.patch(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}",
            json={"name": "超声波清洗机"},
        )
        assert unauthorized.status_code == 401

        renamed = client.patch(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}",
            auth=AUTH,
            json={"name": "  超声波清洗机  "},
        )
        assert renamed.status_code == 200
        assert renamed.json()["name"] == "超声波清洗机"

        assert heartbeat(client, "0.1.1").status_code == 200
        snapshot = client.get("/api/manage/snapshot", auth=AUTH)
        assert snapshot.status_code == 200
        device = next(
            item
            for item in snapshot.json()["devices"]
            if item["model"] == MODEL and item["device_id"] == DEVICE_ID
        )
        assert device["name"] == "超声波清洗机"

        too_long = client.patch(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}",
            auth=AUTH,
            json={"name": "别" * 81},
        )
        assert too_long.status_code == 422

        cleared = client.patch(
            f"/api/manage/devices/{MODEL}/{DEVICE_ID}",
            auth=AUTH,
            json={"name": "   "},
        )
        assert cleared.status_code == 200
        assert cleared.json()["name"] is None
