#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace esphome::omron {

enum class [[nodiscard]] StandardBpError : uint8_t {
  NONE = 0,
  TOO_SHORT,
  TRUNCATED_OPTIONAL_FIELD,
  INVALID_SFLOAT,
  TRAILING_DATA,
};

struct StandardBpTimestamp {
  uint16_t year{0};
  uint8_t month{0};
  uint8_t day{0};
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t second{0};
};

struct StandardBloodPressureMeasurement {
  bool units_kpa{false};
  // The compound value is always three SFLOATs on the wire, but a device that
  // does not measure a subfield sends the reserved NaN code rather than
  // omitting it. MAP in particular is NaN on most cuffs. Each subfield
  // therefore carries its own validity flag instead of failing the parse.
  bool has_systolic{false};
  float systolic{0.0f};
  bool has_diastolic{false};
  float diastolic{0.0f};
  bool has_mean_arterial_pressure{false};
  float mean_arterial_pressure{0.0f};

  bool has_timestamp{false};
  StandardBpTimestamp timestamp{};
  bool has_pulse_rate{false};
  float pulse_rate{0.0f};
  bool has_user_id{false};
  uint8_t user_id{0};
  bool has_measurement_status{false};
  uint16_t measurement_status{0};

  bool body_movement_detected() const { return this->has_measurement_status && (this->measurement_status & 0x0001); }
  bool cuff_too_loose() const { return this->has_measurement_status && (this->measurement_status & 0x0002); }
  bool irregular_pulse_detected() const { return this->has_measurement_status && (this->measurement_status & 0x0004); }
  bool improper_position_detected() const {
    return this->has_measurement_status && (this->measurement_status & 0x0020);
  }
};

// Blood Pressure Feature, 0x2A49. A cuff's own statement of which detections it
// performs, and the only such statement that does not require identifying the
// model first.
//
// Two bytes little-endian, one bit per feature, bits 6 and up reserved. A
// device that supports a detection still reports its result per measurement;
// this says only whether the bit in the measurement status means anything.
struct StandardBpFeatures {
  bool body_movement{false};
  bool cuff_fit{false};
  bool irregular_pulse{false};
  bool pulse_rate_range{false};
  bool measurement_position{false};
  bool multiple_bond{false};
  uint16_t raw{0};
};

// False when the payload is not two bytes. Reserved bits are kept in `raw` and
// otherwise ignored - a device setting one is not malformed, it is newer.
[[nodiscard]] bool parse_standard_bp_features(std::span<const uint8_t> data, StandardBpFeatures &features);

[[nodiscard]] bool decode_ieee11073_sfloat(uint16_t raw, float &value);
StandardBpError parse_standard_blood_pressure_measurement(std::span<const uint8_t> data,
                                                          StandardBloodPressureMeasurement &measurement,
                                                          bool reject_trailing_data = true);
const char *standard_bp_error_to_string(StandardBpError error);

}  // namespace esphome::omron
