#include "omron_poll_plan.h"

#include <algorithm>
#include <utility>

namespace esphome::omron {

// 240 = 15 records of 16 bytes, the most a uint8_t ReadBlock length holds
// without overflowing. Records are never larger than 32 bytes in this catalog,
// so this always covers at least seven of them.
static constexpr uint16_t MAX_MERGED_READ = 240;

bool build_index_read(const PollLayout &layout, ReadRange &range) {
  if (layout.index_size == 0 || layout.transfer_block_size == 0)
    return false;
  range = {layout.index_address, layout.index_size, layout.transfer_block_size};
  return true;
}

static bool add_slot_read(const UserPollLayout &user, uint16_t slot, std::vector<ReadBlock> &reads) {
  uint16_t address = 0;
  if (!record_address(user.ring, slot, address))
    return false;

  if (!reads.empty()) {
    ReadBlock &previous = reads.back();
    const uint32_t previous_end = static_cast<uint32_t>(previous.address) + previous.length;
    const uint16_t combined = static_cast<uint16_t>(previous.length + user.ring.record_size);
    // Merge as far as the length field allows, not as far as one frame holds.
    // Capping at transfer_block_size made the plan carry 48-byte ranges - three
    // whole records, because a fourth would not fit 56 - and the transaction
    // then sent each of them as its own frame. Reads need not align to record
    // boundaries: ask for a stretch of the ring and let the 56-byte transfer
    // size cut it up, which is what extend_reads already does.
    //
    // MAX_MERGED_READ is what a uint8_t length can hold rounded down to a whole
    // record; a longer run simply starts another range and costs nothing.
    if (previous_end == address && combined <= MAX_MERGED_READ) {
      previous.length = static_cast<uint8_t>(combined);
      return true;
    }
  }
  reads.push_back({address, user.ring.record_size});
  return true;
}

bool build_record_plan(const PollLayout &layout, std::span<const uint8_t> index_data,
                       std::vector<UserRecordPlan> &plans) {
  if (index_data.size() < layout.index_size || layout.transfer_block_size == 0)
    return false;
  plans.clear();

  for (size_t user_index = 0; user_index < layout.users.size(); user_index++) {
    const UserPollLayout &user = layout.users[user_index];
    if (!user.enabled)
      continue;
    if (user.cursor_offset >= layout.index_size ||
        static_cast<size_t>(user.cursor_offset) + user.cursor_width > layout.index_size)
      return false;

    uint32_t raw_cursor = 0;
    if (!read_integer(index_data.subspan(user.cursor_offset), user.cursor_width, user.cursor_order, raw_cursor))
      return false;

    UserRecordPlan plan;
    plan.user = static_cast<uint8_t>(user_index);
    plan.raw_cursor = raw_cursor;
    uint16_t requested = static_cast<uint16_t>(1 + layout.backtrack_records);

    // The cursor also says how many records exist, and asking for more than
    // that walks into the part of the ring nobody has written. The cuff answers
    // such a read with a header and no payload, which the transaction can only
    // read as a length mismatch, killing a session that had already fetched
    // every real record of both users.
    const uint32_t masked = raw_cursor & user.ring.cursor_mask;
    const int32_t written = static_cast<int32_t>(masked) + user.ring.cursor_bias + 1;
    if (written > 0 && written < static_cast<int32_t>(user.ring.record_count) &&
        requested > static_cast<uint16_t>(written)) {
      requested = static_cast<uint16_t>(written);
    }
    plan.slots = newest_first_slots(raw_cursor, user.ring, requested);
    if (plan.slots.empty())
      return false;

    // The newest record goes out first, on its own, and the rest follow in
    // address order: one 16-byte read at the cursor's slot, then the ring from
    // its start. Costs one extra frame and buys the newest reading arriving
    // first, which is the one every entity shows.
    const uint16_t newest_slot = plan.slots.front();
    if (!add_slot_read(user, newest_slot, plan.reads))
      return false;

    // newest_first order normally walks backwards, so these reads are not
    // contiguous. Sort addresses for efficient transfer, while retaining the
    // original slot order above for newest-record selection.
    std::vector<uint16_t> sorted_slots = plan.slots;
    std::ranges::sort(sorted_slots);
    for (const uint16_t slot : sorted_slots) {
      if (slot == newest_slot)
        continue;
      if (!add_slot_read(user, slot, plan.reads))
        return false;
    }
    plans.push_back(std::move(plan));
  }
  return !plans.empty();
}

}  // namespace esphome::omron
