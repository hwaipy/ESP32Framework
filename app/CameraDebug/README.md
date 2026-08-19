# ESP32-S3 USB camera benchmark firmware

This is an isolated OTA debug application for device `2884856b37c8`. It uses
Espressif's USB Host UVC driver, enumerates every MJPEG format offered by the
attached camera, and benchmarks viable resolution/FPS combinations. Telemetry
and sampled JPEG frames are posted to `grayfog.chat:50000`.

The firmware is staged only through `tools/stage_debug_ota.py`; it is not added
to the release firmware database.

Current version: `camera_debug_0.1.2` (build `20260819.1`), embedding base
`0.4.1`.
