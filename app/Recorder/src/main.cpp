#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>
#include <usb/uac_host.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>

void startAudioStreamClient();

#define HWAIPY_FIRMWARE_NAME "Hwaipy ESP32 Audio Recorder"
#define HWAIPY_FIRMWARE_VERSION "audio_recorder_1.0.4"
#define HWAIPY_FIRMWARE_BUILD "20260819.4"
#define HWAIPY_APP_SETUP() startAudioStreamClient()

#include "../../../base/src/main.cpp"

namespace {

constexpr char AUDIO_HOST[] = "grayfog.chat";
constexpr uint16_t AUDIO_PORT = 8054;
constexpr char PROTOCOL_MAGIC[] = "RSPAUDIO1\n";
constexpr uint64_t CONTROL_OFFSET = UINT64_MAX;
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint8_t CHANNELS = 1;
constexpr uint8_t SAMPLE_WIDTH = 2;
constexpr uint8_t BITS_PER_SAMPLE = SAMPLE_WIDTH * 8;
constexpr uint32_t FRAME_DURATION_MS = 100;
constexpr uint32_t FRAME_SAMPLES = SAMPLE_RATE * FRAME_DURATION_MS / 1000;
constexpr size_t FRAME_BYTES = FRAME_SAMPLES * CHANNELS * SAMPLE_WIDTH;
constexpr size_t FRAME_HEADER_BYTES = 20;
constexpr size_t ACK_BYTES = 14;
constexpr size_t READ_CHUNK_BYTES = 4096;
constexpr uint32_t UAC_BUFFER_BYTES = 32768;
constexpr uint32_t UAC_THRESHOLD_BYTES = 4096;
constexpr uint16_t FRAME_POOL_COUNT = 40;
constexpr uint16_t INVALID_FRAME_INDEX = UINT16_MAX;
constexpr uint32_t UPLOAD_WINDOW_FRAMES = 20;
constexpr uint32_t ACK_TIMEOUT_MS = 5000;
constexpr uint32_t RECONNECT_DELAY_MS = 2000;
constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 10000;
constexpr uint64_t FRAME_DURATION_NS =
    static_cast<uint64_t>(FRAME_DURATION_MS) * 1000000ULL;
constexpr int64_t WALL_CLOCK_STEP_NS = 2000000000LL;
constexpr time_t VALID_EPOCH = 1700000000;
constexpr uint8_t DEFAULT_GAIN = 12;
constexpr bool DEFAULT_AGC = true;

enum class EventGroup : uint8_t {
  Driver,
  Device,
};

struct RecorderEvent {
  EventGroup group;
  union {
    struct {
      uint8_t address;
      uint8_t interfaceNumber;
      uac_host_driver_event_t event;
    } driver;
    struct {
      uac_host_device_handle_t handle;
      uac_host_device_event_t event;
    } device;
  };
};

struct AudioFrame {
  uint64_t timestampNs;
  uint8_t pcm[FRAME_BYTES];
};

QueueHandle_t eventQueue = nullptr;
QueueHandle_t freeFrameQueue = nullptr;
QueueHandle_t readyFrameQueue = nullptr;
SemaphoreHandle_t microphoneMutex = nullptr;
TaskHandle_t uacTaskHandle = nullptr;
AudioFrame *framePool = nullptr;
uac_host_device_handle_t microphoneHandle = nullptr;
uint16_t captureFrameIndex = INVALID_FRAME_INDEX;
size_t captureFrameOffset = 0;
uint64_t nextCaptureTimestampNs = 0;
String streamDeviceId;

volatile bool microphoneConnected = false;
volatile bool streamConnected = false;
volatile bool enumFilterCalled = false;
volatile uint16_t microphoneVid = 0;
volatile uint16_t microphonePid = 0;
volatile uint8_t actualGain = DEFAULT_GAIN;
volatile bool actualAgc = DEFAULT_AGC;
volatile uint32_t controlGeneration = UINT32_MAX;
volatile uint32_t framesCaptured = 0;
volatile uint32_t framesSent = 0;
volatile uint32_t framesDropped = 0;
volatile uint32_t transferErrors = 0;
volatile uint32_t reconnectCount = 0;
volatile uint32_t audioConnectFailures = 0;
volatile uint32_t audioHandshakeFailures = 0;
volatile uint32_t clockStepCount = 0;
volatile uint64_t acknowledgedBytes = 0;
volatile uint64_t sentBytes = 0;
volatile int32_t usbInstallResult = ESP_ERR_INVALID_STATE;
volatile int32_t uacInstallResult = ESP_ERR_INVALID_STATE;
volatile int32_t rootPortPowerResult = ESP_ERR_INVALID_STATE;
volatile int32_t lastVolumeResult = ESP_ERR_INVALID_STATE;
volatile int32_t lastAgcResult = ESP_ERR_INVALID_STATE;

void putBe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value >> 24U);
  destination[1] = static_cast<uint8_t>(value >> 16U);
  destination[2] = static_cast<uint8_t>(value >> 8U);
  destination[3] = static_cast<uint8_t>(value);
}

void putBe64(uint8_t *destination, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    destination[index] = static_cast<uint8_t>(value >> (56U - index * 8U));
  }
}

uint32_t readBe32(const uint8_t *source) {
  return (static_cast<uint32_t>(source[0]) << 24U) |
         (static_cast<uint32_t>(source[1]) << 16U) |
         (static_cast<uint32_t>(source[2]) << 8U) |
         static_cast<uint32_t>(source[3]);
}

uint64_t readBe64(const uint8_t *source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value = (value << 8U) | source[index];
  }
  return value;
}

uint64_t realtimeNs() {
  timespec now = {};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec <= VALID_EPOCH) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(now.tv_nsec);
}

uint8_t gainToVolume(uint8_t gain) {
  return static_cast<uint8_t>((static_cast<uint16_t>(gain) * 100U + 8U) / 16U);
}

uint8_t volumeToGain(uint8_t volume) {
  return static_cast<uint8_t>((static_cast<uint16_t>(volume) * 16U + 50U) / 100U);
}

void loadSavedControls() {
  Preferences preferences;
  if (!preferences.begin("audio-stream", true)) {
    return;
  }
  actualGain = std::min<uint8_t>(preferences.getUChar("gain", DEFAULT_GAIN), 16);
  actualAgc = preferences.getBool("agc", DEFAULT_AGC);
  preferences.end();
}

void saveControls(uint8_t gain, bool agc) {
  Preferences preferences;
  if (preferences.begin("audio-stream", false)) {
    preferences.putUChar("gain", gain);
    preferences.putBool("agc", agc);
    preferences.end();
  }
}

bool applyMicrophoneControl(uint8_t requestedGain, bool requestedAgc) {
  if (requestedGain > 16 || microphoneMutex == nullptr) {
    return false;
  }
  bool success = false;
  xSemaphoreTake(microphoneMutex, portMAX_DELAY);
  uac_host_device_handle_t handle = microphoneHandle;
  if (handle != nullptr) {
    lastVolumeResult =
        uac_host_device_set_volume(handle, gainToVolume(requestedGain));
    lastAgcResult = uac_host_device_set_agc(handle, requestedAgc);
    uint8_t readVolume = 0;
    bool readAgc = false;
    const esp_err_t volumeReadResult =
        uac_host_device_get_volume(handle, &readVolume);
    const esp_err_t agcReadResult = uac_host_device_get_agc(handle, &readAgc);
    if (lastVolumeResult == ESP_OK && lastAgcResult == ESP_OK &&
        volumeReadResult == ESP_OK && agcReadResult == ESP_OK) {
      actualGain = volumeToGain(readVolume);
      actualAgc = readAgc;
      success = actualGain == requestedGain && actualAgc == requestedAgc;
    }
  }
  xSemaphoreGive(microphoneMutex);
  if (success) {
    saveControls(actualGain, actualAgc);
  }
  return success;
}

bool supportsFormat(const uac_host_dev_alt_param_t &parameters) {
  if (parameters.channels != CHANNELS ||
      parameters.bit_resolution != BITS_PER_SAMPLE) {
    return false;
  }
  if (parameters.sample_freq_type == 0) {
    return SAMPLE_RATE >= parameters.sample_freq_lower &&
           SAMPLE_RATE <= parameters.sample_freq_upper;
  }
  for (uint8_t index = 0; index < parameters.sample_freq_type; ++index) {
    if (parameters.sample_freq[index] == SAMPLE_RATE) {
      return true;
    }
  }
  return false;
}

bool findMicrophoneFormat(uac_host_device_handle_t handle,
                          uac_host_dev_alt_param_t &selected) {
  uac_host_dev_info_t information = {};
  if (uac_host_get_device_info(handle, &information) != ESP_OK) {
    return false;
  }
  microphoneVid = information.VID;
  microphonePid = information.PID;
  for (uint8_t alternate = 1; alternate <= information.iface_alt_num;
       ++alternate) {
    uac_host_dev_alt_param_t parameters = {};
    if (uac_host_get_device_alt_param(handle, alternate, &parameters) == ESP_OK &&
        supportsFormat(parameters)) {
      selected = parameters;
      return true;
    }
  }
  return false;
}

bool usbEnumerationFilter(const usb_device_desc_t *descriptor,
                          uint8_t *configurationValue) {
  enumFilterCalled = true;
  *configurationValue = 1;
  if (descriptor != nullptr) {
    Serial.printf(
        "USB device detected : VID=%04x PID=%04x class=%02x config=%u\n",
        descriptor->idVendor, descriptor->idProduct,
        descriptor->bDeviceClass, *configurationValue);
  }
  return true;
}

void microphoneDeviceCallback(uac_host_device_handle_t handle,
                              const uac_host_device_event_t event, void *) {
  RecorderEvent queued = {};
  queued.group = EventGroup::Device;
  queued.device.handle = handle;
  queued.device.event = event;
  xQueueSend(eventQueue, &queued, 0);
}

void uacDriverCallback(uint8_t address, uint8_t interfaceNumber,
                       const uac_host_driver_event_t event, void *) {
  RecorderEvent queued = {};
  queued.group = EventGroup::Driver;
  queued.driver.address = address;
  queued.driver.interfaceNumber = interfaceNumber;
  queued.driver.event = event;
  xQueueSend(eventQueue, &queued, 0);
}

void releaseCaptureFrame() {
  if (captureFrameIndex != INVALID_FRAME_INDEX) {
    const uint16_t index = captureFrameIndex;
    xQueueSend(freeFrameQueue, &index, 0);
    captureFrameIndex = INVALID_FRAME_INDEX;
    captureFrameOffset = 0;
  }
}

bool acquireCaptureFrame() {
  if (captureFrameIndex != INVALID_FRAME_INDEX) {
    return true;
  }
  uint16_t index = INVALID_FRAME_INDEX;
  if (xQueueReceive(freeFrameQueue, &index, 0) != pdTRUE) {
    if (xQueueReceive(readyFrameQueue, &index, 0) != pdTRUE) {
      ++framesDropped;
      return false;
    }
    ++framesDropped;
  }
  captureFrameIndex = index;
  captureFrameOffset = 0;
  return true;
}

void finishCaptureFrame() {
  const uint64_t observedNow = realtimeNs();
  if (observedNow == 0) {
    nextCaptureTimestampNs = 0;
    releaseCaptureFrame();
    return;
  }
  const uint64_t observedStart = observedNow - FRAME_DURATION_NS;
  uint64_t timestamp = observedStart;
  if (nextCaptureTimestampNs != 0) {
    const int64_t drift = static_cast<int64_t>(observedStart) -
                          static_cast<int64_t>(nextCaptureTimestampNs);
    if (drift >= -WALL_CLOCK_STEP_NS && drift <= WALL_CLOCK_STEP_NS) {
      timestamp = nextCaptureTimestampNs;
    } else {
      ++clockStepCount;
    }
  }
  nextCaptureTimestampNs = timestamp + FRAME_DURATION_NS;
  framePool[captureFrameIndex].timestampNs = timestamp;
  const uint16_t completed = captureFrameIndex;
  captureFrameIndex = INVALID_FRAME_INDEX;
  captureFrameOffset = 0;
  if (xQueueSend(readyFrameQueue, &completed, 0) == pdTRUE) {
    ++framesCaptured;
  } else {
    xQueueSend(freeFrameQueue, &completed, 0);
    ++framesDropped;
  }
}

void ingestPcm(const uint8_t *source, size_t length) {
  while (length > 0) {
    if (!acquireCaptureFrame()) {
      return;
    }
    const size_t copied = std::min(length, FRAME_BYTES - captureFrameOffset);
    std::memcpy(framePool[captureFrameIndex].pcm + captureFrameOffset, source,
                copied);
    captureFrameOffset += copied;
    source += copied;
    length -= copied;
    if (captureFrameOffset == FRAME_BYTES) {
      finishCaptureFrame();
    }
  }
}

void handleMicrophoneData() {
  if (microphoneHandle == nullptr) {
    return;
  }
  uint8_t chunk[READ_CHUNK_BYTES];
  while (true) {
    uint32_t bytesRead = 0;
    const esp_err_t result = uac_host_device_read(
        microphoneHandle, chunk, sizeof(chunk), &bytesRead, 0);
    if (result != ESP_OK || bytesRead == 0) {
      return;
    }
    ingestPcm(chunk, bytesRead);
    if (bytesRead < sizeof(chunk)) {
      return;
    }
  }
}

void handleMicrophoneConnected(uint8_t address, uint8_t interfaceNumber) {
  if (microphoneHandle != nullptr) {
    Serial.println("UAC connect ignored : microphone already active");
    return;
  }
  Serial.printf("UAC connect event   : address=%u interface=%u\n", address,
                interfaceNumber);
  const uac_host_device_config_t deviceConfig = {
      .addr = address,
      .iface_num = interfaceNumber,
      .buffer_size = UAC_BUFFER_BYTES,
      .buffer_threshold = UAC_THRESHOLD_BYTES,
      .callback = microphoneDeviceCallback,
      .callback_arg = nullptr,
  };
  uac_host_device_handle_t handle = nullptr;
  esp_err_t result = uac_host_device_open(&deviceConfig, &handle);
  if (result != ESP_OK) {
    Serial.printf("UAC device open     : %s\n", esp_err_to_name(result));
    return;
  }
  uac_host_dev_alt_param_t format = {};
  if (!findMicrophoneFormat(handle, format)) {
    Serial.println("UAC format          : no compatible 48kHz/16-bit/mono input");
    uac_host_device_close(handle);
    return;
  }
  Serial.printf("UAC microphone      : VID=%04x PID=%04x, 48kHz/16-bit/mono\n",
                microphoneVid, microphonePid);
  const uac_host_stream_config_t streamConfig = {
      .channels = CHANNELS,
      .bit_resolution = BITS_PER_SAMPLE,
      .sample_freq = SAMPLE_RATE,
      .flags = 0,
  };
  result = uac_host_device_start(handle, &streamConfig);
  if (result != ESP_OK) {
    Serial.printf("UAC stream start    : %s\n", esp_err_to_name(result));
    uac_host_device_close(handle);
    return;
  }
  uac_host_device_set_mute(handle, false);
  xSemaphoreTake(microphoneMutex, portMAX_DELAY);
  microphoneHandle = handle;
  microphoneConnected = true;
  xSemaphoreGive(microphoneMutex);
  const bool controlsApplied = applyMicrophoneControl(actualGain, actualAgc);
  Serial.printf("UAC stream ready    : gain=%u AGC=%s controls=%s\n", actualGain,
                actualAgc ? "on" : "off", controlsApplied ? "ok" : "failed");
}

void handleMicrophoneDisconnected(uac_host_device_handle_t handle) {
  Serial.printf("UAC disconnected    : VID=%04x PID=%04x\n", microphoneVid,
                microphonePid);
  xSemaphoreTake(microphoneMutex, portMAX_DELAY);
  if (handle == microphoneHandle) {
    microphoneHandle = nullptr;
    microphoneConnected = false;
  }
  xSemaphoreGive(microphoneMutex);
  releaseCaptureFrame();
  nextCaptureTimestampNs = 0;
  uac_host_device_close(handle);
}

void usbLibraryTask(void *argument) {
  const usb_host_config_t config = {
      .skip_phy_setup = false,
      .root_port_unpowered = true,
      .intr_flags = ESP_INTR_FLAG_LOWMED,
      .enum_filter_cb = usbEnumerationFilter,
  };
  usbInstallResult = usb_host_install(&config);
  Serial.printf("USB host install    : %s\n",
                esp_err_to_name(static_cast<esp_err_t>(usbInstallResult)));
  if (usbInstallResult != ESP_OK) {
    vTaskDelete(nullptr);
    return;
  }
  rootPortPowerResult = usb_host_lib_set_root_port_power(true);
  Serial.printf("USB root port power : %s\n",
                esp_err_to_name(static_cast<esp_err_t>(rootPortPowerResult)));
  xTaskNotifyGive(static_cast<TaskHandle_t>(argument));
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

void uacEventTask(void *) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  const uac_host_driver_config_t config = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = uacDriverCallback,
      .callback_arg = nullptr,
  };
  uacInstallResult = uac_host_install(&config);
  Serial.printf("UAC driver install  : %s\n",
                esp_err_to_name(static_cast<esp_err_t>(uacInstallResult)));
  if (uacInstallResult != ESP_OK) {
    vTaskDelete(nullptr);
    return;
  }
  RecorderEvent event = {};
  while (true) {
    if (xQueueReceive(eventQueue, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (event.group == EventGroup::Driver) {
      if (event.driver.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        handleMicrophoneConnected(event.driver.address,
                                  event.driver.interfaceNumber);
      }
      continue;
    }
    switch (event.device.event) {
      case UAC_HOST_DEVICE_EVENT_RX_DONE:
        handleMicrophoneData();
        break;
      case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ++transferErrors;
        if (transferErrors == 1 || transferErrors % 32 == 0) {
          Serial.printf("UAC transfer errors : %lu\n",
                        static_cast<unsigned long>(transferErrors));
        }
        break;
      case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        handleMicrophoneDisconnected(event.device.handle);
        break;
      default:
        break;
    }
  }
}

bool writeAll(WiFiClient &client, const uint8_t *data, size_t length) {
  size_t written = 0;
  while (written < length && client.connected()) {
    const size_t count = client.write(data + written, length - written);
    if (count == 0) {
      return false;
    }
    written += count;
  }
  return written == length;
}

bool readExact(WiFiClient &client, uint8_t *destination, size_t length,
               uint32_t timeoutMs) {
  size_t received = 0;
  const uint32_t startedAt = millis();
  while (received < length && millis() - startedAt < timeoutMs) {
    const int count = client.read(destination + received, length - received);
    if (count > 0) {
      received += static_cast<size_t>(count);
    } else if (!client.connected()) {
      return false;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  return received == length;
}

bool sendFrameHeader(WiFiClient &client, uint64_t offset, uint64_t timestampNs,
                     uint32_t length) {
  uint8_t header[FRAME_HEADER_BYTES];
  putBe64(header, offset);
  putBe64(header + 8, timestampNs);
  putBe32(header + 16, length);
  return writeAll(client, header, sizeof(header));
}

bool sendControlReport(WiFiClient &client, uint32_t generation) {
  const uint8_t payload[2] = {actualGain,
                              static_cast<uint8_t>(actualAgc ? 1 : 0)};
  return sendFrameHeader(client, CONTROL_OFFSET, generation, sizeof(payload)) &&
         writeAll(client, payload, sizeof(payload));
}

bool processAck(WiFiClient &client, const uint8_t ack[ACK_BYTES],
                uint64_t sentOffset, uint64_t &ackedOffset,
                uint32_t &appliedGeneration, uint32_t &lastControlAttemptAt) {
  const uint64_t nextOffset = readBe64(ack);
  const uint32_t desiredGeneration = readBe32(ack + 8);
  const uint8_t desiredGain = ack[12];
  const bool desiredAgc = ack[13] != 0;
  if (nextOffset < ackedOffset || nextOffset > sentOffset || desiredGain > 16) {
    return false;
  }
  ackedOffset = nextOffset;
  acknowledgedBytes = ackedOffset;
  if (desiredGeneration != appliedGeneration &&
      millis() - lastControlAttemptAt >= 500U) {
    lastControlAttemptAt = millis();
    const bool applied = applyMicrophoneControl(desiredGain, desiredAgc);
    if (applied) {
      appliedGeneration = desiredGeneration;
      controlGeneration = desiredGeneration;
    }
    if (!sendControlReport(client, desiredGeneration)) {
      return false;
    }
  }
  return true;
}

String createStreamId() {
  char value[96];
  snprintf(value, sizeof(value), "esp32-%s-%08lx-%08lx-%lu",
           deviceId.c_str(), static_cast<unsigned long>(time(nullptr)),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(reconnectCount));
  return String(value);
}

bool openAudioConnection(WiFiClient &client, uint64_t &ackedOffset,
                         uint64_t &sentOffset, uint32_t &appliedGeneration,
                         uint32_t &lastControlAttemptAt) {
  client.stop();
  client.setTimeout(ACK_TIMEOUT_MS);
  client.setNoDelay(true);
  if (!client.connect(AUDIO_HOST, AUDIO_PORT, ACK_TIMEOUT_MS)) {
    ++audioConnectFailures;
    if (audioConnectFailures == 1 || audioConnectFailures % 15 == 0) {
      Serial.printf("Audio server connect: failed (%lu attempts)\n",
                    static_cast<unsigned long>(audioConnectFailures));
    }
    return false;
  }
  JsonDocument helloDocument;
  helloDocument["device"] = streamDeviceId;
  helloDocument["stream_id"] = createStreamId();
  helloDocument["sample_rate"] = SAMPLE_RATE;
  helloDocument["channels"] = CHANNELS;
  helloDocument["sample_width"] = SAMPLE_WIDTH;
  helloDocument["codec"] = "pcm_s16le";
  helloDocument["earliest_offset"] = 0;
  helloDocument["gain"] = actualGain;
  helloDocument["agc"] = actualAgc;
  String hello;
  serializeJson(helloDocument, hello);
  hello += '\n';
  if (!writeAll(client, reinterpret_cast<const uint8_t *>(PROTOCOL_MAGIC),
                sizeof(PROTOCOL_MAGIC) - 1U) ||
      !writeAll(client, reinterpret_cast<const uint8_t *>(hello.c_str()),
                hello.length())) {
    ++audioHandshakeFailures;
    Serial.println("Audio server hello  : write failed");
    client.stop();
    return false;
  }
  uint8_t ack[ACK_BYTES];
  if (!readExact(client, ack, sizeof(ack), ACK_TIMEOUT_MS)) {
    ++audioHandshakeFailures;
    Serial.println("Audio server hello  : ACK timeout");
    client.stop();
    return false;
  }
  ackedOffset = 0;
  sentOffset = 0;
  appliedGeneration = UINT32_MAX;
  lastControlAttemptAt = millis() - 500U;
  if (!processAck(client, ack, sentOffset, ackedOffset, appliedGeneration,
                  lastControlAttemptAt) ||
      ackedOffset != 0) {
    ++audioHandshakeFailures;
    Serial.println("Audio server hello  : invalid ACK");
    client.stop();
    return false;
  }
  acknowledgedBytes = 0;
  sentBytes = 0;
  streamConnected = true;
  Serial.printf("Audio stream online : %s -> %s:%u\n", streamDeviceId.c_str(),
                AUDIO_HOST, AUDIO_PORT);
  return true;
}

void audioStreamTask(void *) {
  WiFiClient client;
  uint8_t ackBuffer[ACK_BYTES];
  size_t ackBuffered = 0;
  uint64_t ackedOffset = 0;
  uint64_t sentOffset = 0;
  uint32_t appliedGeneration = UINT32_MAX;
  uint32_t lastControlAttemptAt = 0;
  uint32_t lastAckAt = 0;
  uint32_t lastDiagnosticAt = millis() - DIAGNOSTIC_INTERVAL_MS;
  while (true) {
    if (millis() - lastDiagnosticAt >= DIAGNOSTIC_INTERVAL_MS) {
      lastDiagnosticAt = millis();
      Serial.printf(
          "Recorder status     : mic=%s stream=%s enum=%s frames=%lu/%lu "
          "dropped=%lu reconnects=%lu connect_fail=%lu handshake_fail=%lu\n",
          microphoneConnected ? "ready" : "waiting",
          streamConnected ? "online" : "offline",
          enumFilterCalled ? "seen" : "none",
          static_cast<unsigned long>(framesCaptured),
          static_cast<unsigned long>(framesSent),
          static_cast<unsigned long>(framesDropped),
          static_cast<unsigned long>(reconnectCount),
          static_cast<unsigned long>(audioConnectFailures),
          static_cast<unsigned long>(audioHandshakeFailures));
    }
    if (WiFi.status() != WL_CONNECTED || !microphoneConnected ||
        realtimeNs() == 0) {
      if (client.connected()) {
        client.stop();
      }
      streamConnected = false;
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (!client.connected()) {
      streamConnected = false;
      ++reconnectCount;
      ackBuffered = 0;
      if (!openAudioConnection(client, ackedOffset, sentOffset,
                               appliedGeneration, lastControlAttemptAt)) {
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        continue;
      }
      lastAckAt = millis();
    }

    while (client.available() > 0) {
      const int count = client.read(ackBuffer + ackBuffered,
                                    ACK_BYTES - ackBuffered);
      if (count <= 0) {
        break;
      }
      ackBuffered += static_cast<size_t>(count);
      if (ackBuffered == ACK_BYTES) {
        if (!processAck(client, ackBuffer, sentOffset, ackedOffset,
                        appliedGeneration, lastControlAttemptAt)) {
          client.stop();
          break;
        }
        ackBuffered = 0;
        lastAckAt = millis();
      }
    }
    if (!client.connected()) {
      continue;
    }
    if (millis() - lastAckAt > ACK_TIMEOUT_MS) {
      client.stop();
      continue;
    }

    const uint64_t inFlightBytes = sentOffset - ackedOffset;
    if (inFlightBytes >= FRAME_BYTES * UPLOAD_WINDOW_FRAMES) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    uint16_t frameIndex = INVALID_FRAME_INDEX;
    if (xQueueReceive(readyFrameQueue, &frameIndex, pdMS_TO_TICKS(10)) !=
        pdTRUE) {
      continue;
    }
    AudioFrame &frame = framePool[frameIndex];
    const bool sent = sendFrameHeader(client, sentOffset, frame.timestampNs,
                                      FRAME_BYTES) &&
                      writeAll(client, frame.pcm, FRAME_BYTES);
    xQueueSend(freeFrameQueue, &frameIndex, portMAX_DELAY);
    if (!sent) {
      ++framesDropped;
      client.stop();
      continue;
    }
    sentOffset += FRAME_BYTES;
    sentBytes = sentOffset;
    ++framesSent;
  }
}

}  // namespace

void startAudioStreamClient() {
#if !CONFIG_IDF_TARGET_ESP32S3
#error "Audio Stream Client requires ESP32-S3 USB OTG support"
#endif
  loadSavedControls();
  streamDeviceId = "ESP32-" + deviceId;
  framePool = static_cast<AudioFrame *>(heap_caps_calloc(
      FRAME_POOL_COUNT, sizeof(AudioFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  eventQueue = xQueueCreate(16, sizeof(RecorderEvent));
  freeFrameQueue = xQueueCreate(FRAME_POOL_COUNT, sizeof(uint16_t));
  readyFrameQueue = xQueueCreate(FRAME_POOL_COUNT, sizeof(uint16_t));
  microphoneMutex = xSemaphoreCreateMutex();
  if (framePool == nullptr || eventQueue == nullptr || freeFrameQueue == nullptr ||
      readyFrameQueue == nullptr || microphoneMutex == nullptr) {
    Serial.println("Audio stream allocation failed.");
    return;
  }
  for (uint16_t index = 0; index < FRAME_POOL_COUNT; ++index) {
    xQueueSend(freeFrameQueue, &index, 0);
  }
  xTaskCreatePinnedToCore(audioStreamTask, "audio_stream", 12288, nullptr, 4,
                          nullptr, 1);
  xTaskCreatePinnedToCore(uacEventTask, "uac_events", 8192, nullptr, 4,
                          &uacTaskHandle, 0);
  xTaskCreatePinnedToCore(usbLibraryTask, "usb_events", 4096, uacTaskHandle, 5,
                          nullptr, 0);
  Serial.printf("Audio stream device : %s\n", streamDeviceId.c_str());
  Serial.printf("PCM frame pool      : %u x %u bytes (%u ms each)\n",
                FRAME_POOL_COUNT, static_cast<unsigned>(FRAME_BYTES),
                FRAME_DURATION_MS);
}
