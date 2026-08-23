#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace esphome::omron {

enum class MemoryByteOrder : uint8_t { LITTLE = 0, BIG };

struct MemoryBlock {
  uint16_t address{0};
  std::vector<uint8_t> data{};
};

struct RingLayout {
  uint16_t records_address{0};
  uint16_t record_count{0};
  uint8_t record_size{0};
  uint16_t cursor_mask{0x00FF};
  int8_t cursor_bias{-1};
  uint16_t slot_min{0};
  uint16_t slot_max{0};
};

class OmronMemoryImage {
 public:
  void clear();
  bool add_block(uint16_t address, std::span<const uint8_t> data);
  bool contains(uint16_t address, size_t length) const;
  bool read(uint16_t address, std::span<uint8_t> destination) const;
  std::vector<uint8_t> read(uint16_t address, size_t length) const;
  const std::vector<MemoryBlock> &blocks() const { return this->blocks_; }

 private:
  std::vector<MemoryBlock> blocks_{};
};

// [[nodiscard]] throughout below: each of these writes its result through a
// reference and answers whether it did. Dropped, the caller reads whatever the
// out-parameter held before the call - which for an address or a cursor is a
// plausible number pointing somewhere nobody chose.
[[nodiscard]] bool read_integer(std::span<const uint8_t> data, size_t width, MemoryByteOrder order, uint32_t &value);
[[nodiscard]] bool normalize_write_cursor(uint32_t raw_cursor, const RingLayout &layout, uint16_t &latest_slot);
[[nodiscard]] bool record_address(const RingLayout &layout, uint16_t slot, uint16_t &address);
std::vector<uint16_t> newest_first_slots(uint32_t raw_cursor, const RingLayout &layout, uint16_t limit = 0);

uint32_t measurement_fingerprint(std::string_view profile, uint8_t user, uint16_t address,
                                 std::span<const uint8_t> record);

}  // namespace esphome::omron
