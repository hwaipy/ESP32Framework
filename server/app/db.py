from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import Iterator

from .config import settings


SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    model TEXT NOT NULL,
    device_id TEXT NOT NULL,
    name TEXT,
    current_version TEXT NOT NULL,
    current_build TEXT,
    target_version TEXT,
    status TEXT NOT NULL,
    ota_status TEXT NOT NULL,
    uptime_seconds INTEGER,
    rssi INTEGER,
    free_heap INTEGER,
    reset_reason TEXT,
    remote_ip TEXT,
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL,
    PRIMARY KEY (model, device_id)
);

CREATE TABLE IF NOT EXISTS releases (
    model TEXT NOT NULL,
    version TEXT NOT NULL,
    build TEXT,
    file_path TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    notes TEXT,
    created_at TEXT NOT NULL,
    PRIMARY KEY (model, version),
    UNIQUE (model, sha256)
);

CREATE TABLE IF NOT EXISTS heartbeats (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    model TEXT NOT NULL,
    device_id TEXT NOT NULL,
    version TEXT NOT NULL,
    build TEXT,
    status TEXT NOT NULL,
    ota_status TEXT NOT NULL,
    uptime_seconds INTEGER,
    rssi INTEGER,
    free_heap INTEGER,
    reset_reason TEXT,
    remote_ip TEXT,
    received_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_devices_last_seen
ON devices(last_seen);

CREATE INDEX IF NOT EXISTS idx_heartbeats_device_received
ON heartbeats(model, device_id, received_at DESC);

CREATE INDEX IF NOT EXISTS idx_releases_model_created
ON releases(model, created_at DESC);
"""


def utc_now() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


def init_db(database_path: Path | None = None) -> None:
    path = database_path or settings.database_path
    path.parent.mkdir(parents=True, exist_ok=True)
    settings.firmware_dir.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as connection:
        connection.executescript(SCHEMA)
        connection.execute("PRAGMA optimize")


@contextmanager
def connect(database_path: Path | None = None) -> Iterator[sqlite3.Connection]:
    path = database_path or settings.database_path
    connection = sqlite3.connect(path, timeout=10)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    connection.execute("PRAGMA journal_mode = WAL")
    try:
        yield connection
        connection.commit()
    except Exception:
        connection.rollback()
        raise
    finally:
        connection.close()

