#include "omron_command_writer.h"

namespace esphome::omron {

void OmronCommandWriter::begin(std::span<const uint8_t> data, uint8_t channel_count, bool single_channel) {
  this->command_.assign(data.begin(), data.end());
  this->offset_ = 0;
  this->channel_count_ = channel_count == 0 ? 1 : channel_count;
  this->single_channel_ = single_channel || this->channel_count_ == 1;
}

void OmronCommandWriter::clear() {
  this->command_.clear();
  this->offset_ = 0;
}

bool OmronCommandWriter::next(WriteFragment &fragment) const {
  if (this->command_.empty() || this->offset_ >= this->command_.size())
    return false;

  const size_t remaining = this->command_.size() - this->offset_;
  if (this->single_channel_) {
    fragment.channel = 0;
    fragment.offset = this->offset_;
    fragment.length = remaining;
    fragment.last = true;
    return true;
  }

  // Which characteristic this stretch belongs on is the offset divided by the
  // stride, not a counter: a resumed command has to land on the same channel it
  // would have if every fragment had gone out in one pass.
  const size_t channel = this->offset_ / CHANNEL_STRIDE;
  if (channel >= this->channel_count_)
    return false;
  fragment.channel = static_cast<uint8_t>(channel);
  fragment.offset = this->offset_;
  fragment.length = remaining < CHANNEL_STRIDE ? remaining : CHANNEL_STRIDE;
  fragment.last = fragment.offset + fragment.length >= this->command_.size();
  return true;
}

}  // namespace esphome::omron
