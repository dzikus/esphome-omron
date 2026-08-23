#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "omron_profiles.h"

namespace esphome::omron {

struct OmronDateTime {
  uint16_t year{0};
  uint8_t month{0};
  uint8_t day{0};
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t second{0};
};

struct OmronMeasurement {
  uint16_t systolic_mm_hg{0};
  uint16_t diastolic_mm_hg{0};
  uint16_t pulse_bpm{0};
  OmronDateTime timestamp{};

  bool irregular_heartbeat{false};
  bool movement_detected{false};
  // Set means the cuff was wrapped correctly, not that anything is wrong.
  // Omron reports its loose-cuff error when this reads zero. See OmronEntityData.
  bool cuff_flag{false};
  // Offset 6 bit 13, and unsourced: no model here is known to describe a
  // battery field. Published as "Battery low" all the same, so treat the entity
  // as unproven.
  bool battery_flag{false};
  // Which reading of a consecutive series this is, so the TruRead index on a
  // cuff that takes three in a row. Not a position code, whatever the reference
  // driver's label says: position is a separate field and this model has none.
  uint8_t consecutive_measurement{0};
  // Artifact and IHB detection, four bits each, sharing byte 9.
  uint8_t artifact_detection{0};
  uint8_t ihb_detection{0};

  bool has_record_id{false};
  uint16_t record_id{0};
};

enum class [[nodiscard]] MeasurementParseError : uint8_t {
  NONE = 0,
  LENGTH_MISMATCH,
  EMPTY_SLOT,
  UNSUPPORTED_FORMAT,
  INVALID_DATE,
  INVALID_MEASUREMENT,
};

bool is_valid_datetime(const OmronDateTime &value);

// ISO-8601, naive, no zone suffix.
//
// The cuff stores local wall time and says nothing about where it is, so a
// "+00:00" here would assert UTC and shift every reading by the local offset.
// Attaching a real zone needs a time source and belongs to the client.
std::string format_date(const OmronDateTime &value);
std::string format_datetime(const OmronDateTime &value);

// Seconds from a fixed civil epoch, computed arithmetically rather than through
// mktime. Both sides of a drift comparison are local wall clock, so the zone
// cancels and never has to be known; going through mktime made the answer
// depend on whether libc's TZ happened to be configured, which on a first run
// it was not: a cuff one second behind reported two hours ahead.
int64_t civil_seconds(const OmronDateTime &value);

MeasurementParseError parse_measurement_record(std::span<const uint8_t> data, const OmronProfile &profile,
                                               OmronMeasurement &measurement);

const char *measurement_parse_error_to_string(MeasurementParseError error);

enum class [[nodiscard]] ClockParseError : uint8_t {
  NONE = 0,
  UNSUPPORTED_LAYOUT,
  LENGTH_MISMATCH,
  CHECKSUM_MISMATCH,
  INVALID_DATE,
};

// Decodes the cuff's own clock out of the time window of its settings block.
// The window carries a trailing checksum over everything before it, which is
// the only integrity check available here: an unset clock reads as a valid date
// in 2019, so a bad decode looks exactly like a device nobody ever configured.
//
// `fields_offset` is the profile's, and NO_CLOCK refuses: a model whose
// definition places no clock has no offset to read one from, and picking the
// common one would decode six bytes of some other setting as a date.
ClockParseError parse_device_clock(std::span<const uint8_t> data, uint8_t fields_offset, OmronDateTime &clock);

const char *clock_parse_error_to_string(ClockParseError error);

}  // namespace esphome::omron
