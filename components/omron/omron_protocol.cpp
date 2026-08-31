#include "omron_protocol.h"

#include <algorithm>
#include <cstddef>

namespace esphome::omron {

static constexpr uint8_t REQUEST_WRITE_HIGH = 0x01;
static constexpr uint8_t REQUEST_WRITE_LOW = 0xC0;
static constexpr size_t RESPONSE_PAYLOAD_OFFSET = 6;
static constexpr size_t LEGACY_CHANNEL_WIDTH = 16;

std::vector<uint8_t> make_write_request(uint16_t address, std::span<const uint8_t> data) {
  std::vector<uint8_t> request;
  if (data.empty() || data.size() + WRITE_REQUEST_OVERHEAD > 0xFF)
    return request;

  request.reserve(data.size() + WRITE_REQUEST_OVERHEAD);
  request.push_back(static_cast<uint8_t>(data.size() + WRITE_REQUEST_OVERHEAD));
  request.push_back(REQUEST_WRITE_HIGH);
  request.push_back(REQUEST_WRITE_LOW);
  request.push_back(static_cast<uint8_t>(address >> 8));
  request.push_back(static_cast<uint8_t>(address & 0xFF));
  request.push_back(static_cast<uint8_t>(data.size()));
  request.insert(request.end(), data.begin(), data.end());
  request.push_back(0x00);
  request.push_back(xor_bytes(request));
  return request;
}

ProtocolError parse_response(std::span<const uint8_t> frame, ResponseFrame &response) {
  if (frame.size() < READ_RESPONSE_OVERHEAD)
    return ProtocolError::FRAME_TOO_SHORT;
  if (frame[0] != frame.size())
    return ProtocolError::LENGTH_MISMATCH;
  if (xor_bytes(frame) != 0)
    return ProtocolError::CHECKSUM_MISMATCH;

  const uint16_t raw_type = static_cast<uint16_t>((static_cast<uint16_t>(frame[1]) << 8) | frame[2]);
  if (raw_type != static_cast<uint16_t>(PacketType::START_RESPONSE) &&
      raw_type != static_cast<uint16_t>(PacketType::READ_RESPONSE) &&
      raw_type != static_cast<uint16_t>(PacketType::WRITE_RESPONSE) &&
      raw_type != static_cast<uint16_t>(PacketType::END_RESPONSE))
    return ProtocolError::UNEXPECTED_COMMAND;

  // Byte 5 only means "payload length" in a read response. A start response
  // echoes the requested transfer block size there instead: a real HEM-7155T
  // answers our 08 00 00 00 00 10 00 18 with 08 80 00 00 00 10 00 98, where the
  // 0x10 is the block size coming back, not sixteen bytes of payload in an
  // eight-byte frame. Reading it as a length rejected every start response and
  // the session never got past the handshake.
  size_t payload_length = 0;
  if (raw_type == static_cast<uint16_t>(PacketType::READ_RESPONSE)) {
    payload_length = frame[5];
    // The declared payload must fit, but it need not fill the frame. Both
    // references slice the payload out by declared length and ignore whatever
    // follows, so trailing padding is normal device behaviour.
    if (READ_RESPONSE_OVERHEAD + payload_length > frame.size())
      return ProtocolError::PAYLOAD_LENGTH_MISMATCH;
  }

  response.type = static_cast<PacketType>(raw_type);
  response.address = static_cast<uint16_t>((static_cast<uint16_t>(frame[FRAME_ADDRESS_OFFSET]) << 8) |
                                           frame[FRAME_ADDRESS_OFFSET + 1]);
  response.status = raw_type == static_cast<uint16_t>(PacketType::END_RESPONSE) ? frame[6] : 0;
  response.data.clear();
  if (payload_length != 0) {
    const std::span<const uint8_t> payload = frame.subspan(RESPONSE_PAYLOAD_OFFSET, payload_length);
    response.data.assign(payload.begin(), payload.end());
  }
  return ProtocolError::NONE;
}

ProtocolError validate_token_response(std::span<const uint8_t> response, const std::array<uint8_t, 4> &nonce) {
  if (response.size() < 6)
    return ProtocolError::INVALID_TOKEN_RESPONSE;
  if (response[0] != 0x91 || response[1] != 0x00)
    return ProtocolError::INVALID_TOKEN_RESPONSE;
  if (!std::ranges::equal(nonce, response.subspan(2, nonce.size())))
    return ProtocolError::INVALID_TOKEN_RESPONSE;
  return ProtocolError::NONE;
}

void OmronFrameAssembler::reset() {
  this->clear_fragments_();
  this->frame_.clear();
  this->error_ = ProtocolError::NONE;
}

void OmronFrameAssembler::clear_fragments_() {
  for (auto &fragment : this->fragments_)
    fragment.clear();
  this->present_.fill(false);
}

AssembleResult OmronFrameAssembler::add_fragment(uint8_t channel, std::span<const uint8_t> data) {
  if (data.empty() || this->channel_count_ == 0 || this->channel_count_ > this->fragments_.size() ||
      channel >= this->channel_count_ || channel >= this->fragments_.size()) {
    this->error_ = ProtocolError::INVALID_CHANNEL;
    return AssembleResult::ERROR;
  }
  if (this->channel_count_ > 1 && data.size() > LEGACY_CHANNEL_WIDTH) {
    this->error_ = ProtocolError::FRAGMENT_TOO_LARGE;
    return AssembleResult::ERROR;
  }
  this->fragments_[channel].assign(data.begin(), data.end());
  this->present_[channel] = true;
  return this->try_assemble_();
}

AssembleResult OmronFrameAssembler::try_assemble_() {
  if (!this->present_[0])
    return AssembleResult::INCOMPLETE;
  if (this->fragments_[0].empty() || this->fragments_[0][0] < READ_RESPONSE_OVERHEAD) {
    this->error_ = ProtocolError::FRAME_TOO_SHORT;
    return AssembleResult::ERROR;
  }

  const size_t frame_size = this->fragments_[0][0];
  const size_t required_channels =
      this->channel_count_ == 1 ? 1 : (frame_size + LEGACY_CHANNEL_WIDTH - 1) / LEGACY_CHANNEL_WIDTH;
  if (required_channels > this->channel_count_ || required_channels > this->fragments_.size()) {
    this->error_ = ProtocolError::LENGTH_MISMATCH;
    return AssembleResult::ERROR;
  }
  for (size_t channel = 0; channel < required_channels; channel++) {
    if (!this->present_[channel])
      return AssembleResult::INCOMPLETE;
  }

  this->frame_.clear();
  this->frame_.reserve(frame_size);
  for (size_t channel = 0; channel < required_channels && this->frame_.size() < frame_size; channel++) {
    const size_t remaining = frame_size - this->frame_.size();
    const size_t take = this->fragments_[channel].size() < remaining ? this->fragments_[channel].size() : remaining;
    this->frame_.insert(this->frame_.end(), this->fragments_[channel].begin(),
                        this->fragments_[channel].begin() + static_cast<std::ptrdiff_t>(take));
  }
  if (this->frame_.size() != frame_size) {
    this->error_ = ProtocolError::LENGTH_MISMATCH;
    return AssembleResult::ERROR;
  }
  // A completed response consumes all channel fragments. Keeping them would
  // let a later channel-0 notification combine with stale higher channels.
  this->clear_fragments_();
  if (xor_bytes(this->frame_) != 0) {
    this->error_ = ProtocolError::CHECKSUM_MISMATCH;
    return AssembleResult::ERROR;
  }
  this->error_ = ProtocolError::NONE;
  return AssembleResult::COMPLETE;
}

const char *protocol_error_to_string(ProtocolError error) {
  switch (error) {
    case ProtocolError::NONE:
      return "none";
    case ProtocolError::FRAME_TOO_SHORT:
      return "frame too short";
    case ProtocolError::LENGTH_MISMATCH:
      return "length mismatch";
    case ProtocolError::CHECKSUM_MISMATCH:
      return "checksum mismatch";
    case ProtocolError::UNEXPECTED_COMMAND:
      return "unexpected command";
    case ProtocolError::PAYLOAD_LENGTH_MISMATCH:
      return "payload length mismatch";
    case ProtocolError::INVALID_CHANNEL:
      return "invalid channel";
    case ProtocolError::FRAGMENT_TOO_LARGE:
      return "fragment too large";
    case ProtocolError::INVALID_UNLOCK_RESPONSE:
      return "invalid unlock response";
    case ProtocolError::UNLOCK_KEY_REJECTED:
      return "device rejected the bind key";
    case ProtocolError::INVALID_TOKEN_RESPONSE:
      return "invalid token response";
    case ProtocolError::DEVICE_REPORTED_ERROR:
      return "device reported an error";
    case ProtocolError::RETRY_EXHAUSTED:
      return "retry attempts exhausted";
    case ProtocolError::STRAY_FRAME:
      return "frame ignored; the transaction was not waiting on it";
  }
  return "unknown protocol error";
}

}  // namespace esphome::omron
