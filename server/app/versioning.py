from __future__ import annotations

import re
from dataclasses import dataclass


MODEL_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
DEVICE_ID_PATTERN = re.compile(r"^[0-9a-f]{12}$")
VERSION_PATTERN = re.compile(
    r"^(?:([a-z](?:[a-z0-9_]*[a-z0-9])?)_)?"
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?$"
)


@dataclass(frozen=True)
class ParsedVersion:
    prefix: str | None
    core: tuple[int, int, int]
    prerelease: tuple[str, ...] | None


def validate_model(value: str) -> str:
    if not MODEL_PATTERN.fullmatch(value):
        raise ValueError("invalid board model")
    return value


def validate_device_id(value: str) -> str:
    normalized = value.lower()
    if not DEVICE_ID_PATTERN.fullmatch(normalized):
        raise ValueError("device ID must be 12 lowercase hexadecimal characters")
    return normalized


def parse_version(value: str) -> ParsedVersion:
    match = VERSION_PATTERN.fullmatch(value)
    if match is None:
        raise ValueError(
            "version must be semantic, optionally with an app prefix, "
            "for example 0.1.0 or app_name_0.1.0"
        )
    prerelease = tuple(match.group(5).split(".")) if match.group(5) else None
    return ParsedVersion(
        prefix=match.group(1),
        core=(int(match.group(2)), int(match.group(3)), int(match.group(4))),
        prerelease=prerelease,
    )


def compare_versions(left: str, right: str) -> int:
    a = parse_version(left)
    b = parse_version(right)
    if a.core != b.core:
        return 1 if a.core > b.core else -1
    if a.prerelease is None and b.prerelease is None:
        return 0
    if a.prerelease is None:
        return 1
    if b.prerelease is None:
        return -1
    for left_part, right_part in zip(a.prerelease, b.prerelease):
        if left_part == right_part:
            continue
        left_numeric = left_part.isdigit()
        right_numeric = right_part.isdigit()
        if left_numeric and right_numeric:
            return 1 if int(left_part) > int(right_part) else -1
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return 1 if left_part > right_part else -1
    if len(a.prerelease) == len(b.prerelease):
        return 0
    return 1 if len(a.prerelease) > len(b.prerelease) else -1


def ota_action(current: str, target: str | None) -> str:
    if target is None or current == target:
        return "none"
    return "update" if compare_versions(target, current) > 0 else "downgrade"
