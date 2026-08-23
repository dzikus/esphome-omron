#include "omron_profile_adapter.h"

namespace esphome::omron {

MemoryByteOrder cursor_memory_order(const OmronProfile &profile) {
  switch (profile.cursor_byte_order) {
    case CursorByteOrder::BIG:
      return MemoryByteOrder::BIG;
    case CursorByteOrder::LITTLE:
      return MemoryByteOrder::LITTLE;
    case CursorByteOrder::SAME_AS_RECORD:
      break;
  }
  return profile.byte_order == ByteOrder::BIG ? MemoryByteOrder::BIG : MemoryByteOrder::LITTLE;
}

ProfileAdapterError make_poll_layout(const OmronProfile &profile, PollLayout &layout, uint8_t history_records) {
  if (profile.id == OmronProfileId::UNSUPPORTED || profile.record_format == RecordFormat::UNSUPPORTED)
    return ProfileAdapterError::UNSUPPORTED_PROFILE;
  if (profile.user_count == 0 || profile.user_count > profile.users.size() || profile.user_count > layout.users.size())
    return ProfileAdapterError::INVALID_USER_COUNT;
  if (profile.settings_index_region_size == 0)
    return ProfileAdapterError::INVALID_INDEX_SIZE;
  if (profile.transmission_block_size == 0)
    return ProfileAdapterError::INVALID_BLOCK_SIZE;
  if (profile.record_size == 0)
    return ProfileAdapterError::INVALID_RECORD_LAYOUT;

  layout = {};
  layout.index_address = profile.settings_read_address;
  layout.index_size = profile.settings_index_region_size;
  layout.transfer_block_size = profile.transmission_block_size;
  // Resolved against the deepest ring this profile has, then trimmed per user at
  // plan time by the cursor. One field for every user is enough because that
  // trim is what decides how far each one actually reaches.
  if (history_records == HISTORY_RECORDS_ALL) {
    uint16_t deepest = 1;
    for (uint8_t user = 0; user < profile.user_count && user < profile.users.size(); user++)
      deepest = profile.users[user].record_count > deepest ? profile.users[user].record_count : deepest;
    layout.backtrack_records = static_cast<uint16_t>(deepest - 1);
  } else {
    layout.backtrack_records = history_records;
  }

  // The profile states the clock as a window inside the settings block, so the
  // address only exists once that block's own address is added to it.
  if (profile.clock_fields_offset != NO_CLOCK && profile.time_region_end > profile.time_region_start) {
    const uint16_t size = static_cast<uint16_t>(profile.time_region_end - profile.time_region_start);
    if (size <= 0xFF) {
      layout.clock_address = static_cast<uint16_t>(profile.settings_read_address + profile.time_region_start);
      layout.clock_size = static_cast<uint8_t>(size);
    }
  }

  for (uint8_t user_index = 0; user_index < profile.user_count; user_index++) {
    const OmronUserMemoryLayout &source = profile.users[user_index];
    if (source.record_count == 0 || static_cast<uint16_t>(source.write_cursor_offset) + 2 > layout.index_size)
      return ProfileAdapterError::INVALID_RECORD_LAYOUT;

    UserPollLayout &target = layout.users[user_index];
    target.enabled = true;
    target.cursor_offset = source.write_cursor_offset;
    target.cursor_width = 2;
    target.cursor_order = cursor_memory_order(profile);
    target.ring.records_address = source.record_start_address;
    target.ring.record_count = source.record_count;
    target.ring.record_size = profile.record_size;
    target.ring.cursor_mask = source.write_cursor_mask;
    target.ring.cursor_bias = source.slot_index_bias;
    target.ring.slot_min = 0;
    target.ring.slot_max = static_cast<uint16_t>(source.record_count - 1);
  }
  return ProfileAdapterError::NONE;
}

const char *profile_adapter_error_to_string(ProfileAdapterError error) {
  switch (error) {
    case ProfileAdapterError::NONE:
      return "none";
    case ProfileAdapterError::UNSUPPORTED_PROFILE:
      return "unsupported profile";
    case ProfileAdapterError::INVALID_USER_COUNT:
      return "invalid user count";
    case ProfileAdapterError::INVALID_INDEX_SIZE:
      return "invalid index size";
    case ProfileAdapterError::INVALID_BLOCK_SIZE:
      return "invalid transfer block size";
    case ProfileAdapterError::INVALID_RECORD_LAYOUT:
      return "invalid record layout";
  }
  return "unknown";
}

}  // namespace esphome::omron
