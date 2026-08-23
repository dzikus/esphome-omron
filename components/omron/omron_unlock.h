#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace esphome::omron {

using OmronBindKey = std::array<uint8_t, 16>;
using OmronUnlockFrame = std::array<uint8_t, 17>;

enum class UnlockReply : uint8_t {
  INVALID = 0,
  KEY_PROGRAMMED,
  KEY_ACCEPTED,
  ENCRYPTION_CONFIRMED,
  TOKEN_ACCEPTED,
  KEY_REJECTED,
};

// Byte 1 of a key reply: 0x00 accepted, 0x01 key mismatch, 0x04 security
// precondition not met. Worth decoding rather than discarding - it is the one
// diagnostic the cuff hands over, and it separates a bonding problem from a key
// problem.
const char *unlock_status_to_string(uint8_t status);

// Byte 1 of the 0x82 reply is not a key status but the cuff's report on the
// link encryption it just brought up. Non-zero does not fail the session.
const char *encryption_status_to_string(uint8_t status);

// Opens every session, provisioning or not. The cuff answers 0x82 and drives
// the link encryption itself; nothing here asks the OS to encrypt.
OmronUnlockFrame make_confirm_encryption_request();

// Read-session authentication using an already provisioned classic bind key.
OmronUnlockFrame make_key_auth_request(const OmronBindKey &key);

// Writes this node's key into a user slot on the cuff, which only accepts it
// while its display shows the pairing prompt. Needs explicit user consent.
OmronUnlockFrame make_program_key_request(const OmronBindKey &key);

UnlockReply classify_unlock_reply(std::span<const uint8_t> data);

}  // namespace esphome::omron
