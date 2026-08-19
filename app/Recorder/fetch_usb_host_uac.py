from __future__ import annotations

import io
import json
import urllib.request
import zipfile
from pathlib import Path


Import("env")


VERSION = "1.5.0"
PATCH_LEVEL = "agc-control-1"
DOWNLOAD_URL = (
    "https://components.espressif.com/api/downloads/"
    "?object_type=component&object_id=e73bc72f-923c-4777-93e3-511e941c6ce9"
)
workspace_dir = Path(env.subst("$PROJECT_BUILD_DIR")).parent
library_dir = workspace_dir / "vendor" / "USBHostUAC"
version_file = library_dir / ".version"
installed_version = f"{VERSION}+{PATCH_LEVEL}"

if (
    not version_file.is_file()
    or version_file.read_text(encoding="utf-8").strip() != installed_version
):
    print(f"Fetching Espressif USB Host UAC {VERSION}")
    with urllib.request.urlopen(DOWNLOAD_URL, timeout=60) as response:
        archive = zipfile.ZipFile(io.BytesIO(response.read()))
    wanted = {
        "usb_host_uac/uac_host.c": "uac_host.c",
        "usb_host_uac/uac_descriptors.c": "uac_descriptors.c",
        "usb_host_uac/include/usb/uac.h": "include/usb/uac.h",
        "usb_host_uac/include/usb/uac_host.h": "include/usb/uac_host.h",
        "usb_host_uac/LICENSE": "LICENSE",
    }
    for source, destination in wanted.items():
        output = library_dir / destination
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(archive.read(source))
    (library_dir / "library.json").write_text(
        json.dumps(
            {
                "name": "USBHostUAC",
                "version": VERSION,
                "build": {
                    "includeDir": "include",
                    "srcDir": ".",
                    "srcFilter": ["+<uac_host.c>", "+<uac_descriptors.c>"],
                },
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    host_source = library_dir / "uac_host.c"
    host_header = library_dir / "include/usb/uac_host.h"
    source = host_source.read_text(encoding="utf-8")
    header = host_header.read_text(encoding="utf-8")

    replacements = [
        (
            "    uint8_t mute_ch_map;                       /*!< mute channel map */\n",
            "    uint8_t mute_ch_map;                       /*!< mute channel map */\n"
            "    uint8_t agc_ch_map;                        /*!< automatic gain control channel map */\n",
        ),
        (
            "                        if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_MUTE) {\n"
            "                            iface_alt->mute_ch_map |= (1 << ch_num);\n"
            "                        }\n",
            "                        if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_MUTE) {\n"
            "                            iface_alt->mute_ch_map |= (1 << ch_num);\n"
            "                        }\n"
            "                        if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_AUTOMATIC_GAIN) {\n"
            "                            iface_alt->agc_ch_map |= (1 << ch_num);\n"
            "                        }\n",
        ),
        (
            "esp_err_t uac_host_install(const uac_host_driver_config_t *config)\n",
            "static esp_err_t uac_cs_request_set_agc(uac_iface_t *iface, bool enabled)\n"
            "{\n"
            "    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;\n"
            "    uint8_t agc_ch_map = iface->iface_alt[iface->cur_alt].agc_ch_map;\n"
            "    UAC_RETURN_ON_FALSE(feature_unit && agc_ch_map, ESP_ERR_NOT_SUPPORTED, \"automatic gain control not supported\");\n"
            "    uint8_t value = enabled;\n"
            "    uac_cs_request_t request = {\n"
            "        .bRequest = UAC_SET_CUR,\n"
            "        .wIndex = (feature_unit << 8) | (iface->parent->ctrl_iface_num & 0xff),\n"
            "        .wLength = 1,\n"
            "        .data = &value,\n"
            "    };\n"
            "    for (size_t channel = 0; channel < 8; channel++) {\n"
            "        if (agc_ch_map & (1 << channel)) {\n"
            "            request.wValue = (UAC_AUTOMATIC_GAIN_CONTROL << 8) | channel;\n"
            "            UAC_RETURN_ON_ERROR(uac_cs_request_set(iface->parent, &request), \"Unable to set automatic gain control\");\n"
            "        }\n"
            "    }\n"
            "    return ESP_OK;\n"
            "}\n\n"
            "static esp_err_t uac_cs_request_get_agc(uac_iface_t *iface, bool *enabled)\n"
            "{\n"
            "    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;\n"
            "    uint8_t agc_ch_map = iface->iface_alt[iface->cur_alt].agc_ch_map;\n"
            "    UAC_RETURN_ON_FALSE(feature_unit && agc_ch_map, ESP_ERR_NOT_SUPPORTED, \"automatic gain control not supported\");\n"
            "    uint8_t value = 0;\n"
            "    uac_cs_request_t request = {\n"
            "        .bRequest = UAC_GET_CUR,\n"
            "        .wIndex = (feature_unit << 8) | (iface->parent->ctrl_iface_num & 0xff),\n"
            "        .wLength = 1,\n"
            "        .data = &value,\n"
            "    };\n"
            "    for (size_t channel = 0; channel < 8; channel++) {\n"
            "        if (agc_ch_map & (1 << channel)) {\n"
            "            request.wValue = (UAC_AUTOMATIC_GAIN_CONTROL << 8) | channel;\n"
            "            break;\n"
            "        }\n"
            "    }\n"
            "    size_t actual_length = 0;\n"
            "    UAC_RETURN_ON_ERROR(uac_cs_request_get(iface->parent, &request, &actual_length), \"Unable to get automatic gain control\");\n"
            "    UAC_RETURN_ON_FALSE(actual_length == 1, ESP_ERR_INVALID_RESPONSE, \"Invalid automatic gain control response\");\n"
            "    *enabled = value != 0;\n"
            "    return ESP_OK;\n"
            "}\n\n"
            "esp_err_t uac_host_install(const uac_host_driver_config_t *config)\n",
        ),
        (
            "esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume)\n",
            "esp_err_t uac_host_device_set_agc(uac_host_device_handle_t uac_dev_handle, bool enabled)\n"
            "{\n"
            "    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);\n"
            "    UAC_RETURN_ON_INVALID_ARG(iface);\n"
            "    esp_err_t ret = ESP_OK;\n"
            "    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), \"Unable to lock UAC Interface\");\n"
            "    UAC_GOTO_ON_FALSE(UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state,\n"
            "                      ESP_ERR_INVALID_STATE, \"device not ready or active\");\n"
            "    UAC_GOTO_ON_ERROR(uac_cs_request_set_agc(iface, enabled), \"Unable to set automatic gain control\");\n"
            "    uac_host_interface_unlock(iface);\n"
            "    return ESP_OK;\n"
            "fail:\n"
            "    uac_host_interface_unlock(iface);\n"
            "    return ret;\n"
            "}\n\n"
            "esp_err_t uac_host_device_get_agc(uac_host_device_handle_t uac_dev_handle, bool *enabled)\n"
            "{\n"
            "    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);\n"
            "    UAC_RETURN_ON_INVALID_ARG(iface);\n"
            "    UAC_RETURN_ON_INVALID_ARG(enabled);\n"
            "    esp_err_t ret = ESP_OK;\n"
            "    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), \"Unable to lock UAC Interface\");\n"
            "    UAC_GOTO_ON_FALSE(UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state,\n"
            "                      ESP_ERR_INVALID_STATE, \"device not ready or active\");\n"
            "    UAC_GOTO_ON_ERROR(uac_cs_request_get_agc(iface, enabled), \"Unable to get automatic gain control\");\n"
            "    uac_host_interface_unlock(iface);\n"
            "    return ESP_OK;\n"
            "fail:\n"
            "    uac_host_interface_unlock(iface);\n"
            "    return ret;\n"
            "}\n\n"
            "esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume)\n",
        ),
    ]
    for old, new in replacements:
        if old not in source:
            raise RuntimeError(f"USB Host UAC {VERSION} patch context not found")
        source = source.replace(old, new, 1)

    header_marker = "esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume);\n"
    if header_marker not in header:
        raise RuntimeError(f"USB Host UAC {VERSION} header patch context not found")
    header = header.replace(
        header_marker,
        "/** Set or read the UAC Feature Unit automatic gain control. */\n"
        "esp_err_t uac_host_device_set_agc(uac_host_device_handle_t uac_dev_handle, bool enabled);\n"
        "esp_err_t uac_host_device_get_agc(uac_host_device_handle_t uac_dev_handle, bool *enabled);\n\n"
        + header_marker,
        1,
    )
    host_source.write_text(source, encoding="utf-8")
    host_header.write_text(header, encoding="utf-8")
    version_file.write_text(installed_version + "\n", encoding="utf-8")
