#include "omron_standard_bp.h"

#include <cmath>

namespace esphome::omron {

static uint16_t read_u16_le(std::span<const uint8_t> data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

bool decode_ieee11073_sfloat(uint16_t raw, float &value) {
  // Reserved IEEE-11073 values: NaN, +/-Inf and NRes.
  if (raw == 0x07FF || raw == 0x0800 || raw == 0x07FE || raw == 0x0802 || raw == 0x0801)
    return false;

  int16_t mantissa = static_cast<int16_t>(raw & 0x0FFF);
  if (mantissa >= 0x0800)
    mantissa = static_cast<int16_t>(mantissa - 0x1000);
  int8_t exponent = static_cast<int8_t>((raw >> 12) & 0x0F);
  if (exponent >= 0x08)
    exponent = static_cast<int8_t>(exponent - 0x10);
  value = static_cast<float>(mantissa) * std::pow(10.0f, static_cast<float>(exponent));
  return std::isfinite(value);
}

static bool valid_timestamp(const StandardBpTimestamp &timestamp) {
  // Year 0, month 0 and day 0 all mean "unknown" in Date Time. BLS says a cuff
  // should not send month 0 or day 0, but a cuff that does is telling us the
  // date is unavailable, not that the reading is corrupt. Callers drop the
  // timestamp and keep the measurement; see parse_standard_blood_pressure_measurement.
  if (timestamp.year == 0 || timestamp.month == 0 || timestamp.day == 0)
    return false;
  if (timestamp.year < 1582 || timestamp.year > 9999 || timestamp.month > 12 || timestamp.hour > 23 ||
      timestamp.minute > 59 || timestamp.second > 59)
    return false;
  static const uint8_t DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t max_day = DAYS_PER_MONTH[timestamp.month - 1];
  const bool leap = (timestamp.year % 4 == 0 && timestamp.year % 100 != 0) || timestamp.year % 400 == 0;
  if (timestamp.month == 2 && leap)
    max_day = 29;
  return timestamp.day <= max_day;
}

StandardBpError parse_standard_blood_pressure_measurement(std::span<const uint8_t> data,
                                                          StandardBloodPressureMeasurement &measurement,
                                                          bool reject_trailing_data) {
  if (data.size() < 7)
    return StandardBpError::TOO_SHORT;

  const uint8_t flags = data[0];
  // Walked as a shrinking view rather than an index into the whole packet: every
  // optional field below is guarded by how much is left, and a length check
  // written against the original size has to subtract an offset to say so.
  std::span<const uint8_t> rest = data.subspan(1);
  measurement = {};
  measurement.units_kpa = (flags & 0x01) != 0;
  // A reserved SFLOAT here means "this cuff does not report this subfield", not
  // "the packet is corrupt". Rejecting the whole notification over a NaN mean
  // arterial pressure throws away systolic, diastolic and pulse on every device
  // that omits it, which is most of them.
  measurement.has_systolic = decode_ieee11073_sfloat(read_u16_le(rest), measurement.systolic);
  measurement.has_diastolic = decode_ieee11073_sfloat(read_u16_le(rest.subspan(2)), measurement.diastolic);
  measurement.has_mean_arterial_pressure =
      decode_ieee11073_sfloat(read_u16_le(rest.subspan(4)), measurement.mean_arterial_pressure);
  rest = rest.subspan(6);
  // Only give up when the packet carries no pressure at all.
  if (!measurement.has_systolic && !measurement.has_diastolic)
    return StandardBpError::INVALID_SFLOAT;

  if ((flags & 0x02) != 0) {
    if (rest.size() < 7)
      return StandardBpError::TRUNCATED_OPTIONAL_FIELD;
    measurement.timestamp.year = read_u16_le(rest);
    measurement.timestamp.month = rest[2];
    measurement.timestamp.day = rest[3];
    measurement.timestamp.hour = rest[4];
    measurement.timestamp.minute = rest[5];
    measurement.timestamp.second = rest[6];
    rest = rest.subspan(7);
    // An unusable date is dropped, not fatal: the reading itself is still good.
    measurement.has_timestamp = valid_timestamp(measurement.timestamp);
    if (!measurement.has_timestamp)
      measurement.timestamp = {};
  }
  if ((flags & 0x04) != 0) {
    if (rest.size() < 2)
      return StandardBpError::TRUNCATED_OPTIONAL_FIELD;
    measurement.has_pulse_rate = decode_ieee11073_sfloat(read_u16_le(rest), measurement.pulse_rate);
    rest = rest.subspan(2);
  }
  if ((flags & 0x08) != 0) {
    if (rest.empty())
      return StandardBpError::TRUNCATED_OPTIONAL_FIELD;
    measurement.has_user_id = true;
    measurement.user_id = rest[0];
    rest = rest.subspan(1);
  }
  if ((flags & 0x10) != 0) {
    if (rest.size() < 2)
      return StandardBpError::TRUNCATED_OPTIONAL_FIELD;
    measurement.has_measurement_status = true;
    measurement.measurement_status = read_u16_le(rest);
    rest = rest.subspan(2);
  }
  if (reject_trailing_data && !rest.empty())
    return StandardBpError::TRAILING_DATA;
  return StandardBpError::NONE;
}

bool parse_standard_bp_features(std::span<const uint8_t> data, StandardBpFeatures &features) {
  // Exactly two bytes. The characteristic is fixed width in the specification,
  // and a longer read is a device doing something this parser has not been shown
  // rather than a wider field to sign up for.
  if (data.size() != 2)
    return false;
  const uint16_t raw = static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
  features.raw = raw;
  features.body_movement = (raw & 0x0001) != 0;
  features.cuff_fit = (raw & 0x0002) != 0;
  features.irregular_pulse = (raw & 0x0004) != 0;
  features.pulse_rate_range = (raw & 0x0008) != 0;
  features.measurement_position = (raw & 0x0010) != 0;
  features.multiple_bond = (raw & 0x0020) != 0;
  return true;
}

const char *standard_bp_error_to_string(StandardBpError error) {
  switch (error) {
    case StandardBpError::NONE:
      return "none";
    case StandardBpError::TOO_SHORT:
      return "too short";
    case StandardBpError::TRUNCATED_OPTIONAL_FIELD:
      return "truncated optional field";
    case StandardBpError::INVALID_SFLOAT:
      return "invalid SFLOAT";
    case StandardBpError::TRAILING_DATA:
      return "trailing data";
  }
  return "unknown";
}

}  // namespace esphome::omron
