#pragma once

// One outgoing command on its way to the link, fragment by fragment.
//
// A classic profile spreads a command over four TX characteristics, sixteen
// bytes at a time, resuming at the next one after every acknowledgement; a
// modern profile has one channel and sends the whole thing at once. The classic
// half of the catalog declares the four-channel transport, and the host tests
// are the only thing that exercises it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esphome::omron {

struct WriteFragment {
  uint8_t channel{0};
  size_t offset{0};
  size_t length{0};
  bool last{true};
};

class OmronCommandWriter {
 public:
  // Bytes one TX characteristic takes on a multi-channel profile. A command
  // longer than channels * 16 has nowhere left to go.
  static constexpr size_t CHANNEL_STRIDE = 16;

  // `channel_count` is the profile's TX characteristic count. The unlock
  // characteristic is always a channel of its own, whatever the profile says.
  void begin(std::span<const uint8_t> data, uint8_t channel_count, bool single_channel);

  bool active() const { return !this->command_.empty(); }
  bool complete() const { return this->offset_ >= this->command_.size(); }
  const uint8_t *data() const { return this->command_.data(); }
  // esp_ble_gattc_write_char takes uint8_t* though it does not write through
  // it. Better one explained overload here than a const_cast at every call.
  uint8_t *data() { return this->command_.data(); }

  // The fragment to put on the wire now. False when the command has run out of
  // channels: a configuration fault rather than a link failure, and it must not
  // be reported as a write that failed.
  bool next(WriteFragment &fragment) const;

  void advance(const WriteFragment &fragment) { this->offset_ += fragment.length; }

  void clear();

 private:
  std::vector<uint8_t> command_{};
  size_t offset_{0};
  uint8_t channel_count_{1};
  // Whole command on one characteristic: the unlock channel, and any profile
  // whose transport declares a single TX channel.
  bool single_channel_{true};
};

}  // namespace esphome::omron
