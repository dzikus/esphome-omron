#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace esphome::omron {

inline constexpr size_t OMRON_MAX_GATT_CHANNELS = 4;
inline constexpr size_t OMRON_MAX_USERS = 2;

// A record with no sequence number of its own. Not 0, which is a real offset.
inline constexpr uint8_t NO_RECORD_SEQUENCE = 0xFF;

enum class SecurityMode : uint8_t {
  NONE = 0,
  CUSTOM_KEY,
  OS_BOND,
};

// Whether a bond is worth keeping between sessions.
//
// No profile ships PER_SESSION, however tempting it looks: a cuff that seems to
// invalidate its side of the bond after every session is really discarding the
// bond of a host that never registered itself. Register, and one bond carries
// session after session.
//
// Kept reachable through `keep_bond: false`, and kept as the safer default: a
// stale bond fails once and is remade, while dropping a bond that did not need
// dropping costs a walk to the device and a held button every time.
enum class BondPolicy : uint8_t {
  NONE = 0,
  PERSISTENT,
  PER_SESSION,
};

enum class UnlockMode : uint8_t {
  NONE = 0,
  CLASSIC_KEY,
  TOKEN_KEY,
};

enum class ByteOrder : uint8_t {
  BIG = 0,
  LITTLE,
};

// The index pointer need not agree with the record fields, and on a good third
// of this catalog it does not: those models byte-swap the index data in pairs,
// which for a 16-bit field is a big-endian read.
enum class CursorByteOrder : uint8_t {
  SAME_AS_RECORD = 0,
  BIG,
  LITTLE,
};

enum class RecordFormat : uint8_t {
  UNSUPPORTED = 0,
  // All but seven models in this catalog, however differently they spell the
  // offsets. Do not add bit-packed variants of it: the ones that circulate
  // decode a layout no model here uses, and read diastolic where systolic
  // lives.
  CLASSIC_VITAL_14,
  // The 24-byte record of the HEM-9601T family. Bytes 0-11 are CLASSIC_VITAL_14
  // to the bit; byte 17 is what makes it its own format, marking a slot that
  // holds a failed measurement, so the pressures are valid only when it reads
  // zero.
  CLASSIC_VITAL_24_GUARDED,
  // The HEM-6401T family, which writes the date as six plain bytes ahead of the
  // reading instead of packing it into the flag words. The same 13 bytes
  // whether the record is 16 or 32 long.
  PLAIN_DATE_VITAL,
};

// Which of the flag fields a model's records actually carry.
//
// Per model, never all-of-them: an absent field reads as a permanent OFF, which
// is indistinguishable from a correct answer, so a model that reports no cuff
// fit must not get a "Cuff fit" entity. Movement and irregular are the only two
// every model in the catalog carries; the other four each go missing somewhere.
using MeasurementFields = uint8_t;
inline constexpr MeasurementFields MEASUREMENT_FIELD_CUFF = 1 << 0;
inline constexpr MeasurementFields MEASUREMENT_FIELD_MOVEMENT = 1 << 1;
inline constexpr MeasurementFields MEASUREMENT_FIELD_IRREGULAR = 1 << 2;
inline constexpr MeasurementFields MEASUREMENT_FIELD_CONSECUTIVE = 1 << 3;
inline constexpr MeasurementFields MEASUREMENT_FIELD_ARTIFACT = 1 << 4;
inline constexpr MeasurementFields MEASUREMENT_FIELD_IHB = 1 << 5;
inline constexpr MeasurementFields MEASUREMENT_FIELDS_ALL =
    MEASUREMENT_FIELD_CUFF | MEASUREMENT_FIELD_MOVEMENT | MEASUREMENT_FIELD_IRREGULAR | MEASUREMENT_FIELD_CONSECUTIVE |
    MEASUREMENT_FIELD_ARTIFACT | MEASUREMENT_FIELD_IHB;

// Battery is deliberately not one of these. No model here is known to describe
// such a field, and the bit this component reads for it is unsourced. The
// entity stays because it reads zero everywhere, and removing it would close
// the only route to ever finding out whether it means anything.

// A model whose clock this component cannot locate. Not 0, which is a real
// offset: one family puts the fields at the very start of their block.
inline constexpr uint8_t NO_CLOCK = 0xFF;

enum class OmronProfileId : uint8_t {
  UNSUPPORTED = 0,
  HEM_6161T,
  HEM_6232T,
  HEM_7142T2,
  HEM_7146T,
  HEM_7151T,
  HEM_7155T,
  HEM_7155T_MW,
  HEM_7155T_K4,
  HEM_7155T_MW3,
  HEM_7320T,
  HEM_7322T,
  HEM_7342T,
  HEM_7530T,
  HEM_7600T,
  HEM_6231T,
  HEM_6320T,
  HEM_6321T,
  HEM_7136T,
  HEM_7150T,
  HEM_7188T1,
  HEM_7361T,
  HEM_7380T1,
  HEM_7382T1,
  HEM_7386T1,
  // Each of these looks like a variant of a neighbouring family and is not.
  // Filing it there decodes every slot at the wrong address.
  HEM_1026T2,
  HEM_7188T1_LE,
  HEM_7196T1,
  HEM_7377T1,
  HEM_7511T,
  // Not HEM-7600T with a 14-byte record at 0x02AC, however they are usually
  // listed: settings 0x0356, records at 0x041A with a stride of 24, one user.
  HEM_9601T,
  // One family, two maps: these devices measure five things at once and the two
  // groups keep blood pressure at different addresses with different record
  // sizes.
  HEM_6401T,
  HEM_6410T,
  // Each split from a family whose other variants share its memory map exactly
  // and disagree only on ring depth, which is the ceiling on a read no yaml
  // option caps.
  HEM_716BT2_DEEP,
  HEM_7157T_DEEP,
  HEM_7600T_DEEP,
  HEM_9700T,
  // Single-user rings in the WLD4.0 lineage. Both also keep a large block of
  // some other measurement at 0x010000, so picking the record area by size
  // rather than by what it carries lands on the wrong one.
  HEM_7191T1,
  HEM_7440T1,
};

// How much a profile's memory map is worth trusting. HARDWARE_VERIFIED requires
// a reading pulled off a physical cuff, and exactly one profile has earned it:
// hem_7155t_mw3. REFERENCE_TESTED means another project reports the model
// working. REFERENCE_ONLY means nobody in the chain has confirmed the numbers
// on that device.
//
// REFERENCE_ONLY is the zero value on purpose, so a profile that forgets to
// declare one is treated as the least trustworthy rather than the most.
enum class OmronProfileConfidence : uint8_t {
  REFERENCE_ONLY = 0,
  REFERENCE_TESTED,
  HARDWARE_VERIFIED,
};

const char *profile_confidence_to_string(OmronProfileConfidence confidence);

struct OmronGattCapabilities {
  const char *parent_service_uuid;
  std::array<const char *, OMRON_MAX_GATT_CHANNELS> rx_channel_uuids;
  uint8_t rx_channel_count;
  std::array<const char *, OMRON_MAX_GATT_CHANNELS> tx_channel_uuids;
  uint8_t tx_channel_count;
  const char *unlock_characteristic_uuid;
};

struct OmronUserMemoryLayout {
  uint16_t record_start_address;
  uint16_t record_count;
  uint8_t write_cursor_offset;
  uint8_t unread_counter_offset;
  uint16_t write_cursor_mask;
  int8_t slot_index_bias;
};

struct OmronProfile {
  OmronProfileId id;
  const char *model;
  const OmronGattCapabilities *gatt;

  SecurityMode security_mode;
  BondPolicy bond_policy;
  UnlockMode unlock_mode;
  bool token_required;

  ByteOrder byte_order;
  CursorByteOrder cursor_byte_order;
  RecordFormat record_format;
  uint8_t record_size;
  // Response frame size less the header and check bytes. For most families
  // that is 64 - 6 - 2 = 56, or 0x38, which is what the cuff here reads. The
  // value 0x10 circulates for several of these families with no source behind
  // it; treat a short read as the first sign of that.
  uint8_t transmission_block_size;

  uint16_t settings_read_address;
  // Two aliases onto one buffer, not two buffers: a write at this base comes
  // back on the next connection's read at the other, byte for byte. Whether a
  // write aimed at the read base would be accepted has never been tried.
  uint16_t settings_write_address;
  uint8_t settings_index_region_size;
  // Size of one user's settings block. Eight layouts exist across the catalog,
  // and every variant of a given profile shares one.
  //
  // Offsets fall out of it rather than needing a second field: user N starts at
  // settings_index_region_size + (N-1) * this, which gives 24 and 34 on the
  // cuff measured here. Zero means the layout is unknown and no block may be
  // located.
  uint8_t user_block_size;
  // Where the six clock fields begin inside the time region, or NO_CLOCK.
  //
  // Every model whose definition describes a clock states the same six
  // one-byte fields in the same order - year, month, day, hour, minute,
  // second - and differs only in how far into the block they start. So this is
  // one number per model rather than a set of named layouts: the names were a
  // guess at variation that the definitions do not contain, and under them
  // five models that share the common offset had their clock switched off
  // while one that does not had it written four bytes early.
  //
  // NO_CLOCK for a model whose definition places no clock at all. The region
  // below still bounds the user blocks, so it stays either way.
  uint8_t clock_fields_offset;
  uint8_t time_region_start;
  uint8_t time_region_end;

  std::array<OmronUserMemoryLayout, OMRON_MAX_USERS> users;
  uint8_t user_count;

  const char *const *equivalent_model_ids;
  size_t equivalent_model_id_count;

  // Seconds of clock drift this model tolerates before its clock is refreshed.
  // Zero means every session. Stated per profile, so a new entry cannot inherit
  // a number nobody looked up.
  int64_t clock_sync_threshold_s;

  // Byte offset of the record's own sequence number inside the record, stated
  // per model. Two bytes little-endian wherever it exists, and every model that
  // has one puts it at 10. NO_RECORD_SEQUENCE where a model has none: zero is a
  // real offset, so absence needs a sentinel rather than a falsy value.
  //
  // Do not read the record's last two bytes instead, however plausible that
  // looks: on this hardware byte 14 is the checksum over bytes 0-13, so an
  // entity fed from there publishes a checksum and looks entirely credible.
  uint8_t record_sequence_offset;

  // The union across this profile's variants. Six families disagree internally,
  // all of them about IHB, and taking the union is the choice that keeps a real
  // field visible rather than hiding it on the variants that have it. The
  // alternative would be splitting those families over one flag, which would
  // buy nothing: their memory maps are identical.
  MeasurementFields measurement_fields;

  // Every profile must state this: the host tests build with
  // -Werror=missing-field-initializers, so a new entry cannot quietly inherit a
  // confidence it did not earn.
  OmronProfileConfidence confidence;
};

extern const OmronGattCapabilities OMRON_CLASSIC_GATT;
extern const OmronGattCapabilities OMRON_MODERN_GATT;

const OmronProfile &get_profile(OmronProfileId id);
const OmronProfile *profile_for_model(std::string_view model);
const OmronProfile *profile_at(size_t index);
size_t profile_count();

bool profile_matches_model(const OmronProfile &profile, std::string_view model);

constexpr bool requires_os_bond(const OmronProfile &profile) {
  return profile.security_mode == SecurityMode::OS_BOND;
}

constexpr bool requires_protocol_unlock(const OmronProfile &profile) {
  return profile.unlock_mode != UnlockMode::NONE;
}

constexpr bool remove_bond_after_session(const OmronProfile &profile) {
  return profile.bond_policy == BondPolicy::PER_SESSION;
}

}  // namespace esphome::omron
