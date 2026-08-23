#pragma once

#include <cstdint>

#include "omron_poll_plan.h"
#include "omron_profiles.h"

namespace esphome::omron {

enum class [[nodiscard]] ProfileAdapterError : uint8_t {
  NONE = 0,
  UNSUPPORTED_PROFILE,
  INVALID_USER_COUNT,
  INVALID_INDEX_SIZE,
  INVALID_BLOCK_SIZE,
  INVALID_RECORD_LAYOUT,
};

// Resolves CursorByteOrder::SAME_AS_RECORD against the profile's record order.
MemoryByteOrder cursor_memory_order(const OmronProfile &profile);

// Read as deep as the ring goes. Not a magic number standing in for "lots":
// the plan is trimmed to the slots the cursor says were actually written, so
// this only ever means "do not stop early".
//
// Deliberately not "fetch only what the cuff reports as outstanding", however
// reasonable that sounds. The unsent counter can read cleared while the cursor
// still accounts for records nobody has collected.
inline constexpr uint8_t HISTORY_RECORDS_ALL = 0xFF;

// history_records is how many slots older than the newest to include per user.
// Zero reads one record per person, which is all the entities can show; a higher
// number drains the ring, and those records leave as events.
ProfileAdapterError make_poll_layout(const OmronProfile &profile, PollLayout &layout,
                                     uint8_t history_records = HISTORY_RECORDS_ALL);
const char *profile_adapter_error_to_string(ProfileAdapterError error);

}  // namespace esphome::omron
