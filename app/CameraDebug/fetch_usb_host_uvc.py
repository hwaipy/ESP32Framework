from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # noqa: F821

COMMIT = "1fb14d33a942db7d1f7cae48b0b586d0cd21a3b3"
workspace_dir = Path(env.subst("$PROJECT_WORKSPACE_DIR"))  # type: ignore[name-defined]  # noqa: F821
library_dir = workspace_dir / "vendor" / "USBHostUVC"
marker = library_dir / ".source-commit"

if not marker.is_file() or marker.read_text(encoding="utf-8").strip() != COMMIT:
    temporary = workspace_dir / ".usb-host-uvc-fetch"
    shutil.rmtree(temporary, ignore_errors=True)
    subprocess.run(
        ["git", "clone", "--no-checkout", "https://github.com/espressif/esp-usb.git", str(temporary)],
        check=True,
    )
    subprocess.run(["git", "-C", str(temporary), "checkout", COMMIT, "--", "host/class/uvc/usb_host_uvc"], check=True)
    source = temporary / "host" / "class" / "uvc" / "usb_host_uvc"
    shutil.rmtree(library_dir, ignore_errors=True)
    library_dir.mkdir(parents=True)
    for path in source.iterdir():
        if path.name in {"examples", "host_test", "test_app"}:
            continue
        destination = library_dir / path.name
        if path.is_dir():
            shutil.copytree(path, destination)
        else:
            shutil.copy2(path, destination)
    (library_dir / "library.json").write_text(
        json.dumps(
            {
                "name": "USBHostUVC",
                "version": "2.5.1-debug",
                "build": {
                    "includeDir": "include",
                    "flags": ["-Iprivate_include", "-Iinclude/esp_private"],
                    "srcFilter": ["+<*.c>"],
                },
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    marker.write_text(COMMIT + "\n", encoding="utf-8")
    shutil.rmtree(temporary, ignore_errors=True)
