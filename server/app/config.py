from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    public_base_url: str
    data_dir: Path
    manage_username: str
    manage_password: str

    @property
    def database_path(self) -> Path:
        return self.data_dir / "ota.db"

    @property
    def firmware_dir(self) -> Path:
        return self.data_dir / "firmware"


def load_settings() -> Settings:
    return Settings(
        public_base_url=os.getenv("PUBLIC_BASE_URL", "https://ota.hwaipy.cn").rstrip("/"),
        data_dir=Path(os.getenv("DATA_DIR", "./data")).resolve(),
        manage_username=os.getenv("MANAGE_USERNAME", "admin"),
        manage_password=os.getenv("MANAGE_PASSWORD", "change-me"),
    )


settings = load_settings()

