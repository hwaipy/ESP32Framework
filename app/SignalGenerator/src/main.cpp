#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_timer.h>

void startSignalGenerator();

#define HWAIPY_FIRMWARE_NAME "Hwaipy SignalGenerator"
#define HWAIPY_FIRMWARE_VERSION "signal_generator_0.1.0"
#define HWAIPY_FIRMWARE_BUILD "20260808.1"
#define HWAIPY_APP_SETUP() startSignalGenerator()

// SignalGenerator inherits the shared board detection, Wi-Fi provisioning,
// heartbeat, firmware verification, and OTA installation implementation.
#include "../../../base/src/main.cpp"

namespace {

constexpr uint64_t SIGNAL_PERIOD_US = 10000;
constexpr uint64_t SIGNAL_HIGH_US = 1000;
constexpr uint64_t TIMER_TICK_US = 1000;
static_assert(SIGNAL_PERIOD_US % TIMER_TICK_US == 0);
static_assert(SIGNAL_HIGH_US % TIMER_TICK_US == 0);
static_assert(SIGNAL_HIGH_US < SIGNAL_PERIOD_US);

#if CONFIG_IDF_TARGET_ESP32S3
// Excludes strapping, memory, USB, JTAG, and non-output-capable pins.
constexpr gpio_num_t SIGNAL_PINS[] = {
    GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5,
    GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_15,
    GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_21,
};
#elif CONFIG_IDF_TARGET_ESP32C3
// Excludes strapping, flash, USB, JTAG, and UART pins.
constexpr gpio_num_t SIGNAL_PINS[] = {
    GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_3, GPIO_NUM_10,
};
#elif CONFIG_IDF_TARGET_ESP32C6
// Excludes strapping, flash, USB, JTAG, and UART pins.
constexpr gpio_num_t SIGNAL_PINS[] = {
    GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_14,
    GPIO_NUM_20, GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_23,
};
#else
#error "Unsupported ESP32 target for SignalGenerator."
#endif

constexpr size_t SIGNAL_PIN_COUNT = sizeof(SIGNAL_PINS) / sizeof(SIGNAL_PINS[0]);
esp_timer_handle_t signalTimer = nullptr;
uint8_t signalPhase = 0;

void setSignalPins(bool high) {
  for (const gpio_num_t pin : SIGNAL_PINS) {
    gpio_set_level(pin, high ? 1 : 0);
  }
}

void signalTimerCallback(void *) {
  constexpr uint8_t highTicks = SIGNAL_HIGH_US / TIMER_TICK_US;
  constexpr uint8_t ticksPerPeriod = SIGNAL_PERIOD_US / TIMER_TICK_US;
  setSignalPins(signalPhase < highTicks);
  signalPhase = static_cast<uint8_t>((signalPhase + 1U) % ticksPerPeriod);
}

}  // namespace

void startSignalGenerator() {
  uint64_t pinMask = 0;
  for (const gpio_num_t pin : SIGNAL_PINS) {
    pinMask |= 1ULL << static_cast<unsigned>(pin);
  }

  const gpio_config_t outputConfig = {
      .pin_bit_mask = pinMask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  const esp_err_t gpioResult = gpio_config(&outputConfig);
  if (gpioResult != ESP_OK) {
    Serial.printf("Signal GPIO configuration failed: %s\n",
                  esp_err_to_name(gpioResult));
    return;
  }
  setSignalPins(false);

  const esp_timer_create_args_t timerArgs = {
      .callback = signalTimerCallback,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "signal-generator",
      .skip_unhandled_events = true,
  };
  const esp_err_t createResult = esp_timer_create(&timerArgs, &signalTimer);
  if (createResult != ESP_OK) {
    Serial.printf("Signal timer creation failed: %s\n",
                  esp_err_to_name(createResult));
    return;
  }

  signalTimerCallback(nullptr);
  const esp_err_t startResult = esp_timer_start_periodic(signalTimer, TIMER_TICK_US);
  if (startResult != ESP_OK) {
    Serial.printf("Signal timer start failed: %s\n", esp_err_to_name(startResult));
    return;
  }

  Serial.printf("Signal output       : %u GPIOs, %llu us high / %llu us period\n",
                static_cast<unsigned>(SIGNAL_PIN_COUNT), SIGNAL_HIGH_US,
                SIGNAL_PERIOD_US);
  Serial.print("Signal GPIOs        : ");
  for (size_t index = 0; index < SIGNAL_PIN_COUNT; ++index) {
    if (index > 0) {
      Serial.print(", ");
    }
    Serial.print(static_cast<unsigned>(SIGNAL_PINS[index]));
  }
  Serial.println();
}
