#include "omron_memory.h"

#include <algorithm>
#include <ranges>

namespace esphome::omron {

void OmronMemoryImage::clear() {
  this->blocks_.clear();
}

bool OmronMemoryImage::add_block(uint16_t address, std::span<const uint8_t> data) {
  if (data.empty() || static_cast<uint32_t>(address) + data.size() > 0x10000UL)
    return false;
  this->blocks_.push_back({address, std::vector<uint8_t>(data.begin(), data.end())});
  return true;
}

bool OmronMemoryImage::contains(uint16_t address, size_t length) const {
  if (length == 0)
    return true;
  const uint32_t requested_end = static_cast<uint32_t>(address) + length;
  if (requested_end > 0x10000UL)
    return false;

  uint32_t covered_until = address;
  while (covered_until < requested_end) {
    uint32_t best_end = covered_until;
    for (const auto &block : this->blocks_) {
      const uint32_t block_start = block.address;
      const uint32_t block_end = block_start + block.data.size();
      if (block_start <= covered_until && block_end > best_end)
        best_end = block_end;
    }
    if (best_end == covered_until)
      return false;
    covered_until = best_end;
  }
  return true;
}

bool OmronMemoryImage::read(uint16_t address, std::span<uint8_t> destination) const {
  const size_t length = destination.size();
  if (length == 0)
    return true;
  if (!this->contains(address, length))
    return false;

  for (size_t offset = 0; offset < length; offset++) {
    const uint16_t current = static_cast<uint16_t>(address + offset);
    bool found = false;
    // Prefer the newest block when retry responses overlap an earlier block.
    for (const auto &block : this->blocks_ | std::views::reverse) {
      const uint32_t start = block.address;
      const uint32_t end = start + block.data.size();
      if (current >= start && current < end) {
        destination[offset] = block.data[current - start];
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

std::vector<uint8_t> OmronMemoryImage::read(uint16_t address, size_t length) const {
  std::vector<uint8_t> result(length);
  if (!this->read(address, result))
    result.clear();
  return result;
}

bool read_integer(std::span<const uint8_t> data, size_t width, MemoryByteOrder order, uint32_t &value) {
  if (width == 0 || width > sizeof(uint32_t) || data.size() < width)
    return false;
  value = 0;
  if (order == MemoryByteOrder::LITTLE) {
    for (size_t i = 0; i < width; i++)
      value |= static_cast<uint32_t>(data[i]) << (8 * i);
  } else {
    for (size_t i = 0; i < width; i++)
      value = (value << 8) | data[i];
  }
  return true;
}

bool normalize_write_cursor(uint32_t raw_cursor, const RingLayout &layout, uint16_t &latest_slot) {
  if (layout.record_count == 0 || layout.record_size == 0 || layout.slot_max < layout.slot_min)
    return false;
  const uint32_t masked = raw_cursor & layout.cursor_mask;
  const int32_t biased = static_cast<int32_t>(masked) + layout.cursor_bias;
  const int32_t span = static_cast<int32_t>(layout.slot_max - layout.slot_min + 1);
  if (span <= 0)
    return false;
  int32_t wrapped = (biased - layout.slot_min) % span;
  if (wrapped < 0)
    wrapped += span;
  latest_slot = static_cast<uint16_t>(layout.slot_min + wrapped);
  return true;
}

bool record_address(const RingLayout &layout, uint16_t slot, uint16_t &address) {
  if (slot < layout.slot_min || slot > layout.slot_max || layout.record_size == 0)
    return false;
  const uint32_t result =
      static_cast<uint32_t>(layout.records_address) + static_cast<uint32_t>(slot) * layout.record_size;
  if (result + layout.record_size > 0x10000UL)
    return false;
  address = static_cast<uint16_t>(result);
  return true;
}

std::vector<uint16_t> newest_first_slots(uint32_t raw_cursor, const RingLayout &layout, uint16_t limit) {
  std::vector<uint16_t> result;
  uint16_t latest = 0;
  if (!normalize_write_cursor(raw_cursor, layout, latest))
    return result;
  const uint32_t span = static_cast<uint32_t>(layout.slot_max) - layout.slot_min + 1U;
  const uint16_t available = static_cast<uint16_t>(std::min<uint32_t>(layout.record_count, span));
  const uint16_t count = limit == 0 ? available : std::min(limit, available);
  result.reserve(count);
  for (uint16_t offset = 0; offset < count; offset++) {
    const uint32_t relative =
        (static_cast<uint32_t>(latest) - layout.slot_min + span - (static_cast<uint32_t>(offset) % span)) % span;
    result.push_back(static_cast<uint16_t>(static_cast<uint32_t>(layout.slot_min) + relative));
  }
  return result;
}

static uint32_t fnv1a_byte(uint32_t hash, uint8_t value) {
  hash ^= value;
  return hash * 16777619UL;
}

uint32_t measurement_fingerprint(std::string_view profile, uint8_t user, uint16_t address,
                                 std::span<const uint8_t> record) {
  uint32_t hash = 2166136261UL;
  for (char value : profile)
    hash = fnv1a_byte(hash, static_cast<uint8_t>(value));
  hash = fnv1a_byte(hash, user);
  hash = fnv1a_byte(hash, static_cast<uint8_t>(address >> 8));
  hash = fnv1a_byte(hash, static_cast<uint8_t>(address & 0xFF));
  for (uint8_t value : record)
    hash = fnv1a_byte(hash, value);
  return hash;
}

}  // namespace esphome::omron
