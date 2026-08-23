#pragma once

// Everything this component writes into a cuff's settings region.
//
// Nothing here talks to a link: these turn a settings image plus an intent into
// frames, and sending them is omron_session's business.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "omron_measurement.h"
#include "omron_profiles.h"

namespace esphome::omron {

enum class [[nodiscard]] ClockWriteError : uint8_t {
  NONE = 0,
  NULL_ARGUMENT,
  UNSUPPORTED_LAYOUT,
  WRITE_UNSUPPORTED,
  WINDOW_LENGTH,
  INVALID_TIME,
  BUILD_FAILED,
};

// The address is derived from the profile and cannot be passed in, which holds
// for every builder here: records and every other setting share this address
// space, so an address computed at a call site is one slip away from destroying
// data that exists only on the device.
ClockWriteError build_clock_write_request(const OmronProfile &profile, std::span<const uint8_t> window,
                                          const OmronDateTime &target, std::vector<uint8_t> &request,
                                          uint16_t &address);

const char *clock_write_error_to_string(ClockWriteError error);

// One of the settings blocks a model keeps for a person. Offsets count from the
// start of the settings buffer; add the read base to look one up in a dump, the
// write base to aim a frame at it. Same buffer either way.
//
// The geometry belongs to the exact variant, not to the model name. Variants of
// the 7155T disagree: some use 14-byte blocks at 16 and 30, the one here uses
// 10-byte blocks at 24 and 34. Under the wrong geometry not a single stored
// checksum reproduces, which is the cheapest way to notice.
struct OmronSettingsBlock {
  uint8_t offset;
  uint8_t size;
};

// Blocks are per user for everything except the clock. False for a profile
// whose user_block_size is zero, meaning its layout is unknown.
[[nodiscard]] bool user_settings_block(const OmronProfile &profile, uint8_t user_number, OmronSettingsBlock &block);

// Whether this model's user block carries the registered flag and the version
// counter, or only a birth date. Block size decides it: six-byte blocks hold
// the date alone, while the 10, 14 and 26 byte layouts all put the date at 0-2,
// the flag at 3 and the counter at 4-7.
//
// It separates what may be read from what may be registered. A registration is
// a bump of the version counter, so a model without one cannot register in the
// sense this component means - but its birth date is still there to be read.
bool user_block_carries_version(const OmronProfile &profile);

// A single per-user bit the cuff keeps. It marks nothing useful, reading zero
// even for a registered user, and is decoded only because it is free. The
// version counter is what actually records a registration. False on a model
// whose block has neither.
[[nodiscard]] bool user_registered_flag(const OmronProfile &profile, uint8_t user_number,
                                        std::span<const uint8_t> settings, bool &value);

// The birth date read back out of a user block: the one field whose value this
// component chooses, so it returns as proof a write landed rather than as an
// echo. Only the date part is filled. An erased block reads 1900-01-01, which
// is a real answer - nobody has registered in this slot.
[[nodiscard]] bool user_birth_date(const OmronProfile &profile, uint8_t user_number, std::span<const uint8_t> settings,
                                   OmronDateTime &value);

enum class [[nodiscard]] SettingsWriteError : uint8_t {
  NONE = 0,
  NULL_ARGUMENT,
  UNSUPPORTED_MODEL,
  BUFFER_LENGTH,
  // The block did not add up as read, which means the layout is wrong: writing
  // under a wrong layout puts a byte somewhere it was never meant to go.
  CHECKSUM_MISMATCH,
  // Zeros satisfy every checksum rule there is, so an empty block is a read
  // that did not happen rather than a block worth writing over.
  EMPTY_BLOCK,
  BUILD_FAILED,
};

struct SettingsWriteFrame {
  uint16_t address{0};
  std::vector<uint8_t> frame{};
};

// What a session changes in the settings buffer. An absent date leaves those
// bytes as they were read.
struct SessionSettingsUpdate {
  uint8_t user_number{0};
  // Bit 0 is user 1. Every user whose records this session read and published,
  // so their "not yet collected" counter can be cleared. Both areas, not just
  // the registered one: leaving a counter standing tells the cuff something
  // untrue about records it has already handed over.
  uint8_t collected_users{0};
  // The user block with its counter stepped. Only a session that registers
  // writes it.
  bool register_block{false};
  // By value, and it has to stay that way: the session copies this struct to
  // rebuild the frame at send time, and anything borrowed rather than copied has
  // to be re-pointed by hand at every rebuild, which drops the next field added
  // here without a word.
  std::optional<OmronDateTime> birth_date{};
  std::optional<OmronDateTime> clock{};

  // Birth dates for users this session is NOT registering, indexed from zero.
  // Off unless yaml asks, because a user block written outside a registering
  // session is a shape no other host sends. It exists because a cuff holds two
  // people and a node registers as one, leaving the other's date no way in
  // short of a second pairing. The version counter is left alone, since this
  // write claims no registration.
  std::array<std::optional<OmronDateTime>, OMRON_MAX_USERS> standalone_birth_dates{};
};

// Every settings write a session makes, as contiguous runs, in buffer order.
//
// Runs, not fields. Registering user 1 writes 34 bytes covering the pointer
// region and that user's block together because they touch; registering user 2
// writes the pointer region alone and then 26 bytes of block plus clock,
// because user 1's block lies between. Changing the buffer and sending each
// changed stretch produces both without either being special-cased.
SettingsWriteError build_session_settings_writes(const OmronProfile &profile, const SessionSettingsUpdate &update,
                                                 std::span<const uint8_t> settings,
                                                 std::vector<SettingsWriteFrame> &frames);

// The counter the cuff compares before it will accept a user block. Stepping it
// is what a registration is; zero for a model whose block has no room for one.
uint32_t user_settings_version(const OmronProfile &profile, uint8_t user_number, std::span<const uint8_t> settings);

const char *settings_write_error_to_string(SettingsWriteError error);

}  // namespace esphome::omron
