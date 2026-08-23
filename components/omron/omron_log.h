#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// One set of log lines, two builds. Under the firmware these are esphome's own
// macros, unchanged; on the host they reach a sink the test suite silences by
// default. Not a general logging abstraction - it exists so the session layer
// can log and still compile into the host binary.

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#define OMRON_LOG_E(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define OMRON_LOG_W(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define OMRON_LOG_I(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define OMRON_LOG_D(tag, ...) ESP_LOGD(tag, __VA_ARGS__)
#define OMRON_LOG_V(tag, ...) ESP_LOGV(tag, __VA_ARGS__)

namespace esphome::omron {
using ::esphome::format_hex_pretty;
}  // namespace esphome::omron

#else

namespace esphome::omron {

// Byte for byte what esphome prints: uppercase bytes joined by dots, count
// appended past four. Test expectations quote real log lines.
std::string format_hex_pretty(const uint8_t *data, size_t length);

// Off by default, so the suite's output stays its own asserts. The runner turns
// it on when OMRON_TEST_LOG is set.
void host_log_enable(bool enabled);

void host_log(char level, const char *tag, const char *format, ...) __attribute__((format(printf, 3, 4)));

}  // namespace esphome::omron

#define OMRON_LOG_E(tag, ...) ::esphome::omron::host_log('E', tag, __VA_ARGS__)
#define OMRON_LOG_W(tag, ...) ::esphome::omron::host_log('W', tag, __VA_ARGS__)
#define OMRON_LOG_I(tag, ...) ::esphome::omron::host_log('I', tag, __VA_ARGS__)
#define OMRON_LOG_D(tag, ...) ::esphome::omron::host_log('D', tag, __VA_ARGS__)
#define OMRON_LOG_V(tag, ...) ::esphome::omron::host_log('V', tag, __VA_ARGS__)

#endif  // USE_ESP32
