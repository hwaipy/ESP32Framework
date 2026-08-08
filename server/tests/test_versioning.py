import pytest

from app.versioning import compare_versions, ota_action, parse_version


@pytest.mark.parametrize(
    ("left", "right", "expected"),
    [
        ("0.1.1", "0.1.0", 1),
        ("1.0.0", "1.0.0", 0),
        ("1.0.0-alpha", "1.0.0", -1),
        ("1.0.0-beta.2", "1.0.0-beta.11", -1),
        ("super_sonic_cleaner_presser_0.1.1", "super_sonic_cleaner_presser_0.1.0", 1),
    ],
)
def test_compare_versions(left, right, expected):
    assert compare_versions(left, right) == expected


def test_ota_action_supports_downgrade():
    assert ota_action("1.2.0", "1.3.0") == "update"
    assert ota_action("1.2.0", "1.1.0") == "downgrade"
    assert ota_action("1.2.0", "1.2.0") == "none"
    assert ota_action("1.2.0", None) == "none"
    assert ota_action(
        "super_sonic_cleaner_presser_0.1.0",
        "super_sonic_cleaner_presser_0.1.1",
    ) == "update"


def test_version_requires_semver():
    with pytest.raises(ValueError):
        parse_version("latest")


def test_prefixed_version_keeps_app_identity():
    version = parse_version("super_sonic_cleaner_presser_0.1.0")
    assert version.prefix == "super_sonic_cleaner_presser"
    assert version.core == (0, 1, 0)
