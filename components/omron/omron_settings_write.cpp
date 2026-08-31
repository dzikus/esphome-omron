#include "omron_settings_write.h"

#include <algorithm>

#include "omron_protocol.h"

namespace esphome::omron {

ClockWriteError build_clock_write_request(const OmronProfile &profile, std::span<const uint8_t> window,
                                          const OmronDateTime &target, std::vector<uint8_t> &request,
                                          uint16_t &address) {
  request.clear();
  if (window.empty())
    return ClockWriteError::NULL_ARGUMENT;
  const size_t window_length = window.size();
  if (profile.clock_fields_offset == NO_CLOCK)
    return ClockWriteError::UNSUPPORTED_LAYOUT;
  // A profile with no write address has not had this verified on hardware, and
  // guessing one would aim a write at whatever happens to live there.
  if (profile.settings_write_address == 0 || profile.time_region_end <= profile.time_region_start)
    return ClockWriteError::WRITE_UNSUPPORTED;

  const size_t fields_at = profile.clock_fields_offset;
  const size_t region_size = static_cast<size_t>(profile.time_region_end - profile.time_region_start);
  if (window_length != region_size || window_length < 2 || window_length > 0xFF)
    return ClockWriteError::WINDOW_LENGTH;
  const size_t checksum_at = window_length - 2;
  if (fields_at + 6 > checksum_at)
    return ClockWriteError::WINDOW_LENGTH;
  if (!is_valid_datetime(target) || target.year < 2000 || target.year > 2255)
    return ClockWriteError::INVALID_TIME;

  // Everything outside the six clock fields is written back exactly as read:
  // the window holds other settings, and this command is not entitled to them.
  std::vector<uint8_t> updated(window.begin(), window.end());
  updated[fields_at] = static_cast<uint8_t>(target.year - 2000);
  updated[fields_at + 1] = target.month;
  updated[fields_at + 2] = target.day;
  updated[fields_at + 3] = target.hour;
  updated[fields_at + 4] = target.minute;
  updated[fields_at + 5] = target.second;

  uint32_t sum = 0;
  for (size_t i = 0; i < checksum_at; i++)
    sum += updated[i];
  updated[checksum_at] = static_cast<uint8_t>(sum & 0xFF);

  const uint16_t target_address = static_cast<uint16_t>(profile.settings_write_address + profile.time_region_start);
  request = make_write_request(target_address, updated);
  if (request.empty())
    return ClockWriteError::BUILD_FAILED;

  address = target_address;
  return ClockWriteError::NONE;
}

const char *clock_write_error_to_string(ClockWriteError error) {
  switch (error) {
    case ClockWriteError::NONE:
      return "none";
    case ClockWriteError::NULL_ARGUMENT:
      return "null argument";
    case ClockWriteError::UNSUPPORTED_LAYOUT:
      return "unsupported clock layout";
    case ClockWriteError::WRITE_UNSUPPORTED:
      return "profile has no verified settings write address";
    case ClockWriteError::WINDOW_LENGTH:
      return "clock window length mismatch";
    case ClockWriteError::INVALID_TIME:
      return "refusing to write an invalid time";
    case ClockWriteError::BUILD_FAILED:
      return "write frame could not be built";
  }
  return "unknown clock write error";
}

namespace {

// Offsets INSIDE a user block. These are the same on every model, whatever the
// block size.
//
// Where a block STARTS and how big it is are not: eight layouts exist, so
// user_settings_block derives both from the profile. Do not reintroduce them as
// constants here.
//
// The registered flag: bit 0, three bytes into the block.
constexpr uint8_t REGISTERED_FLAG_IN_BLOCK = 3;
// Birth date: year less 1900, month, day, at the very start of the block.
constexpr uint8_t BIRTH_DATE_IN_BLOCK = 0;
constexpr uint8_t BIRTH_DATE_SIZE = 3;
// The version counter the cuff compares before it accepts a block. Four bytes,
// little-endian.
constexpr uint8_t VERSION_IN_BLOCK = 4;
constexpr uint8_t VERSION_SIZE = 4;
constexpr uint8_t REGISTERED_FLAG_BIT = 0x01;
// Registering somebody rewrites the region ahead of the blocks, changing only
// the two bytes of that user's unsent counter and leaving them reading 00 80.
// Nothing else in the region moves, which also rules out a checksum over it.
//
// The offset of those two bytes is per ring and comes from the catalog. Most
// profiles put user 1 at 4 and user 2 at 6; the 6401 family states 14, so this
// must not be computed from a constant however well one happens to fit.
constexpr uint8_t USER_POINTER_SIZE = 2;
// 0x8000 little-endian: counter zero, flag kept. Every blood pressure ring in
// the catalog clears to this value.
constexpr uint8_t USER_POINTER_REGISTERED[USER_POINTER_SIZE] = {0x00, 0x80};

// The unsent counter of one user, as a byte offset into the settings region.
// Users are numbered from 1.
size_t user_pointer_field(const OmronProfile &profile, uint8_t user_number) {
  return profile.users[user_number - 1].unread_counter_offset;
}
// Which user a settings run is for, one bit each, at offset 4 of the clock
// block. Write-only: reads come back with the byte clear, so it never shows up
// in a dump.
//
// Bits, not a user number. The two happen to agree for users 1 and 2, which is
// every model here; from user 3 up a number would be 0b11 and address the first
// two users instead of the third.
constexpr uint8_t USER_MARKER_IN_CLOCK = 4;

// The birth date at the head of a user block: year less 1900, month, day. Only
// those three fields of the argument are read. Declined rather than clamped,
// because a date nobody can vouch for must not ride along with a registration
// the bond depends on.
bool apply_birth_date(uint8_t *block, const OmronDateTime &date) {
  if (date.year < 1900 || date.year > 2155 || date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31)
    return false;
  block[BIRTH_DATE_IN_BLOCK] = static_cast<uint8_t>(date.year - 1900);
  block[BIRTH_DATE_IN_BLOCK + 1] = date.month;
  block[BIRTH_DATE_IN_BLOCK + 2] = date.day;
  return true;
}

// Exactly one bit: a run belongs to one user, so callers replace the marker
// byte rather than adding to it. Zero for a user number that cannot be
// expressed, which leaves the marker clear rather than pointing at somebody
// else.
uint8_t user_marker_bit(uint8_t user_number) {
  if (user_number < 1 || user_number > 8)
    return 0;
  return static_cast<uint8_t>(1u << (user_number - 1));
}

// The window has to hold the six fields and still leave the checksum two bytes
// from its end; shorter and the two overlap, putting the clock in whatever
// settings follow it. Per profile, because where the fields start is.
constexpr size_t clock_window_minimum(uint8_t fields_offset) {
  return static_cast<size_t>(fields_offset) + 6 + 2;
}

// Sets the time in a window the caller is already rewriting, leaving the marker
// and the settings around it alone. The caller recomputes the window checksum
// afterwards; every builder that reaches this does that anyway.
//
// A time the caller cannot vouch for is declined rather than written: these
// windows go out as part of the registration run the bond depends on, and a bad
// clock must cost the wrong time, not the registration.
bool apply_clock_fields(uint8_t *window, uint8_t fields_offset, const OmronDateTime &target) {
  if (fields_offset == NO_CLOCK || !is_valid_datetime(target) || target.year < 2000 || target.year > 2255)
    return false;
  window[fields_offset] = static_cast<uint8_t>(target.year - 2000);
  window[fields_offset + 1] = target.month;
  window[fields_offset + 2] = target.day;
  window[fields_offset + 3] = target.hour;
  window[fields_offset + 4] = target.minute;
  window[fields_offset + 5] = target.second;
  return true;
}

// The clock block puts its checksum two bytes from the end and sums everything
// before it. Nothing says the shorter blocks differ, and the checksum test below
// refuses the write if they do.
size_t block_checksum_offset(const OmronSettingsBlock &block) {
  return block.offset + block.size - 2;
}

uint8_t block_checksum(const uint8_t *settings, const OmronSettingsBlock &block) {
  uint32_t sum = 0;
  for (size_t i = block.offset; i < block_checksum_offset(block); i++)
    sum += settings[i];
  return static_cast<uint8_t>(sum & 0xFF);
}

// Raises the version the cuff compares before it will consider the block, then
// makes the block add up. The counter is little-endian: a stored 1 steps to 2
// and an erased 0 to 1, both in the first byte.
void finalize_target_block(const OmronProfile &profile, uint8_t target_user, std::vector<uint8_t> &buffer) {
  OmronSettingsBlock block{};
  if (!user_settings_block(profile, target_user, block))
    return;
  const size_t counter = block.offset + VERSION_IN_BLOCK;
  for (size_t i = counter; i < counter + VERSION_SIZE; i++) {
    if (++buffer[i] != 0)
      break;
  }
  buffer[block_checksum_offset(block)] = block_checksum(buffer.data(), block);
}

}  // namespace

bool user_block_carries_version(const OmronProfile &profile) {
  return profile.user_block_size >= VERSION_IN_BLOCK + VERSION_SIZE;
}

bool user_settings_block(const OmronProfile &profile, uint8_t user_number, OmronSettingsBlock &block) {
  // Derived from the profile, never hardcoded: eight block layouts exist.
  if (profile.settings_write_address == 0 || profile.user_block_size == 0)
    return false;
  if (user_number < 1 || user_number > 2 || user_number > profile.user_count)
    return false;
  const uint32_t offset = static_cast<uint32_t>(profile.settings_index_region_size) +
                          static_cast<uint32_t>(user_number - 1) * profile.user_block_size;
  // The blocks live between the pointer region and the clock. Refuse a profile
  // whose numbers put them anywhere else: reaching past the end of the region
  // is how a settings buffer gets written over.
  if (offset + profile.user_block_size > profile.time_region_start || profile.time_region_start == 0)
    return false;
  // Three bytes is the smallest block holding anything this component reads:
  // the birth date, which every layout puts at the head.
  if (profile.user_block_size < BIRTH_DATE_IN_BLOCK + BIRTH_DATE_SIZE)
    return false;
  block.offset = static_cast<uint8_t>(offset);
  block.size = profile.user_block_size;
  return true;
}

bool user_registered_flag(const OmronProfile &profile, uint8_t user_number, std::span<const uint8_t> settings,
                          bool &value) {
  OmronSettingsBlock block{};
  if (!user_settings_block(profile, user_number, block) || !user_block_carries_version(profile))
    return false;
  const size_t flag_offset = block.offset + REGISTERED_FLAG_IN_BLOCK;
  if (settings.size() <= flag_offset)
    return false;
  value = (settings[flag_offset] & REGISTERED_FLAG_BIT) != 0;
  return true;
}

bool user_birth_date(const OmronProfile &profile, uint8_t user_number, std::span<const uint8_t> settings,
                     OmronDateTime &value) {
  OmronSettingsBlock block{};
  if (!user_settings_block(profile, user_number, block))
    return false;
  if (settings.size() < static_cast<size_t>(block.offset) + BIRTH_DATE_IN_BLOCK + BIRTH_DATE_SIZE)
    return false;

  const std::span<const uint8_t> date = settings.subspan(block.offset + BIRTH_DATE_IN_BLOCK);
  // The inverse of apply_birth_date, and deliberately written as one: a reader
  // that disagreed with the writer would report a landed write as a failed one.
  OmronDateTime parsed{};
  parsed.year = static_cast<uint16_t>(1900 + date[0]);
  parsed.month = date[1];
  parsed.day = date[2];
  if (!is_valid_datetime(parsed))
    return false;
  value = parsed;
  return true;
}

uint32_t user_settings_version(const OmronProfile &profile, uint8_t user_number, std::span<const uint8_t> settings) {
  OmronSettingsBlock block{};
  if (!user_settings_block(profile, user_number, block) || !user_block_carries_version(profile))
    return 0;
  if (settings.size() < static_cast<size_t>(block.offset) + VERSION_IN_BLOCK + VERSION_SIZE)
    return 0;
  // Little-endian. Read big-endian, a counter of 1 logs as 16777216.
  const std::span<const uint8_t> counter = settings.subspan(block.offset + VERSION_IN_BLOCK);
  return static_cast<uint32_t>(counter[0]) | (static_cast<uint32_t>(counter[1]) << 8) |
         (static_cast<uint32_t>(counter[2]) << 16) | (static_cast<uint32_t>(counter[3]) << 24);
}

SettingsWriteError build_session_settings_writes(const OmronProfile &profile, const SessionSettingsUpdate &update,
                                                 std::span<const uint8_t> settings,
                                                 std::vector<SettingsWriteFrame> &frames) {
  frames.clear();
  if (settings.empty())
    return SettingsWriteError::NULL_ARGUMENT;
  const size_t settings_length = settings.size();

  // A registration is a bump of the version counter plus the block checksum over
  // it, so a model whose block has no counter cannot be registered this way at
  // all - its block holds a birth date and nothing else. Refused here rather
  // than in the block locator, because the date is still worth reading on those
  // models even though nothing may be written back.
  OmronSettingsBlock first{};
  OmronSettingsBlock block{};
  // Only registration needs the counter. A standalone birth date does not claim
  // to have registered anything, so the families whose block lists a date and
  // nothing else - 31 variants with a six-byte block - must not be refused here
  // on the strength of a counter they were never going to use.
  if (update.register_block && !user_block_carries_version(profile))
    return SettingsWriteError::UNSUPPORTED_MODEL;
  if (!user_settings_block(profile, 1, first) || !user_settings_block(profile, update.user_number, block) ||
      update.user_number > profile.user_count || profile.settings_write_address == 0 ||
      profile.time_region_end <= profile.time_region_start)
    return SettingsWriteError::UNSUPPORTED_MODEL;
  if (profile.clock_fields_offset == NO_CLOCK ||
      static_cast<size_t>(profile.time_region_end - profile.time_region_start) <
          clock_window_minimum(profile.clock_fields_offset))
    return SettingsWriteError::UNSUPPORTED_MODEL;

  // Safe to index: the guard above refuses any user number the profile does not
  // have, and user_settings_block refuses 0 and anything past OMRON_MAX_USERS.
  const size_t pointer_field = user_pointer_field(profile, update.user_number);
  if (settings_length < profile.time_region_end || pointer_field + USER_POINTER_SIZE > first.offset)
    return SettingsWriteError::BUFFER_LENGTH;

  // Zeros satisfy every checksum rule there is, so a block that reads empty is
  // a read that did not happen rather than a block worth stepping. Ungated, a
  // dump taken at the wrong base writes a run of zeros over a real user block.
  if (update.register_block) {
    if (std::ranges::all_of(settings.subspan(block.offset, block.size), [](uint8_t v) { return v == 0; }))
      return SettingsWriteError::EMPTY_BLOCK;
    // And the block has to add up as it was read. Not-all-zero does not cover
    // the failure that actually happens: a block read under the wrong base
    // address is full of real bytes that are not this block. It
    // costs most here, because this is the frame that steps the version counter
    // and the cuff takes it because the counter moved, not because the bytes
    // made sense.
    if (settings[block_checksum_offset(block)] != block_checksum(settings.data(), block))
      return SettingsWriteError::CHECKSUM_MISMATCH;
  }

  std::vector<uint8_t> buffer(settings.begin(), settings.end());

  // This user's two-byte field, plus the field of every other user whose
  // records this session collected. Nothing in the pointer region carries a
  // checksum, so the bytes can be changed one at a time.
  for (size_t i = 0; i < USER_POINTER_SIZE; i++)
    buffer[pointer_field + i] = USER_POINTER_REGISTERED[i];
  for (uint8_t user = 1; user <= profile.user_count && user <= OMRON_MAX_USERS; user++) {
    if ((update.collected_users & (1U << (user - 1))) == 0)
      continue;
    const size_t field = user_pointer_field(profile, user);
    if (field + USER_POINTER_SIZE > first.offset)
      continue;
    for (size_t i = 0; i < USER_POINTER_SIZE; i++)
      buffer[field + i] = USER_POINTER_REGISTERED[i];
  }

  // The block belongs to the session that registers. Its counter is what makes
  // the cuff take it - a block offering the version already stored is one the
  // cuff has no reason to accept - and the date has to go in ahead of the
  // checksum, so the block adds up over what actually leaves.
  if (update.register_block) {
    if (update.birth_date.has_value())
      apply_birth_date(buffer.data() + block.offset, *update.birth_date);
    finalize_target_block(profile, update.user_number, buffer);
  }

  // Birth dates for the users this session is not registering. Each one has to
  // clear its own guards: the block must not read empty, its checksum must
  // reproduce as read, and the date must actually differ from what is stored.
  // That last one is what makes this a one-shot rather than an EEPROM write in
  // every session for the rest of the device's life.
  std::array<bool, OMRON_MAX_USERS> standalone_written{};
  for (uint8_t user = 1; user <= profile.user_count && user <= OMRON_MAX_USERS; user++) {
    const std::optional<OmronDateTime> &wanted = update.standalone_birth_dates[user - 1];
    if (!wanted.has_value() || (update.register_block && user == update.user_number))
      continue;
    OmronSettingsBlock target{};
    if (!user_settings_block(profile, user, target) ||
        static_cast<size_t>(target.offset) + target.size > settings_length)
      continue;
    if (std::ranges::all_of(settings.subspan(target.offset, target.size), [](uint8_t v) { return v == 0; }) ||
        settings[block_checksum_offset(target)] != block_checksum(settings.data(), target))
      continue;
    OmronDateTime stored{};
    if (user_birth_date(profile, user, settings, stored) && stored.year == wanted->year &&
        stored.month == wanted->month && stored.day == wanted->day)
      continue;
    apply_birth_date(buffer.data() + target.offset, *wanted);
    buffer[block_checksum_offset(target)] = block_checksum(buffer.data(), target);
    standalone_written[user - 1] = true;
  }

  // The clock goes out marked in every session, carrying the wall clock of the
  // moment when there is one to carry.
  const OmronSettingsBlock window{profile.time_region_start,
                                  static_cast<uint8_t>(profile.time_region_end - profile.time_region_start)};
  buffer[window.offset + USER_MARKER_IN_CLOCK] = user_marker_bit(update.user_number);
  if (update.clock.has_value())
    apply_clock_fields(buffer.data() + window.offset, profile.clock_fields_offset, *update.clock);
  buffer[block_checksum_offset(window)] = block_checksum(buffer.data(), window);

  struct Region {
    uint8_t start;
    uint8_t end;
  };
  // Ascending by construction: the pointer region, then each user block that
  // changed in user order, then the clock. The merge below relies on that.
  std::array<Region, 2 + OMRON_MAX_USERS> regions{};
  size_t count = 0;
  regions[count++] = Region{0, first.offset};
  for (uint8_t user = 1; user <= profile.user_count && user <= OMRON_MAX_USERS; user++) {
    OmronSettingsBlock candidate{};
    const bool registering = update.register_block && user == update.user_number;
    if ((!registering && !standalone_written[user - 1]) || !user_settings_block(profile, user, candidate))
      continue;
    regions[count++] = Region{candidate.offset, static_cast<uint8_t>(candidate.offset + candidate.size)};
  }
  regions[count++] = Region{window.offset, static_cast<uint8_t>(window.offset + window.size)};

  // Touching regions become one run, which is why this works in regions rather
  // than a frame per field. Both registration shapes fall out of it: user 1's
  // block touches the pointer region, so those two merge and the clock goes
  // alone; user 2's does not, because user 1's block lies between, but its own
  // block touches the clock instead.
  //
  // Capped at the transfer block size. That changes nothing for those two - 24
  // and 26 bytes - and matters when a standalone date makes all four regions
  // touch: unbounded, that merge asks for a 60-byte payload in a 68-byte frame,
  // past both the cuff's own maximum and the negotiated MTU.
  for (size_t i = 0; i < count;) {
    size_t last = i;
    while (last + 1 < count && regions[last].end == regions[last + 1].start &&
           static_cast<size_t>(regions[last + 1].end) - regions[i].start <= profile.transmission_block_size)
      last++;
    const uint8_t start = regions[i].start;
    const size_t length = static_cast<size_t>(regions[last].end) - start;
    const uint16_t target = static_cast<uint16_t>(profile.settings_write_address + start);
    const std::vector<uint8_t> request =
        make_write_request(target, std::span<const uint8_t>(buffer).subspan(start, length));
    if (request.empty())
      return SettingsWriteError::BUILD_FAILED;
    frames.push_back(SettingsWriteFrame{target, request});
    i = last + 1;
  }
  return SettingsWriteError::NONE;
}

const char *settings_write_error_to_string(SettingsWriteError error) {
  switch (error) {
    case SettingsWriteError::NONE:
      return "none";
    case SettingsWriteError::NULL_ARGUMENT:
      return "null argument";
    case SettingsWriteError::UNSUPPORTED_MODEL:
      return "no settings block layout for this model or user";
    case SettingsWriteError::BUFFER_LENGTH:
      return "settings dump too short for the block";
    case SettingsWriteError::CHECKSUM_MISMATCH:
      return "block checksum does not reproduce, so the layout is wrong";
    case SettingsWriteError::EMPTY_BLOCK:
      return "block read back all zeros, so it was never really read";
    case SettingsWriteError::BUILD_FAILED:
      return "write frame could not be built";
  }
  return "unknown settings write error";
}

}  // namespace esphome::omron
