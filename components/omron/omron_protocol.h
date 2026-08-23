#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esphome::omron {

// [[nodiscard]] because this component writes to a blood-pressure monitor's
// permanent memory: dropping one of these on the floor is a compile error.
enum class [[nodiscard]] ProtocolError : uint8_t {
  NONE = 0,
  FRAME_TOO_SHORT,
  LENGTH_MISMATCH,
  CHECKSUM_MISMATCH,
  UNEXPECTED_COMMAND,
  PAYLOAD_LENGTH_MISMATCH,
  INVALID_CHANNEL,
  FRAGMENT_TOO_LARGE,
  INVALID_UNLOCK_RESPONSE,
  UNLOCK_KEY_REJECTED,
  INVALID_TOKEN_RESPONSE,
  RETRY_EXHAUSTED,
  // The cuff answered the command it was asked for and refused what was sent.
  // Not a protocol violation on either side.
  DEVICE_REPORTED_ERROR,
  // Well formed, but for something this transaction is not waiting on, so it
  // was counted and dropped and the transaction did not move.
  //
  // Distinct from NONE on purpose: told "no error", the caller reads a stray as
  // progress, clears its wait and re-sends a command that is still in flight,
  // without touching the attempt counter meant to bound that.
  STRAY_FRAME,
};

enum class PacketType : uint16_t {
  START_REQUEST = 0x0000,
  READ_REQUEST = 0x0100,
  WRITE_REQUEST = 0x01C0,
  END_REQUEST = 0x0F00,
  START_RESPONSE = 0x8000,
  READ_RESPONSE = 0x8100,
  // A write is acknowledged, and the acknowledgement echoes the address it
  // landed on. Both have to match before a write counts as done.
  WRITE_RESPONSE = 0x81C0,
  END_RESPONSE = 0x8F00,
};

enum class AssembleResult : uint8_t {
  INCOMPLETE = 0,
  COMPLETE,
  ERROR,
};

struct ResponseFrame {
  PacketType type{PacketType::START_RESPONSE};
  uint16_t address{0};
  uint8_t status{0};
  std::vector<uint8_t> data{};
};

// Six header bytes, one trailing pad, one checksum. The same shape in both
// directions, so a read reply carrying N bytes of payload is N + 8 on the wire.
inline constexpr size_t WRITE_REQUEST_OVERHEAD = 8;
inline constexpr size_t READ_RESPONSE_OVERHEAD = 8;
// Where the address sits in every request and every response, big endian in
// both directions whatever the endianness of the records behind it.
inline constexpr size_t FRAME_ADDRESS_OFFSET = 3;

uint8_t xor_bytes(std::span<const uint8_t> data);

std::array<uint8_t, 8> make_start_request();
std::array<uint8_t, 8> make_read_request(uint16_t address, uint8_t length);
std::array<uint8_t, 8> make_end_request();
// Empty on refusal. Callers must not choose the address freely: use the
// purpose-built wrapper that derives it from the profile.
std::vector<uint8_t> make_write_request(uint16_t address, std::span<const uint8_t> data);

ProtocolError parse_response(std::span<const uint8_t> frame, ResponseFrame &response);

// The session token: twenty bytes, an opcode, four of nonce and padding, and a
// reply that echoes the nonce back. Stateless, so a fresh nonce each session is
// the whole of it. Named for what it is rather than for a model, because every
// profile on the modern stack opens this way.
std::array<uint8_t, 20> make_token_request(const std::array<uint8_t, 4> &nonce);
ProtocolError validate_token_response(std::span<const uint8_t> response, const std::array<uint8_t, 4> &nonce);

class OmronFrameAssembler {
 public:
  explicit OmronFrameAssembler(uint8_t channel_count = 1) : channel_count_(channel_count) {}

  void reset();
  AssembleResult add_fragment(uint8_t channel, std::span<const uint8_t> data);
  const std::vector<uint8_t> &frame() const { return frame_; }
  ProtocolError error() const { return error_; }

 private:
  AssembleResult try_assemble_();
  void clear_fragments_();

  uint8_t channel_count_{1};
  std::array<std::vector<uint8_t>, 4> fragments_{};
  std::array<bool, 4> present_{{false, false, false, false}};
  std::vector<uint8_t> frame_{};
  ProtocolError error_{ProtocolError::NONE};
};

const char *protocol_error_to_string(ProtocolError error);

}  // namespace esphome::omron
