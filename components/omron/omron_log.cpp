#include "omron_log.h"

// Host half only. Under the firmware omron_log.h resolves to esphome's macros
// and this file compiles to nothing, which keeps the shim from costing the
// device a single byte.
#ifndef USE_ESP32

#include <cstdarg>
#include <cstdio>

namespace esphome::omron {

namespace {

bool g_enabled = false;

char hex_digit(uint8_t value) {
  return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
}

}  // namespace

std::string format_hex_pretty(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0)
    return "";
  std::string result;
  result.resize(3 * length - 1);
  for (size_t i = 0; i < length; i++) {
    result[3 * i] = hex_digit(static_cast<uint8_t>((data[i] & 0xF0) >> 4));
    result[3 * i + 1] = hex_digit(static_cast<uint8_t>(data[i] & 0x0F));
    if (i != length - 1)
      result[3 * i + 2] = '.';
  }
  if (length > 4)
    return result + " (" + std::to_string(length) + ")";
  return result;
}

void host_log_enable(bool enabled) {
  g_enabled = enabled;
}

void host_log(char level, const char *tag, const char *format, ...) {
  if (!g_enabled)
    return;
  std::fprintf(stderr, "[%c][%s] ", level, tag);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
}

}  // namespace esphome::omron

#endif  // !USE_ESP32
