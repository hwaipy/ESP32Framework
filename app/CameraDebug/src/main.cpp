#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>
#include <usb/uvc_host.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

void startCameraBenchmark();

#define HWAIPY_FIRMWARE_NAME "Hwaipy ESP32 USB Camera Benchmark"
#define HWAIPY_FIRMWARE_VERSION "camera_debug_0.1.2"
#define HWAIPY_FIRMWARE_BUILD "20260819.1"
#define HWAIPY_APP_SETUP() startCameraBenchmark()

#include "../../../base/src/main.cpp"

namespace {

constexpr char DEBUG_HOST[] = "grayfog.chat";
constexpr uint16_t DEBUG_PORT = 50000;
constexpr uint32_t TEST_DURATION_MS = 10000;
constexpr uint32_t FRAME_BUFFER_BYTES = 300 * 1024;
constexpr uint8_t FRAME_BUFFER_COUNT = 2;
constexpr uint32_t JPEG_UPLOAD_INTERVAL_MS = 2000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 5000;
constexpr uint32_t OPEN_TIMEOUT_MS = 8000;

struct Candidate {
  uint16_t width;
  uint16_t height;
  float fps;
};

struct Result {
  Candidate requested;
  float measuredFps;
  uint32_t frames;
  uint32_t bytes;
  uint32_t maxFrameBytes;
  uint32_t errors;
  int32_t openResult;
  int32_t startResult;
  bool passed;
};

SemaphoreHandle_t frameMutex = nullptr;
SemaphoreHandle_t streamMutex = nullptr;
TaskHandle_t benchmarkTaskHandle = nullptr;
uint8_t *latestJpeg = nullptr;
uint8_t *uploadJpeg = nullptr;
size_t latestJpegLength = 0;
uint16_t latestWidth = 0;
uint16_t latestHeight = 0;
uint8_t cameraAddress = 0;
uint8_t cameraStreamIndex = 0;
uvc_host_frame_info_t *frameInfo = nullptr;
size_t frameInfoCount = 0;
uvc_host_stream_hdl_t streamHandle = nullptr;

volatile bool cameraConnected = false;
volatile bool streamRunning = false;
volatile uint32_t framesReceived = 0;
volatile uint32_t bytesReceived = 0;
volatile uint32_t maxFrameBytes = 0;
volatile uint32_t transferErrors = 0;
volatile uint32_t overflows = 0;
volatile uint32_t underflows = 0;
volatile int32_t usbInstallResult = ESP_ERR_INVALID_STATE;
volatile int32_t uvcInstallResult = ESP_ERR_INVALID_STATE;
volatile int32_t rootPortPowerResult = ESP_ERR_INVALID_STATE;
volatile uint32_t enumerationCount = 0;
volatile uint16_t enumeratedVid = 0;
volatile uint16_t enumeratedPid = 0;
volatile uint8_t enumeratedDeviceClass = 0;
String lastMode = "none";

const char *formatName(uvc_host_stream_format format) {
  switch (format) {
    case UVC_VS_FORMAT_MJPEG:
      return "MJPEG";
    case UVC_VS_FORMAT_YUY2:
      return "YUY2";
    case UVC_VS_FORMAT_H264:
      return "H264";
    case UVC_VS_FORMAT_H265:
      return "H265";
    case UVC_VS_FORMAT_NV12:
      return "NV12";
    default:
      return "unknown";
  }
}

bool postBody(const String &resource, const char *contentType,
              const uint8_t *body, size_t length) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  WiFiClient client;
  HTTPClient request;
  request.setConnectTimeout(3000);
  request.setTimeout(8000);
  const String url = String("http://") + DEBUG_HOST + ":" + DEBUG_PORT +
                     "/devices/" + deviceId + "/" + resource;
  if (!request.begin(client, url)) {
    return false;
  }
  request.addHeader("Content-Type", contentType);
  const int status = request.POST(const_cast<uint8_t *>(body), length);
  request.end();
  return status >= 200 && status < 300;
}

bool postJson(JsonDocument &document) {
  String body;
  serializeJson(document, body);
  return postBody("log", "application/json",
                  reinterpret_cast<const uint8_t *>(body.c_str()),
                  body.length());
}

void addFormat(JsonArray formats, const uvc_host_frame_info_t &info) {
  JsonObject item = formats.add<JsonObject>();
  item["format"] = formatName(info.format);
  item["width"] = info.h_res;
  item["height"] = info.v_res;
  item["default_fps"] =
      info.default_interval == 0 ? 0.0f : 10000000.0f / info.default_interval;
  JsonArray fps = item["fps"].to<JsonArray>();
  if (info.interval_type > 0) {
    for (uint8_t index = 0;
         index < info.interval_type && index < CONFIG_UVC_INTERVAL_ARRAY_SIZE;
         ++index) {
      if (info.interval[index] != 0) {
        fps.add(10000000.0f / info.interval[index]);
      }
    }
  } else {
    if (info.interval_min != 0) {
      fps.add(10000000.0f / info.interval_min);
    }
    if (info.interval_max != 0 && info.interval_max != info.interval_min) {
      fps.add(10000000.0f / info.interval_max);
    }
  }
}

void reportFormats() {
  JsonDocument document;
  document["event"] = "uvc_formats";
  document["firmware"] = HWAIPY_FIRMWARE_VERSION;
  document["device_id"] = deviceId;
  document["stream_index"] = cameraStreamIndex;
  JsonArray formats = document["formats"].to<JsonArray>();
  for (size_t index = 0; index < frameInfoCount; ++index) {
    addFormat(formats, frameInfo[index]);
  }
  postJson(document);
}

bool frameCallback(const uvc_host_frame_t *frame, void *) {
  if (frame == nullptr) {
    return true;
  }
  ++framesReceived;
  bytesReceived += frame->data_len;
  if (frame->data_len > maxFrameBytes) {
    maxFrameBytes = frame->data_len;
  }
  if (frame->data_len >= 4 && frame->data_len <= FRAME_BUFFER_BYTES &&
      frame->data[0] == 0xff && frame->data[1] == 0xd8 &&
      xSemaphoreTake(frameMutex, 0) == pdTRUE) {
    std::memcpy(latestJpeg, frame->data, frame->data_len);
    latestJpegLength = frame->data_len;
    latestWidth = frame->vs_format.h_res;
    latestHeight = frame->vs_format.v_res;
    xSemaphoreGive(frameMutex);
  }
  return true;
}

void streamEventCallback(const uvc_host_stream_event_data_t *event, void *) {
  if (event == nullptr) {
    return;
  }
  switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
      ++transferErrors;
      break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
      ++overflows;
      break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
      ++underflows;
      break;
    case UVC_HOST_DEVICE_DISCONNECTED:
      cameraConnected = false;
      streamRunning = false;
      break;
    default:
      break;
  }
}

bool sameCandidate(const Candidate &left, const Candidate &right) {
  return left.width == right.width && left.height == right.height &&
         std::abs(left.fps - right.fps) < 0.2f;
}

std::vector<Candidate> candidateList() {
  std::vector<Candidate> candidates;
  for (size_t index = 0; index < frameInfoCount; ++index) {
    const uvc_host_frame_info_t &info = frameInfo[index];
    if (info.format != UVC_VS_FORMAT_MJPEG) {
      continue;
    }
    auto append = [&](uint32_t interval) {
      if (interval == 0) {
        return;
      }
      Candidate candidate = {static_cast<uint16_t>(info.h_res),
                             static_cast<uint16_t>(info.v_res),
                             10000000.0f / interval};
      if (std::none_of(candidates.begin(), candidates.end(),
                       [&](const Candidate &other) {
                         return sameCandidate(candidate, other);
                       })) {
        candidates.push_back(candidate);
      }
    };
    append(info.default_interval);
    if (info.interval_type > 0) {
      for (uint8_t fpsIndex = 0;
           fpsIndex < info.interval_type &&
           fpsIndex < CONFIG_UVC_INTERVAL_ARRAY_SIZE;
           ++fpsIndex) {
        append(info.interval[fpsIndex]);
      }
    } else {
      append(info.interval_min);
      append(info.interval_max);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &left, const Candidate &right) {
              const uint32_t leftPixels = left.width * left.height;
              const uint32_t rightPixels = right.width * right.height;
              return leftPixels != rightPixels ? leftPixels < rightPixels
                                               : left.fps < right.fps;
            });
  return candidates;
}

Result testCandidate(const Candidate &candidate, uint32_t durationMs,
                     bool leaveRunning) {
  Result result = {};
  result.requested = candidate;
  framesReceived = 0;
  bytesReceived = 0;
  maxFrameBytes = 0;
  transferErrors = 0;
  overflows = 0;
  underflows = 0;

  uvc_host_stream_config_t config = {};
  config.event_cb = streamEventCallback;
  config.frame_cb = frameCallback;
  config.usb.dev_addr = cameraAddress;
  config.usb.uvc_stream_index = cameraStreamIndex;
  config.vs_format.h_res = candidate.width;
  config.vs_format.v_res = candidate.height;
  config.vs_format.fps = candidate.fps;
  config.vs_format.format = UVC_VS_FORMAT_MJPEG;
  config.advanced.number_of_frame_buffers = FRAME_BUFFER_COUNT;
  config.advanced.frame_size = FRAME_BUFFER_BYTES;
  config.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  config.advanced.number_of_urbs = 4;
  config.advanced.urb_size = 8 * 1024;

  uvc_host_stream_hdl_t handle = nullptr;
  result.openResult = uvc_host_stream_open(
      &config, pdMS_TO_TICKS(OPEN_TIMEOUT_MS), &handle);
  if (result.openResult != ESP_OK) {
    return result;
  }
  xSemaphoreTake(streamMutex, portMAX_DELAY);
  streamHandle = handle;
  xSemaphoreGive(streamMutex);
  result.startResult = uvc_host_stream_start(handle);
  if (result.startResult != ESP_OK) {
    uvc_host_stream_close(handle);
    xSemaphoreTake(streamMutex, portMAX_DELAY);
    streamHandle = nullptr;
    xSemaphoreGive(streamMutex);
    return result;
  }
  streamRunning = true;
  char mode[48];
  snprintf(mode, sizeof(mode), "%ux%u@%.1f", candidate.width,
           candidate.height, candidate.fps);
  lastMode = mode;
  const uint32_t startedAt = millis();
  while (cameraConnected && millis() - startedAt < durationMs) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  const uint32_t elapsed = std::max<uint32_t>(1, millis() - startedAt);
  result.frames = framesReceived;
  result.bytes = bytesReceived;
  result.maxFrameBytes = maxFrameBytes;
  result.errors = transferErrors + overflows + underflows;
  result.measuredFps = result.frames * 1000.0f / elapsed;
  result.passed = result.frames >= 5 &&
                  result.measuredFps >= candidate.fps * 0.70f &&
                  result.errors <= 2;
  if (!leaveRunning || !result.passed) {
    if (cameraConnected) {
      uvc_host_stream_stop(handle);
    }
    uvc_host_stream_close(handle);
    xSemaphoreTake(streamMutex, portMAX_DELAY);
    streamHandle = nullptr;
    xSemaphoreGive(streamMutex);
    streamRunning = false;
  }
  return result;
}

void reportResult(const Result &result) {
  JsonDocument document;
  document["event"] = "benchmark_result";
  document["width"] = result.requested.width;
  document["height"] = result.requested.height;
  document["requested_fps"] = result.requested.fps;
  document["measured_fps"] = result.measuredFps;
  document["frames"] = result.frames;
  document["bytes"] = result.bytes;
  document["max_frame_bytes"] = result.maxFrameBytes;
  document["errors"] = result.errors;
  document["open_result"] = result.openResult;
  document["start_result"] = result.startResult;
  document["passed"] = result.passed;
  postJson(document);
}

void benchmarkTask(void *) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  reportFormats();
  const std::vector<Candidate> candidates = candidateList();
  std::vector<Result> results;
  for (const Candidate &candidate : candidates) {
    if (!cameraConnected) {
      break;
    }
    Result result = testCandidate(candidate, TEST_DURATION_MS, false);
    results.push_back(result);
    reportResult(result);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  int bestIndex = -1;
  for (size_t index = 0; index < results.size(); ++index) {
    if (!results[index].passed) {
      continue;
    }
    if (bestIndex < 0) {
      bestIndex = static_cast<int>(index);
      continue;
    }
    const Result &best = results[bestIndex];
    const Result &candidate = results[index];
    const uint32_t bestPixels = best.requested.width * best.requested.height;
    const uint32_t candidatePixels =
        candidate.requested.width * candidate.requested.height;
    if (candidatePixels > bestPixels ||
        (candidatePixels == bestPixels &&
         candidate.measuredFps > best.measuredFps)) {
      bestIndex = static_cast<int>(index);
    }
  }
  if (bestIndex >= 0 && cameraConnected) {
    JsonDocument document;
    document["event"] = "benchmark_best";
    document["width"] = results[bestIndex].requested.width;
    document["height"] = results[bestIndex].requested.height;
    document["fps"] = results[bestIndex].requested.fps;
    postJson(document);
    Result continuous =
        testCandidate(results[bestIndex].requested, UINT32_MAX, true);
    if (!continuous.passed) {
      reportResult(continuous);
    }
  }
  vTaskDelete(nullptr);
}

void driverEventCallback(const uvc_host_driver_event_data_t *event, void *) {
  if (event == nullptr ||
      event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED ||
      cameraConnected) {
    return;
  }
  cameraAddress = event->device_connected.dev_addr;
  cameraStreamIndex = event->device_connected.uvc_stream_index;
  frameInfoCount = event->device_connected.frame_info_num;
  frameInfo = static_cast<uvc_host_frame_info_t *>(heap_caps_calloc(
      frameInfoCount, sizeof(uvc_host_frame_info_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (frameInfo == nullptr) {
    return;
  }
  size_t capacity = frameInfoCount;
  if (uvc_host_get_frame_list(
          cameraAddress, cameraStreamIndex,
          reinterpret_cast<uvc_host_frame_info_t(*)[]>(frameInfo),
          &capacity) != ESP_OK) {
    heap_caps_free(frameInfo);
    frameInfo = nullptr;
    frameInfoCount = 0;
    return;
  }
  frameInfoCount = capacity;
  cameraConnected = true;
  xTaskNotifyGive(benchmarkTaskHandle);
}

bool usbEnumerationFilter(const usb_device_desc_t *descriptor,
                          uint8_t *configurationValue) {
  ++enumerationCount;
  if (descriptor != nullptr) {
    enumeratedVid = descriptor->idVendor;
    enumeratedPid = descriptor->idProduct;
    enumeratedDeviceClass = descriptor->bDeviceClass;
  }
  *configurationValue = 1;
  return true;
}

void usbLibraryTask(void *) {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

void uploadTask(void *) {
  uint32_t lastUploadAt = 0;
  uint32_t lastTelemetryAt = 0;
  while (true) {
    const uint32_t now = millis();
    if (streamRunning && now - lastUploadAt >= JPEG_UPLOAD_INTERVAL_MS &&
        xSemaphoreTake(frameMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      const size_t length = latestJpegLength;
      const uint16_t width = latestWidth;
      const uint16_t height = latestHeight;
      if (length > 0) {
        std::memcpy(uploadJpeg, latestJpeg, length);
      }
      xSemaphoreGive(frameMutex);
      if (length > 0 &&
          postBody("frame", "image/jpeg", uploadJpeg, length)) {
        lastUploadAt = now;
      }
      (void)width;
      (void)height;
    }
    if (now - lastTelemetryAt >= TELEMETRY_INTERVAL_MS) {
      JsonDocument document;
      document["event"] = "camera_status";
      document["camera_connected"] = cameraConnected;
      document["stream_running"] = streamRunning;
      document["mode"] = lastMode;
      document["frames"] = framesReceived;
      document["bytes"] = bytesReceived;
      document["max_frame_bytes"] = maxFrameBytes;
      document["transfer_errors"] = transferErrors;
      document["overflows"] = overflows;
      document["underflows"] = underflows;
      document["free_heap"] = ESP.getFreeHeap();
      document["free_psram"] = ESP.getFreePsram();
      document["usb_install"] = usbInstallResult;
      document["uvc_install"] = uvcInstallResult;
      document["root_power"] = rootPortPowerResult;
      document["enumerations"] = enumerationCount;
      document["usb_vid"] = enumeratedVid;
      document["usb_pid"] = enumeratedPid;
      document["usb_device_class"] = enumeratedDeviceClass;
      postJson(document);
      lastTelemetryAt = now;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void startCameraBenchmark() {
#if !CONFIG_IDF_TARGET_ESP32S3
#error "USB camera benchmark requires ESP32-S3 USB OTG support"
#endif
  frameMutex = xSemaphoreCreateMutex();
  streamMutex = xSemaphoreCreateMutex();
  latestJpeg = static_cast<uint8_t *>(heap_caps_malloc(
      FRAME_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uploadJpeg = static_cast<uint8_t *>(heap_caps_malloc(
      FRAME_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (frameMutex == nullptr || streamMutex == nullptr || latestJpeg == nullptr ||
      uploadJpeg == nullptr) {
    Serial.println("Camera benchmark allocation failed.");
    return;
  }
  xTaskCreatePinnedToCore(benchmarkTask, "camera_bench", 12288, nullptr, 4,
                          &benchmarkTaskHandle, 1);
  xTaskCreatePinnedToCore(uploadTask, "camera_upload", 10240, nullptr, 3,
                          nullptr, 1);

  const usb_host_config_t hostConfig = {
      .skip_phy_setup = false,
      .root_port_unpowered = true,
      .intr_flags = ESP_INTR_FLAG_LOWMED,
      .enum_filter_cb = usbEnumerationFilter,
  };
  usbInstallResult = usb_host_install(&hostConfig);
  if (usbInstallResult != ESP_OK) {
    Serial.printf("USB Host install failed: %s\n",
                  esp_err_to_name(usbInstallResult));
    return;
  }
  rootPortPowerResult = usb_host_lib_set_root_port_power(true);
  const uvc_host_driver_config_t driverConfig = {
      .driver_task_stack_size = 6144,
      .driver_task_priority = 7,
      .xCoreID = 0,
      .create_background_task = true,
      .event_cb = driverEventCallback,
      .user_ctx = nullptr,
  };
  uvcInstallResult = uvc_host_install(&driverConfig);
  // Register the UVC client before servicing host-library events. Otherwise a
  // camera present at boot can finish enumeration before the class driver is
  // listening and its connection event is lost.
  xTaskCreatePinnedToCore(usbLibraryTask, "usb_events", 4096, nullptr, 6,
                          nullptr, 0);
  Serial.printf("USB camera benchmark: usb=%s power=%s uvc=%s psram=%u\n",
                esp_err_to_name(usbInstallResult),
                esp_err_to_name(rootPortPowerResult),
                esp_err_to_name(uvcInstallResult), ESP.getPsramSize());
}
