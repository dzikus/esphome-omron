#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "omron_memory.h"
#include "omron_transaction.h"

namespace esphome::omron {

struct UserPollLayout {
  bool enabled{false};
  uint8_t cursor_offset{0};
  uint8_t cursor_width{2};
  MemoryByteOrder cursor_order{MemoryByteOrder::LITTLE};
  RingLayout ring{};
};

struct PollLayout {
  uint16_t index_address{0};
  uint8_t index_size{0};
  uint8_t transfer_block_size{0x10};
  // Sixteen bits because a ring can be a thousand slots deep (HEM-9700T) and an
  // uncapped read reaches all of it. A uint8_t here stops at 254 in silence.
  uint16_t backtrack_records{2};
  // The cuff's own clock, inside the settings block rather than at an address
  // of its own: the profile's time region is an offset into that block.
  uint16_t clock_address{0};
  uint8_t clock_size{0};
  std::array<UserPollLayout, 4> users{};
};

struct UserRecordPlan {
  uint8_t user{0};
  uint32_t raw_cursor{0};
  std::vector<uint16_t> slots{};
  std::vector<ReadBlock> reads{};
};

[[nodiscard]] bool build_index_read(const PollLayout &layout, ReadRange &range);
[[nodiscard]] bool build_record_plan(const PollLayout &layout, std::span<const uint8_t> index_data,
                                     std::vector<UserRecordPlan> &plans);

}  // namespace esphome::omron
