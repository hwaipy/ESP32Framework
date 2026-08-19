#include <Arduino.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>

#include "ota_root_ca.h"

SET_LOOP_TASK_STACK_SIZE(16384);

namespace {

#ifndef HWAIPY_FIRMWARE_NAME
#define HWAIPY_FIRMWARE_NAME "Hwaipy ESP32 OTA base"
#endif

#define HWAIPY_BASE_VERSION "0.4.1"

#ifndef HWAIPY_FIRMWARE_VERSION
#define HWAIPY_FIRMWARE_VERSION HWAIPY_BASE_VERSION
#endif

#ifndef HWAIPY_FIRMWARE_BUILD
#define HWAIPY_FIRMWARE_BUILD "20260819.2"
#endif

#ifndef HWAIPY_APP_SETUP
#define HWAIPY_APP_SETUP() ((void)0)
#endif

#if CONFIG_IDF_TARGET_ESP32S3
constexpr char DEVICE_MODEL[] = "esp32-s3-supermini";
#define HWAIPY_BOARD_HAS_PSRAM 1
#elif CONFIG_IDF_TARGET_ESP32C3
constexpr char DEVICE_MODEL[] = "esp32-c3-supermini";
#define HWAIPY_BOARD_HAS_PSRAM 0
#elif CONFIG_IDF_TARGET_ESP32C6
constexpr char DEVICE_MODEL[] = "esp32-c6-supermini";
#define HWAIPY_BOARD_HAS_PSRAM 0
#else
#error "Unsupported ESP32 target. Add its model and memory policy first."
#endif

constexpr char FIRMWARE_NAME[] = HWAIPY_FIRMWARE_NAME;
constexpr char FIRMWARE_VERSION[] = HWAIPY_FIRMWARE_VERSION;
constexpr char FIRMWARE_BUILD[] = HWAIPY_FIRMWARE_BUILD;
constexpr char BASE_VERSION[] = HWAIPY_BASE_VERSION;
constexpr char OTA_BASE_URL[] = "https://ota.hwaipy.cn";
constexpr uint32_t DEFAULT_HEARTBEAT_INTERVAL_MS = 60000;
constexpr uint32_t RETRY_HEARTBEAT_INTERVAL_MS = 15000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 20000;
constexpr uint32_t OTA_READ_TIMEOUT_MS = 30000;
constexpr size_t MAX_WIFI_NETWORKS = 8;

struct NetworkCredential {
  String ssid;
  String password;
};

String deviceId;
String otaStatus = "idle";
uint32_t heartbeatIntervalMs = DEFAULT_HEARTBEAT_INTERVAL_MS;
uint32_t nextHeartbeatAt = 0;
bool reportPendingOtaResult = false;
String serialLine;

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
      return "task-watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep-sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

String factoryDeviceId() {
  const uint64_t mac = ESP.getEfuseMac();
  char value[13];
  snprintf(value, sizeof(value), "%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned>(mac & 0xFF),
           static_cast<unsigned>((mac >> 8) & 0xFF),
           static_cast<unsigned>((mac >> 16) & 0xFF),
           static_cast<unsigned>((mac >> 24) & 0xFF),
           static_cast<unsigned>((mac >> 32) & 0xFF),
           static_cast<unsigned>((mac >> 40) & 0xFF));
  return String(value);
}

#if HWAIPY_BOARD_HAS_PSRAM
bool testPsram() {
  if (!psramFound()) {
    return false;
  }
  const size_t testSize = 64 * 1024;
  auto *buffer = static_cast<uint8_t *>(
      heap_caps_malloc(testSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    return false;
  }
  for (size_t index = 0; index < testSize; ++index) {
    buffer[index] = static_cast<uint8_t>((index * 37U + 11U) & 0xFFU);
  }
  bool passed = true;
  for (size_t index = 0; index < testSize; ++index) {
    if (buffer[index] != static_cast<uint8_t>((index * 37U + 11U) & 0xFFU)) {
      passed = false;
      break;
    }
  }
  heap_caps_free(buffer);
  return passed;
}
#endif

bool boardSelfTest() {
#if HWAIPY_BOARD_HAS_PSRAM
  return testPsram();
#else
  return true;
#endif
}

void printHardwareReport() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(FIRMWARE_NAME);
  Serial.println("========================================");
  Serial.printf("Model             : %s\n", DEVICE_MODEL);
  Serial.printf("Device ID         : %s\n", deviceId.c_str());
  Serial.printf("Firmware          : %s (%s)\n", FIRMWARE_VERSION, FIRMWARE_BUILD);
  Serial.printf("Base              : %s\n", BASE_VERSION);
  Serial.printf("Chip              : %s, %u cores, %u MHz\n", ESP.getChipModel(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash             : %u bytes, %u MHz\n", ESP.getFlashChipSize(),
                ESP.getFlashChipSpeed() / 1000000U);
#if HWAIPY_BOARD_HAS_PSRAM
  Serial.printf("PSRAM             : %u bytes, test %s\n", ESP.getPsramSize(),
                testPsram() ? "PASS" : "FAIL");
#else
  Serial.println("PSRAM             : not present (normal for this board)");
#endif
  Serial.printf("OTA slots         : %s\n", Update.canRollBack() ? "ready" : "waiting for first OTA");
  Serial.printf("Reset reason      : %s\n", resetReasonName(esp_reset_reason()));
  Serial.println("========================================");
}

void loadOtaResult() {
  Preferences preferences;
  if (!preferences.begin("hwaipy-ota", false)) {
    return;
  }
  const String pendingTarget = preferences.isKey("pending")
                                   ? preferences.getString("pending", "")
                                   : String();
  if (!pendingTarget.isEmpty() && pendingTarget == FIRMWARE_VERSION) {
    otaStatus = "success";
    reportPendingOtaResult = true;
  }
  preferences.end();
}

void clearOtaResult() {
  Preferences preferences;
  if (preferences.begin("hwaipy-ota", false)) {
    preferences.remove("pending");
    preferences.end();
  }
  reportPendingOtaResult = false;
  otaStatus = "idle";
}

void rememberOtaTarget(const String &targetVersion) {
  Preferences preferences;
  if (preferences.begin("hwaipy-ota", false)) {
    preferences.putString("pending", targetVersion);
    preferences.end();
  }
}

void confirmRunningImage() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (boardSelfTest()) {
      const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
      Serial.printf("OTA image confirmation: %s\n", esp_err_to_name(result));
    } else {
      Serial.println("Self-test failed; rolling back OTA image.");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
}

bool loadNetworkCredentials(NetworkCredential *networks, size_t &networkCount) {
  networkCount = 0;
  Preferences preferences;
  if (!preferences.begin("hwaipy-net", true)) {
    return false;
  }

  const String encodedNetworks = preferences.isKey("networks")
                                     ? preferences.getString("networks", "")
                                     : String();
  if (!encodedNetworks.isEmpty()) {
    JsonDocument document;
    if (!deserializeJson(document, encodedNetworks)) {
      const JsonArrayConst entries = document.as<JsonArrayConst>();
      for (JsonObjectConst entry : entries) {
        const String ssid = entry["ssid"].as<String>();
        const String password = entry["password"].as<String>();
        if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
            networkCount >= MAX_WIFI_NETWORKS) {
          continue;
        }
        networks[networkCount++] = {ssid, password};
      }
    }
  }

  // Versions before 0.3.0 stored one network in these two keys. Keep reading
  // them until a configuration change migrates the data to the ordered list.
  if (networkCount == 0) {
    const String legacySsid =
        preferences.isKey("ssid") ? preferences.getString("ssid", "") : String();
    const String legacyPassword = preferences.isKey("password")
                                      ? preferences.getString("password", "")
                                      : String();
    if (!legacySsid.isEmpty()) {
      networks[networkCount++] = {legacySsid, legacyPassword};
    }
  }
  preferences.end();
  return networkCount > 0;
}

bool saveNetworkCredentials(const NetworkCredential *networks, size_t networkCount) {
  JsonDocument document;
  JsonArray entries = document.to<JsonArray>();
  for (size_t index = 0; index < networkCount; ++index) {
    JsonObject entry = entries.add<JsonObject>();
    entry["ssid"] = networks[index].ssid;
    entry["password"] = networks[index].password;
  }
  String encodedNetworks;
  serializeJson(document, encodedNetworks);

  Preferences preferences;
  if (!preferences.begin("hwaipy-net", false)) {
    return false;
  }
  const bool saved = preferences.putString("networks", encodedNetworks) > 0;
  if (saved) {
    if (preferences.isKey("ssid")) {
      preferences.remove("ssid");
    }
    if (preferences.isKey("password")) {
      preferences.remove("password");
    }
    if (preferences.isKey("last")) {
      preferences.remove("last");
    }
  }
  preferences.end();
  return saved;
}

void rememberSuccessfulNetwork(size_t index) {
  Preferences preferences;
  if (!preferences.begin("hwaipy-net", false)) {
    return;
  }
  if (!preferences.isKey("last") || preferences.getUChar("last") != index) {
    preferences.putUChar("last", static_cast<uint8_t>(index));
  }
  preferences.end();
}

int lastSuccessfulNetwork() {
  Preferences preferences;
  if (!preferences.begin("hwaipy-net", true)) {
    return -1;
  }
  const int index = preferences.isKey("last") ? preferences.getUChar("last") : -1;
  preferences.end();
  return index;
}

void clearNetworkCredentials() {
  Preferences preferences;
  if (preferences.begin("hwaipy-net", false)) {
    preferences.clear();
    preferences.end();
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true, true);
  }
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  NetworkCredential networks[MAX_WIFI_NETWORKS];
  size_t networkCount = 0;
  if (!loadNetworkCredentials(networks, networkCount)) {
    Serial.println("Wi-Fi is not configured.");
    Serial.println("Send: wifi {\"ssid\":\"YOUR_SSID\",\"password\":\"YOUR_PASSWORD\"}");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  for (size_t index = 0; index < networkCount; ++index) {
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(networks[index].ssid.c_str(), networks[index].password.c_str());
    Serial.printf("Connecting to Wi-Fi [%u/%u] %s", static_cast<unsigned>(index + 1),
                  static_cast<unsigned>(networkCount), networks[index].ssid.c_str());
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
      delay(300);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      rememberSuccessfulNetwork(index);
      WiFi.setAutoReconnect(true);
      Serial.printf("Wi-Fi connected: %s, RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.printf("Unable to connect to %s.\n", networks[index].ssid.c_str());
  }
  WiFi.disconnect(false, false);
  Serial.println("All configured Wi-Fi networks failed.");
  return false;
}

bool synchronizeClock() {
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    return true;
  }
  configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com");
  Serial.print("Synchronizing clock");
  const uint32_t startedAt = millis();
  while (time(nullptr) <= 1700000000 && millis() - startedAt < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return time(nullptr) > 1700000000;
}

String urlEncodeQueryValue(const String &value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t byte = static_cast<uint8_t>(value[index]);
    const bool unreserved =
        (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
        byte == '~';
    if (unreserved) {
      encoded += static_cast<char>(byte);
    } else {
      encoded += '%';
      encoded += HEX_DIGITS[(byte >> 4) & 0x0F];
      encoded += HEX_DIGITS[byte & 0x0F];
    }
  }
  return encoded;
}

String heartbeatUrl() {
  String url = String(OTA_BASE_URL) + "/" + DEVICE_MODEL + "/" + deviceId + "/hb";
  url += "?v=" + String(FIRMWARE_VERSION);
  url += "&base=" + String(BASE_VERSION);
  url += "&build=" + String(FIRMWARE_BUILD);
  url += "&uptime=" + String(millis() / 1000U);
  url += "&status=ok";
  url += "&rssi=" + String(WiFi.RSSI());
  url += "&heap=" + String(ESP.getFreeHeap());
  url += "&reset=" + String(resetReasonName(esp_reset_reason()));
  url += "&ota=" + otaStatus;
  url += "&wifi_ssid=" + urlEncodeQueryValue(WiFi.SSID());
  url += "&local_ip=" + WiFi.localIP().toString();
  return url;
}

String bytesToHex(const uint8_t *bytes, size_t length) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  String output;
  output.reserve(length * 2);
  for (size_t index = 0; index < length; ++index) {
    output += HEX_DIGITS[(bytes[index] >> 4) & 0x0F];
    output += HEX_DIGITS[bytes[index] & 0x0F];
  }
  return output;
}

int sha256Start(mbedtls_sha256_context *context) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_starts(context, 0);
#else
  return mbedtls_sha256_starts_ret(context, 0);
#endif
}

int sha256Update(mbedtls_sha256_context *context, const uint8_t *data,
                 size_t length) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_update(context, data, length);
#else
  return mbedtls_sha256_update_ret(context, data, length);
#endif
}

int sha256Finish(mbedtls_sha256_context *context, uint8_t digest[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_finish(context, digest);
#else
  return mbedtls_sha256_finish_ret(context, digest);
#endif
}

bool constantTimeEquals(const String &left, const String &right) {
  if (left.length() != right.length()) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t index = 0; index < left.length(); ++index) {
    difference |= static_cast<uint8_t>(
        std::tolower(static_cast<unsigned char>(left[index])) ^
        std::tolower(static_cast<unsigned char>(right[index])));
  }
  return difference == 0;
}

bool installFirmware(const String &url, const String &targetVersion,
                     const String &expectedSha256, size_t expectedSize) {
  if (expectedSha256.length() != 64 || expectedSize == 0) {
    Serial.println("OTA metadata is incomplete.");
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setCACert(OTA_ROOT_CA);
  secureClient.setTimeout(OTA_READ_TIMEOUT_MS / 1000U);
  HTTPClient request;
  request.setTimeout(HTTP_TIMEOUT_MS);
  request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!request.begin(secureClient, url)) {
    Serial.println("Unable to open OTA URL.");
    return false;
  }
  const int responseCode = request.GET();
  if (responseCode != HTTP_CODE_OK) {
    Serial.printf("OTA download returned HTTP %d.\n", responseCode);
    request.end();
    return false;
  }
  const int contentLength = request.getSize();
  if (contentLength <= 0 || static_cast<size_t>(contentLength) != expectedSize) {
    Serial.printf("OTA size mismatch: expected %u, received %d.\n",
                  static_cast<unsigned>(expectedSize), contentLength);
    request.end();
    return false;
  }
  if (!Update.begin(expectedSize, U_FLASH)) {
    Update.printError(Serial);
    request.end();
    return false;
  }

  mbedtls_sha256_context shaContext;
  mbedtls_sha256_init(&shaContext);
  if (sha256Start(&shaContext) != 0) {
    Serial.println("Unable to initialize OTA SHA-256 verification.");
    mbedtls_sha256_free(&shaContext);
    Update.abort();
    request.end();
    return false;
  }

  WiFiClient *stream = request.getStreamPtr();
  uint8_t buffer[4096];
  size_t received = 0;
  unsigned lastReportedProgress = 0;
  uint32_t lastDataAt = millis();
  bool writeOk = true;
  Serial.printf("Installing %s (%u bytes)\n", targetVersion.c_str(),
                static_cast<unsigned>(expectedSize));
  while (received < expectedSize) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t toRead = std::min(available, sizeof(buffer));
      const int count = stream->readBytes(buffer, toRead);
      if (count <= 0) {
        writeOk = false;
        break;
      }
      if (sha256Update(&shaContext, buffer, count) != 0) {
        writeOk = false;
        break;
      }
      if (Update.write(buffer, count) != static_cast<size_t>(count)) {
        writeOk = false;
        break;
      }
      received += count;
      lastDataAt = millis();
      const unsigned progress =
          static_cast<unsigned>((received * 100U) / expectedSize);
      if (progress == 100U || progress >= lastReportedProgress + 5U) {
        Serial.printf("\rOTA progress: %u%%", progress);
        lastReportedProgress = progress;
      }
    } else {
      if (!request.connected() || millis() - lastDataAt > OTA_READ_TIMEOUT_MS) {
        writeOk = false;
        break;
      }
      delay(10);
    }
  }
  Serial.println();

  uint8_t digest[32];
  if (sha256Finish(&shaContext, digest) != 0) {
    Serial.println("Unable to finish OTA SHA-256 verification.");
    mbedtls_sha256_free(&shaContext);
    Update.abort();
    request.end();
    return false;
  }
  mbedtls_sha256_free(&shaContext);
  request.end();

  const String actualSha256 = bytesToHex(digest, sizeof(digest));
  if (!writeOk || received != expectedSize ||
      !constantTimeEquals(actualSha256, expectedSha256)) {
    Serial.printf("OTA verification failed. SHA-256: %s\n", actualSha256.c_str());
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Update.printError(Serial);
    return false;
  }

  rememberOtaTarget(targetVersion);
  Serial.println("OTA verified. Rebooting into the requested version.");
  delay(750);
  ESP.restart();
  return true;
}

bool sendHeartbeat() {
  WiFiClientSecure secureClient;
  secureClient.setCACert(OTA_ROOT_CA);
  HTTPClient request;
  request.setTimeout(HTTP_TIMEOUT_MS);
  if (!request.begin(secureClient, heartbeatUrl())) {
    Serial.println("Unable to open heartbeat URL.");
    return false;
  }
  const int responseCode = request.GET();
  if (responseCode != HTTP_CODE_OK) {
    Serial.printf("Heartbeat returned HTTP %d.\n", responseCode);
    request.end();
    return false;
  }

  JsonDocument response;
  const DeserializationError jsonError = deserializeJson(response, request.getStream());
  request.end();
  if (jsonError) {
    Serial.printf("Heartbeat JSON error: %s\n", jsonError.c_str());
    return false;
  }

  const uint32_t intervalSeconds = response["heartbeat_interval"] | 60U;
  heartbeatIntervalMs = constrain(intervalSeconds, 15U, 3600U) * 1000U;
  const String action = response["action"].as<String>();
  const String targetVersion = response["target_version"].as<String>();
  const String otaUrl = response["ota_url"].as<String>();
  const String sha256 = response["firmware_sha256"].as<String>();
  const size_t firmwareSize = response["firmware_size"] | 0U;

  Serial.printf("Heartbeat OK: action=%s, current=%s, target=%s\n", action.c_str(),
                FIRMWARE_VERSION, targetVersion.isEmpty() ? "none" : targetVersion.c_str());
  if (reportPendingOtaResult) {
    clearOtaResult();
  }
  if ((action == "update" || action == "downgrade") &&
      !targetVersion.isEmpty() && targetVersion != FIRMWARE_VERSION) {
    otaStatus = "downloading";
    if (!installFirmware(otaUrl, targetVersion, sha256, firmwareSize)) {
      otaStatus = "failed";
      return false;
    }
  }
  return true;
}

void handleSerialCommand(String command) {
  command.trim();
  if (command.isEmpty()) {
    return;
  }
  if (command == "info") {
    printHardwareReport();
    return;
  }
  if (command == "heartbeat") {
    nextHeartbeatAt = millis();
    Serial.println("Heartbeat scheduled.");
    return;
  }
  if (command == "wifi clear") {
    clearNetworkCredentials();
    Serial.println("All Wi-Fi credentials cleared.");
    return;
  }
  if (command == "wifi list") {
    NetworkCredential networks[MAX_WIFI_NETWORKS];
    size_t networkCount = 0;
    if (!loadNetworkCredentials(networks, networkCount)) {
      Serial.println("No Wi-Fi networks configured.");
      return;
    }
    const int lastSuccessful = lastSuccessfulNetwork();
    Serial.printf("Configured Wi-Fi networks (%u):\n", static_cast<unsigned>(networkCount));
    for (size_t index = 0; index < networkCount; ++index) {
      Serial.printf("  [%u] %s%s%s\n", static_cast<unsigned>(index),
                    networks[index].ssid.c_str(),
                    static_cast<int>(index) == lastSuccessful ? " (last successful)" : "",
                    WiFi.status() == WL_CONNECTED && WiFi.SSID() == networks[index].ssid
                        ? " (connected)"
                        : "");
    }
    return;
  }
  if (command.startsWith("wifi remove ")) {
    const String indexText = command.substring(12);
    char *end = nullptr;
    const long requestedIndex = strtol(indexText.c_str(), &end, 10);
    if (indexText.isEmpty() || end == indexText.c_str() || *end != '\0') {
      Serial.println("Usage: wifi remove INDEX");
      return;
    }
    NetworkCredential networks[MAX_WIFI_NETWORKS];
    size_t networkCount = 0;
    if (!loadNetworkCredentials(networks, networkCount) || requestedIndex < 0 ||
        static_cast<size_t>(requestedIndex) >= networkCount) {
      Serial.println("Wi-Fi network index is invalid.");
      return;
    }
    const String removedSsid = networks[requestedIndex].ssid;
    for (size_t index = static_cast<size_t>(requestedIndex); index + 1 < networkCount;
         ++index) {
      networks[index] = networks[index + 1];
    }
    --networkCount;
    bool saved = false;
    if (networkCount == 0) {
      clearNetworkCredentials();
      saved = true;
    } else {
      saved = saveNetworkCredentials(networks, networkCount);
    }
    if (!saved) {
      Serial.println("Unable to save Wi-Fi credentials.");
      return;
    }
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true, false);
    }
    nextHeartbeatAt = millis();
    Serial.printf("Wi-Fi network removed: %s.\n", removedSsid.c_str());
    return;
  }
  const bool addCommand = command.startsWith("wifi add ");
  if (addCommand || command.startsWith("wifi ")) {
    JsonDocument document;
    const DeserializationError error =
        deserializeJson(document, command.substring(addCommand ? 9 : 5));
    if (error) {
      Serial.printf("Wi-Fi command JSON error: %s\n", error.c_str());
      return;
    }
    const String ssid = document["ssid"].as<String>();
    const String password = document["password"].as<String>();
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
      Serial.println("Wi-Fi SSID/password length is invalid.");
      return;
    }
    NetworkCredential networks[MAX_WIFI_NETWORKS];
    size_t networkCount = 0;
    loadNetworkCredentials(networks, networkCount);
    size_t targetIndex = networkCount;
    for (size_t index = 0; index < networkCount; ++index) {
      if (networks[index].ssid == ssid) {
        targetIndex = index;
        break;
      }
    }
    if (targetIndex == networkCount) {
      if (networkCount >= MAX_WIFI_NETWORKS) {
        Serial.printf("Wi-Fi network limit reached (%u).\n",
                      static_cast<unsigned>(MAX_WIFI_NETWORKS));
        return;
      }
      ++networkCount;
    }
    networks[targetIndex] = {ssid, password};
    if (!saveNetworkCredentials(networks, networkCount)) {
      Serial.println("Unable to save Wi-Fi credentials.");
      return;
    }
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true, false);
    }
    nextHeartbeatAt = millis();
    Serial.printf("Wi-Fi credentials saved for %s at index %u.\n", ssid.c_str(),
                  static_cast<unsigned>(targetIndex));
    return;
  }
  Serial.println(
      "Commands: info | heartbeat | wifi list | wifi clear | wifi remove INDEX | "
      "wifi [add] {JSON}");
}

void processSerial() {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 256) {
      serialLine += character;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // USB Serial/JTAG output must never hold up networking when no terminal is
  // consuming the CDC stream. Diagnostics are best-effort only.
  Serial.setTxTimeoutMs(0);
#endif
  delay(1500);
  deviceId = factoryDeviceId();
  confirmRunningImage();
  loadOtaResult();
  printHardwareReport();
  HWAIPY_APP_SETUP();
  nextHeartbeatAt = millis();
}

void loop() {
  processSerial();
  if (static_cast<int32_t>(millis() - nextHeartbeatAt) >= 0) {
    bool success = false;
    if (connectWifi() && synchronizeClock()) {
      success = sendHeartbeat();
    }
    nextHeartbeatAt = millis() +
                      (success ? heartbeatIntervalMs : RETRY_HEARTBEAT_INTERVAL_MS);
  }
  delay(20);
}
