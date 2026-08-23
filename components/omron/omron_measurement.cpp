#include "omron_measurement.h"

#include <algorithm>
#include <cstdio>

#include "omron_metrics.h"
#include "omron_protocol.h"

namespace esphome::omron {

static bool is_leap_year(uint16_t year) {
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

static uint8_t days_in_month(uint16_t year, uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12)
    return 0;
  if (month == 2 && is_leap_year(year))
    return 29;
  return DAYS[month - 1];
}

std::string format_date(const OmronDateTime &value) {
  char buffer[16];
  // Date alone: a birth date has no time of day, and a midnight suffix would be
  // three fields of invented precision.
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", static_cast<unsigned>(value.year),
                static_cast<unsigned>(value.month), static_cast<unsigned>(value.day));
  return buffer;
}

std::string format_datetime(const OmronDateTime &value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u", static_cast<unsigned>(value.year),
                static_cast<unsigned>(value.month), static_cast<unsigned>(value.day), static_cast<unsigned>(value.hour),
                static_cast<unsigned>(value.minute), static_cast<unsigned>(value.second));
  return buffer;
}

bool is_valid_datetime(const OmronDateTime &value) {
  if (value.year == 0 || value.month < 1 || value.month > 12)
    return false;
  const uint8_t maximum_day = days_in_month(value.year, value.month);
  if (value.day < 1 || value.day > maximum_day)
    return false;
  return value.hour <= 23 && value.minute <= 59 && value.second <= 59;
}

static bool is_uniform(std::span<const uint8_t> data, uint8_t value) {
  return std::ranges::all_of(data, [value](uint8_t byte) { return byte == value; });
}

static bool measurement_values_valid(const OmronMeasurement &value) {
  // Rejects partially initialized EEPROM slots before publication.
  return plausible_vitals(value.systolic_mm_hg, value.diastolic_mm_hg, value.pulse_bpm);
}

// Coarse corruption bounds. The lower one treats a pre-2010 slot as stale or
// uninitialised memory rather than a reading. The upper one exists
// because latest-record selection is a max-by-timestamp: without it, a single
// corrupt slot decoding to a far-future date outranks every real reading for
// good and freezes the published measurement. A tight "not after now" check
// needs a trusted runtime clock, which this pure layer deliberately has no
// access to; this bound is what can be enforced without one.
static constexpr uint16_t MEASUREMENT_MIN_YEAR = 2010;
static constexpr uint16_t MEASUREMENT_MAX_YEAR = 2099;

static bool measurement_datetime_valid(const OmronDateTime &value) {
  return value.year >= MEASUREMENT_MIN_YEAR && value.year <= MEASUREMENT_MAX_YEAR && is_valid_datetime(value);
}

static MeasurementParseError parse_classic_vital_14(std::span<const uint8_t> data, OmronMeasurement &measurement) {
  if (data.size() < 8)
    return MeasurementParseError::LENGTH_MISMATCH;

  const uint8_t raw_systolic = data[0];
  if (raw_systolic > 0xE1)
    return MeasurementParseError::EMPTY_SLOT;

  const uint16_t flags1 = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  const uint16_t flags2 = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
  if (data[1] == 0 && data[2] == 0 && (data[3] & 0x3F) == 0 && flags1 == 0 && flags2 == 0)
    return MeasurementParseError::EMPTY_SLOT;

  OmronMeasurement candidate{};
  candidate.systolic_mm_hg = static_cast<uint16_t>(raw_systolic) + 25;
  candidate.diastolic_mm_hg = data[1];
  candidate.pulse_bpm = data[2];
  candidate.timestamp.year = static_cast<uint16_t>(2000 + (data[3] & 0x3F));
  candidate.timestamp.hour = static_cast<uint8_t>(flags1 & 0x1F);
  candidate.timestamp.day = static_cast<uint8_t>((flags1 >> 5) & 0x1F);
  candidate.timestamp.month = static_cast<uint8_t>((flags1 >> 10) & 0x0F);
  candidate.irregular_heartbeat = ((flags1 >> 14) & 0x01) != 0;
  candidate.movement_detected = ((flags1 >> 15) & 0x01) != 0;

  const uint8_t raw_second = static_cast<uint8_t>(flags2 & 0x3F);
  const uint8_t raw_minute = static_cast<uint8_t>((flags2 >> 6) & 0x3F);
  candidate.timestamp.second = raw_second > 59 ? 59 : raw_second;
  candidate.timestamp.minute = raw_minute > 59 ? 59 : raw_minute;
  candidate.cuff_flag = ((flags2 >> 12) & 0x01) != 0;
  candidate.battery_flag = ((flags2 >> 13) & 0x01) != 0;
  candidate.consecutive_measurement = static_cast<uint8_t>((flags2 >> 14) & 0x03);
  // Byte 9, two nibbles: artifact low, IHB high.
  if (data.size() > 9) {
    candidate.artifact_detection = static_cast<uint8_t>(data[9] & 0x0F);
    candidate.ihb_detection = static_cast<uint8_t>((data[9] >> 4) & 0x0F);
  }

  // The record number is not read here. It comes from the offset the profile
  // carries, applied in parse_measurement_record; the last two bytes are the
  // checksum on this hardware, not a sequence number.

  if (!measurement_datetime_valid(candidate.timestamp))
    return MeasurementParseError::INVALID_DATE;
  if (!measurement_values_valid(candidate))
    return MeasurementParseError::INVALID_MEASUREMENT;

  measurement = candidate;
  return MeasurementParseError::NONE;
}

// The HEM-6401T family. The date is six plain bytes at the head of the record
// and the reading follows it, which is the whole difference from
// CLASSIC_VITAL_14: no bit packing beyond the two status fields.
//
//   0 year+2000 | 1 month | 2 day | 3 hour | 4 minute | 5 second
//   6 systolic+25 | 7 diastolic | 8 pulse | 9 artifact | 10 IHB
//   11 bits 0-1 movement, bits 2-3 irregular, bits 4-7 a positioning value
//   12 an internal temperature, signed
//
// Cuff fit and the battery bit are left clear on purpose: this family carries
// neither. The positioning nibble in byte 11 is left undecoded rather than
// folded into a flag it is not: four bits with no readable scale.
static MeasurementParseError parse_plain_date_vital(std::span<const uint8_t> data, OmronMeasurement &measurement) {
  static constexpr size_t FIELDS_END = 13;
  if (data.size() < FIELDS_END)
    return MeasurementParseError::LENGTH_MISMATCH;

  const uint8_t raw_systolic = data[6];
  if (raw_systolic > 0xE1)
    return MeasurementParseError::EMPTY_SLOT;
  if (is_uniform(data.first(9), 0x00))
    return MeasurementParseError::EMPTY_SLOT;

  OmronMeasurement candidate{};
  candidate.timestamp.year = static_cast<uint16_t>(2000 + data[0]);
  candidate.timestamp.month = data[1];
  candidate.timestamp.day = data[2];
  candidate.timestamp.hour = data[3];
  const uint8_t raw_minute = data[4];
  const uint8_t raw_second = data[5];
  candidate.timestamp.minute = raw_minute > 59 ? 59 : raw_minute;
  candidate.timestamp.second = raw_second > 59 ? 59 : raw_second;
  candidate.systolic_mm_hg = static_cast<uint16_t>(raw_systolic) + 25;
  candidate.diastolic_mm_hg = data[7];
  candidate.pulse_bpm = data[8];

  // Two bits each, not one, and only a 1 means the flag is set: this family
  // uses 2 for "no value", so a non-zero test reports a missing measurement as
  // a warning.
  //
  // Movement is the low pair and the irregular beat the high one. Easy to swap,
  // and swapped in more than one place that documents this family.
  candidate.movement_detected = (data[11] & 0x03) == 1;
  candidate.irregular_heartbeat = ((data[11] >> 2) & 0x03) == 1;
  // Whole bytes here rather than the nibbles the packed families use.
  candidate.artifact_detection = data[9];
  candidate.ihb_detection = data[10];

  if (!measurement_datetime_valid(candidate.timestamp))
    return MeasurementParseError::INVALID_DATE;
  if (!measurement_values_valid(candidate))
    return MeasurementParseError::INVALID_MEASUREMENT;

  measurement = candidate;
  return MeasurementParseError::NONE;
}

MeasurementParseError parse_measurement_record(std::span<const uint8_t> data, const OmronProfile &profile,
                                               OmronMeasurement &measurement) {
  if (profile.record_format == RecordFormat::UNSUPPORTED)
    return MeasurementParseError::UNSUPPORTED_FORMAT;
  if (profile.record_size == 0 || data.size() != profile.record_size)
    return MeasurementParseError::LENGTH_MISMATCH;
  if (is_uniform(data, 0x00) || is_uniform(data, 0xFF))
    return MeasurementParseError::EMPTY_SLOT;

  MeasurementParseError error = MeasurementParseError::UNSUPPORTED_FORMAT;
  switch (profile.record_format) {
    case RecordFormat::CLASSIC_VITAL_14:
      error = parse_classic_vital_14(data, measurement);
      break;
    case RecordFormat::CLASSIC_VITAL_24_GUARDED:
      // Byte 17 gates the three values, as a condition on the fields themselves
      // rather than as a flag: a non-zero byte means the slot carries no
      // reading at all, not a reading with a warning.
      if (data.size() < 18)
        return MeasurementParseError::LENGTH_MISMATCH;
      if (data[17] != 0)
        return MeasurementParseError::INVALID_MEASUREMENT;
      error = parse_classic_vital_14(data, measurement);
      break;
    case RecordFormat::PLAIN_DATE_VITAL:
      error = parse_plain_date_vital(data, measurement);
      break;
    case RecordFormat::UNSUPPORTED:
      break;
  }
  if (error != MeasurementParseError::NONE)
    return error;

  // The record's own number, two bytes little-endian at the offset the profile
  // states. Kept out of the format parsers because it varies per model rather
  // than per layout: the same CLASSIC_VITAL_14 records appear on models that
  // carry the number and on models that do not.
  const uint8_t sequence_offset = profile.record_sequence_offset;
  if (sequence_offset != NO_RECORD_SEQUENCE && static_cast<size_t>(sequence_offset) + 2 <= data.size()) {
    measurement.has_record_id = true;
    measurement.record_id =
        static_cast<uint16_t>(data[sequence_offset]) | (static_cast<uint16_t>(data[sequence_offset + 1]) << 8);
  }
  return MeasurementParseError::NONE;
}

const char *measurement_parse_error_to_string(MeasurementParseError error) {
  switch (error) {
    case MeasurementParseError::NONE:
      return "none";
    case MeasurementParseError::LENGTH_MISMATCH:
      return "record length mismatch";
    case MeasurementParseError::EMPTY_SLOT:
      return "empty record slot";
    case MeasurementParseError::UNSUPPORTED_FORMAT:
      return "unsupported record format";
    case MeasurementParseError::INVALID_DATE:
      return "invalid record date";
    case MeasurementParseError::INVALID_MEASUREMENT:
      return "invalid measurement values";
  }
  return "unknown measurement parse error";
}

int64_t civil_seconds(const OmronDateTime &value) {
  // Days from civil date, shifting the year so March starts the era and leap
  // days land at the end of it.
  int64_t year = value.year;
  const int64_t month = value.month;
  const int64_t day = value.day;
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t year_of_era = year - era * 400;
  const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const int64_t days = era * 146097 + day_of_era - 719468;
  return days * 86400 + value.hour * 3600 + value.minute * 60 + value.second;
}

ClockParseError parse_device_clock(std::span<const uint8_t> data, uint8_t fields_offset, OmronDateTime &clock) {
  if (fields_offset == NO_CLOCK)
    return ClockParseError::UNSUPPORTED_LAYOUT;

  // Six chronological fields, then the checksum that closes the window two
  // bytes from its end. Both are per profile rather than fixed: the fields
  // start where that model's definition says, and the window is whatever the
  // time region spans.
  const size_t fields_at = fields_offset;
  if (data.size() < 2)
    return ClockParseError::LENGTH_MISMATCH;
  const size_t checksum_at = data.size() - 2;
  if (fields_at + 6 > checksum_at)
    return ClockParseError::LENGTH_MISMATCH;

  uint32_t sum = 0;
  for (size_t i = 0; i < checksum_at; i++)
    sum += data[i];
  if ((sum & 0xFF) != data[checksum_at])
    return ClockParseError::CHECKSUM_MISMATCH;

  // Filled before the range check so a caller can still show what the cuff
  // holds. A never-set clock reads as 2019-01-01 00:00:63 with a correct
  // checksum: the window is right, the seconds field is simply not a time.
  clock.year = static_cast<uint16_t>(2000 + data[fields_at]);
  clock.month = data[fields_at + 1];
  clock.day = data[fields_at + 2];
  clock.hour = data[fields_at + 3];
  clock.minute = data[fields_at + 4];
  clock.second = data[fields_at + 5];
  if (!is_valid_datetime(clock))
    return ClockParseError::INVALID_DATE;

  return ClockParseError::NONE;
}

const char *clock_parse_error_to_string(ClockParseError error) {
  switch (error) {
    case ClockParseError::NONE:
      return "none";
    case ClockParseError::UNSUPPORTED_LAYOUT:
      return "unsupported clock layout";
    case ClockParseError::LENGTH_MISMATCH:
      return "clock window too short";
    case ClockParseError::CHECKSUM_MISMATCH:
      return "clock checksum mismatch";
    case ClockParseError::INVALID_DATE:
      return "invalid clock date";
  }
  return "unknown clock parse error";
}

}  // namespace esphome::omron
