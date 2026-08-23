#include "omron_unlock.h"

#include <algorithm>

namespace esphome::omron {

static OmronUnlockFrame make_key_command(uint8_t command, const OmronBindKey &key) {
  OmronUnlockFrame frame{};
  frame[0] = command;
  std::ranges::copy(key, frame.begin() + 1);
  return frame;
}

OmronUnlockFrame make_key_auth_request(const OmronBindKey &key) {
  return make_key_command(0x01, key);
}

OmronUnlockFrame make_confirm_encryption_request() {
  return make_key_command(0x02, OmronBindKey{});
}

OmronUnlockFrame make_program_key_request(const OmronBindKey &key) {
  return make_key_command(0x00, key);
}

const char *unlock_status_to_string(uint8_t status) {
  switch (status) {
    case 0x00:
      return "accepted";
    case 0x01:
      return "bind key does not match the one stored on the device";
    case 0x04:
      return "security precondition not met";
    default:
      return "unknown status";
  }
}

const char *encryption_status_to_string(uint8_t status) {
  switch (status) {
    case 0x00:
      return "up";
    case 0x01:
      return "cancelled";
    case 0x08:
    case 0x17:
      return "peer lost its pairing information";
    case 0x0F:
      return "timed out";
    default:
      return "failed";
  }
}

UnlockReply classify_unlock_reply(std::span<const uint8_t> data) {
  if (data.empty())
    return UnlockReply::INVALID;
  // A single-byte reply carries no status, so treat the opcode echo alone as
  // the answer rather than inventing a rejection the device never sent.
  const bool rejected = data.size() >= 2 && data[1] != 0x00;
  switch (data[0]) {
    case 0x80:
      return rejected ? UnlockReply::KEY_REJECTED : UnlockReply::KEY_PROGRAMMED;
    case 0x81:
      return rejected ? UnlockReply::KEY_REJECTED : UnlockReply::KEY_ACCEPTED;
    case 0x82:
      // Deliberately not gated on byte 1. The status there describes the link
      // encryption, not the command, and the session continues either way.
      return UnlockReply::ENCRYPTION_CONFIRMED;
    case 0x91:
      return data.size() >= 2 && data[1] == 0x00 ? UnlockReply::TOKEN_ACCEPTED : UnlockReply::INVALID;
    default:
      return UnlockReply::INVALID;
  }
}

}  // namespace esphome::omron
