#pragma once

// What an entity is told, as plain values: no entity pointers, no Component, no
// ESPHome. Keep it that way - omron_entities.h pulls in ESPHome's sensor
// headers, and anything declared there is out of reach of the host suite.

#include <cstdint>
#include <string>

namespace esphome::omron {

// The catalog owns the real per-model count; this header stays free of profile
// knowledge, so omron_entities.cpp asserts the two numbers agree.
inline constexpr uint8_t OMRON_ENTITY_USER_SLOTS = 2;

enum class OmronEntityCapability : uint32_t {
  SYSTOLIC = 1U << 0,
  DIASTOLIC = 1U << 1,
  PULSE = 1U << 2,
  TIMESTAMP = 1U << 3,
  USER = 1U << 4,
  SEQUENCE = 1U << 5,
  PROFILE = 1U << 6,
  STATUS = 1U << 7,
  CUFF_FIT = 1U << 8,
  BODY_MOVEMENT = 1U << 9,
  IRREGULAR_PULSE = 1U << 10,
  IMPROPER_POSITION = 1U << 11,
  BATTERY = 1U << 12,
  RSSI = 1U << 13,
  POLL_DURATION = 1U << 14,
  CONNECTION = 1U << 15,
  // Settings, not measurements: read out of the cuff's own configuration on
  // every session, so they publish without a record having been taken.
  BIRTH_DATE = 1U << 16,
  SETTINGS_VERSION = 1U << 17,
  // Consecutive measurement, artifact detection and IHB detection, all three
  // decoded out of a record.
  CONSECUTIVE_MEASUREMENT = 1U << 18,
  ARTIFACT_DETECTION = 1U << 19,
  IHB_DETECTION = 1U << 20,
};

using OmronEntityCapabilities = uint32_t;

constexpr OmronEntityCapabilities entity_capability(OmronEntityCapability capability) {
  return static_cast<OmronEntityCapabilities>(capability);
}

constexpr OmronEntityCapabilities operator|(OmronEntityCapability lhs, OmronEntityCapability rhs) {
  return entity_capability(lhs) | entity_capability(rhs);
}

constexpr OmronEntityCapabilities operator|(OmronEntityCapabilities lhs, OmronEntityCapability rhs) {
  return lhs | entity_capability(rhs);
}

// Where each capability is published, as four disjoint groups.
//
// The grouping is not documentation. omron_entities.cpp builds its publishing
// tables from these masks and refuses to compile when a capability in a group
// has no row, which is the one failure this layer cannot otherwise detect: the
// entity is configured, the record carries the field, and nothing is ever sent.
// That file is also one of the two the host suite cannot compile, so a runtime
// check there would be seen by nothing.

// Decoded from one measurement and belonging to the person who took it.
inline constexpr OmronEntityCapabilities OMRON_CAPABILITIES_USER_RECORD =
    OmronEntityCapability::SYSTOLIC | OmronEntityCapability::DIASTOLIC | OmronEntityCapability::PULSE |
    OmronEntityCapability::TIMESTAMP | OmronEntityCapability::USER | OmronEntityCapability::SEQUENCE |
    OmronEntityCapability::CUFF_FIT | OmronEntityCapability::BODY_MOVEMENT | OmronEntityCapability::IRREGULAR_PULSE |
    OmronEntityCapability::IMPROPER_POSITION | OmronEntityCapability::CONSECUTIVE_MEASUREMENT |
    OmronEntityCapability::ARTIFACT_DETECTION | OmronEntityCapability::IHB_DETECTION;

// Read out of the cuff's stored configuration rather than off a measurement,
// and still per person.
inline constexpr OmronEntityCapabilities OMRON_CAPABILITIES_USER_SETTINGS =
    OmronEntityCapability::BIRTH_DATE | OmronEntityCapability::SETTINGS_VERSION;

// Arrives on a record but describes the cuff, so it is published once rather
// than into every person's set.
inline constexpr OmronEntityCapabilities OMRON_CAPABILITIES_CUFF_RECORD =
    entity_capability(OmronEntityCapability::BATTERY);

// The cuff, the link and the poll. Published with the value as an argument,
// none of them from a record.
inline constexpr OmronEntityCapabilities OMRON_CAPABILITIES_CUFF =
    OmronEntityCapability::PROFILE | OmronEntityCapability::STATUS | OmronEntityCapability::RSSI |
    OmronEntityCapability::POLL_DURATION | OmronEntityCapability::CONNECTION;

inline constexpr OmronEntityCapabilities OMRON_CAPABILITIES_ALL =
    OMRON_CAPABILITIES_USER_RECORD | OMRON_CAPABILITIES_USER_SETTINGS | OMRON_CAPABILITIES_CUFF_RECORD |
    OMRON_CAPABILITIES_CUFF;

static_assert((OMRON_CAPABILITIES_USER_RECORD & OMRON_CAPABILITIES_USER_SETTINGS) == 0 &&
                  (OMRON_CAPABILITIES_USER_RECORD & OMRON_CAPABILITIES_CUFF_RECORD) == 0 &&
                  (OMRON_CAPABILITIES_USER_RECORD & OMRON_CAPABILITIES_CUFF) == 0 &&
                  (OMRON_CAPABILITIES_USER_SETTINGS & OMRON_CAPABILITIES_CUFF_RECORD) == 0 &&
                  (OMRON_CAPABILITIES_USER_SETTINGS & OMRON_CAPABILITIES_CUFF) == 0 &&
                  (OMRON_CAPABILITIES_CUFF_RECORD & OMRON_CAPABILITIES_CUFF) == 0,
              "a capability in two groups would publish twice or into the wrong entity set");

// The groups are contiguous from bit 0, so this is the whole enum exactly when
// nothing has been added past the last member without being placed above.
static_assert(OMRON_CAPABILITIES_ALL == (entity_capability(OmronEntityCapability::IHB_DETECTION) << 1) - 1,
              "a capability was added without being given a publishing group");

// Transport-neutral handoff from a validated protocol record to the entity
// layer. `available` describes fields present in this specific record. It is
// intersected with the selected profile's capability mask before publication.
struct OmronEntityData {
  OmronEntityCapabilities available{0};

  uint16_t systolic{0};
  uint16_t diastolic{0};
  uint16_t pulse{0};
  uint8_t user{0};
  uint32_t sequence{0};
  // Which reading of a consecutive series this is, which on an X4 is the
  // TruRead index. Two bits, so 0 to 3, and 0 on a single measurement.
  uint8_t consecutive_measurement{0};
  // Four bits each, sharing one byte.
  uint8_t artifact_detection{0};
  uint8_t ihb_detection{0};

  // ISO-8601 without a zone: the cuff's own wall clock, whose offset is unknown.
  std::string timestamp{};

  // Three of these four are problem-true; cuff_fit is inverted and carries no
  // device_class because of it. Set means the cuff was wrapped CORRECTLY, and a
  // loose wrap is reported by clearing the bit rather than setting it. It also
  // reads set on ordinary readings, which no other interpretation explains.
  bool cuff_fit{false};
  bool body_movement{false};
  bool irregular_pulse{false};
  // Standard characteristic only, bit 5 of the SIG measurement status. No
  // stored record carries a position field on any model here; the bit this was
  // once fed from is the consecutive-measurement index.
  bool improper_position{false};
  bool battery_low{false};

  bool has(OmronEntityCapability capability) const { return (this->available & entity_capability(capability)) != 0; }

  void mark_available(OmronEntityCapability capability) { this->available |= entity_capability(capability); }
};

// What the cuff stores about a person, as against what it measured off them.
// Kept apart from OmronEntityData because these belong to no record: they come
// from the settings region and are read on every session. The birth date is the
// only value in the device this component chooses, so reading it back is the
// proof that a registration write survived.
struct OmronUserSettingsData {
  OmronEntityCapabilities available{0};

  // ISO-8601 date, no time and no zone: a birth date has neither.
  std::string birth_date{};
  uint32_t version{0};

  bool has(OmronEntityCapability capability) const { return (this->available & entity_capability(capability)) != 0; }

  void mark_available(OmronEntityCapability capability) { this->available |= entity_capability(capability); }
};

}  // namespace esphome::omron
