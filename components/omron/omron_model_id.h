#pragma once

// Which cuff is on the other end, from what it reports about itself.
//
// A wrong profile does not fail. It reads a different area and returns numbers
// that look like blood pressure, and nothing downstream catches that. Since one
// trade name can cover two memory maps, this answers "I do not know" rather
// than guessing: it may agree with or contradict a configured profile, never
// choose one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "omron_profiles.h"

namespace esphome::omron {

// Which of the two proprietary GATT stacks the cuff exposed. Free to determine:
// discovery runs every session anyway and the two service UUIDs are disjoint.
enum class OmronStack : uint8_t {
  UNKNOWN = 0,
  CLASSIC,
  MODERN,
};

enum class ModelIdentity : uint8_t {
  // Says nothing at all, and in particular not that the configured profile is
  // wrong.
  UNKNOWN = 0,
  EXACT,
  // A trade name whose candidate models agree about the memory map, or whose
  // disagreement the discovered stack settled.
  RESOLVED,
  // A known trade name whose candidates disagree and the stack did not separate.
  AMBIGUOUS,
};

struct ModelIdentification {
  ModelIdentity identity{ModelIdentity::UNKNOWN};
  // Set for EXACT and RESOLVED, null otherwise. Never a "best guess": a null
  // here means nothing may be concluded.
  const OmronProfile *profile{nullptr};
  // Both are logged: "one of three, and we know one" is a weaker statement
  // than "one of one".
  uint8_t candidates{0};
  uint8_t known_candidates{0};
};

enum class ProfileVerdict : uint8_t {
  // Nothing to compare, which is also the answer for UNKNOWN and AMBIGUOUS.
  UNVERIFIED = 0,
  CONFIRMED,
  // A different profile over the same memory map. Common: HEM-7155T-MW3 and
  // HEM-7155T-K4 are separate entries over one map.
  COMPATIBLE,
  // The identified model reads a different area, or cuts records differently.
  // This is the failure the file exists for.
  MISMATCH,
};

// Whether two profiles would read the same bytes and mean the same thing by
// them. Deliberately not operator==: the transport block size, the bond policy
// and the confidence level all differ between profiles that address memory
// identically, and none of them can turn a reading into a wrong number.
bool same_record_memory_map(const OmronProfile &a, const OmronProfile &b);

// `reported` is the DIS 0x2A24 string as it came off the wire, trailing padding
// and all. `stack` may be UNKNOWN, which only costs the six trade names that
// need it.
ModelIdentification identify_model(std::string_view reported, OmronStack stack);

ProfileVerdict verify_configured_profile(const OmronProfile &configured, const ModelIdentification &identification);

const char *model_identity_to_string(ModelIdentity identity);

// The name this profile answers to in yaml, derived from its model id: lower
// case, dashes to underscores. A derivation rather than a second list that can
// drift out of step with the codegen table.
//
// Detection is mostly used to find out what to configure, and a log line saying
// "HEM-7155T-K4" leaves the reader guessing at the spelling of a key that has
// to match exactly.
std::string profile_config_key(const OmronProfile &profile);

}  // namespace esphome::omron
