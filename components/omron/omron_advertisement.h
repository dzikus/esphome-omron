#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace esphome::omron {

inline constexpr uint16_t OMRON_COMPANY_ID = 0x020E;

// What a cuff says about itself while advertising, which is what decides
// whether a session is possible at all: the cuff sleeps with its radio off and
// only advertises when it has a reason to talk.
struct OmronAdvertisementFlags {
  uint8_t format{0};
  uint8_t user_register_count{0};
  bool invalid_time{false};
  bool pairing_mode{false};
  bool forced_transfer{false};

  // One two-byte sequence number per user slot: which person has a new reading,
  // readable without connecting. A slot never written reads zero.
  static constexpr uint8_t MAX_USER_SEQUENCES = 4;
  uint8_t sequence_count{0};
  // Bounds-checked, because the length that fills it comes from the peer.
  std::array<uint16_t, MAX_USER_SEQUENCES> user_sequence{};

  // The cuff is asking to be talked to. Connecting outside this hammers a
  // sleeping device and logs pairing timeouts that say nothing about the peer.
  bool wants_session() const { return this->pairing_mode || this->invalid_time || this->forced_transfer; }
};

// Decodes the manufacturer payload, company identifier already stripped. False
// for a format this does not recognise, which is deliberately not the same as
// "no flags set": an unknown format must not read as an invitation.
[[nodiscard]] bool parse_advertisement_flags(std::span<const uint8_t> payload, OmronAdvertisementFlags &flags);

}  // namespace esphome::omron
