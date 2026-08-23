#include "omron_advertisement.h"

namespace esphome::omron {

namespace {

// The flag byte is the same in every format that carries one; only its position
// and the length rules differ.
void apply_flag_byte(uint8_t flag_byte, OmronAdvertisementFlags &flags) {
  flags.user_register_count = flag_byte & 0x03;
  flags.invalid_time = (flag_byte & 0x04) != 0;
  flags.pairing_mode = (flag_byte & 0x08) != 0;
  flags.forced_transfer = (flag_byte & 0x40) != 0;
}

// Offsets here count from a payload with the company identifier already
// stripped, which is two bytes short of how these are usually written down.
// Every format puts the sequence numbers somewhere different, and reading one
// at another's offset returns neighbouring bytes rather than failing, so they
// are passed in per format rather than guessed. The block count is one more
// than the field in the flag byte, which is what the frame lengths imply.
// Short frames stop the walk instead of failing it: a truncated tail costs the
// last sequence number, not the flags that decide whether to connect at all.
void read_user_sequences(std::span<const uint8_t> payload, size_t first_offset, size_t stride,
                         OmronAdvertisementFlags &flags) {
  const uint8_t blocks = static_cast<uint8_t>((payload[1] & 0x03) + 1);
  for (uint8_t index = 0; index < blocks && index < OmronAdvertisementFlags::MAX_USER_SEQUENCES; index++) {
    const size_t offset = first_offset + static_cast<size_t>(index) * stride;
    if (offset + 1 >= payload.size())
      return;
    flags.user_sequence[index] = static_cast<uint16_t>(payload[offset] | (payload[offset + 1] << 8));
    flags.sequence_count = static_cast<uint8_t>(index + 1);
  }
}

}  // namespace

bool parse_advertisement_flags(std::span<const uint8_t> payload, OmronAdvertisementFlags &flags) {
  const size_t length = payload.size();
  if (length < 2)
    return false;

  flags = {};
  flags.format = payload[0];
  const uint8_t flag_byte = payload[1];

  switch (payload[0]) {
    case 0x03: {
      // Older cuffs. Bits 4 and 5 carry a guidance field here rather than the
      // mode bits the newer formats put there, and bit 6 is not a transfer
      // request at all, so the flag byte is read field by field instead of
      // through the shared decoder.
      if (length < 3)
        return false;
      flags.user_register_count = flag_byte & 0x03;
      flags.invalid_time = (flag_byte & 0x04) != 0;
      flags.pairing_mode = (flag_byte & 0x08) != 0;
      return true;
    }

    case 0x01:
    case 0x02:
    case 0x06: {
      // Three bytes of sequence numbers per registered user follow the header.
      const size_t minimum = 4 + static_cast<size_t>(flag_byte & 0x03) * 3;
      if (length < minimum)
        return false;
      apply_flag_byte(flag_byte, flags);
      // Offset 2 counting from here, 4 counting from the start of the
      // advertisement: the company identifier is already behind us. Stride 3.
      // This is the family our own cuff advertises in.
      read_user_sequences(payload, 2, 3, flags);
      return true;
    }

    case 0x08: {
      const uint8_t users = flag_byte & 0x03;
      const bool length_ok = (users == 0 && length == 10) || (users == 1 && length == 13);
      if (!length_ok)
        return false;
      apply_flag_byte(flag_byte, flags);
      // Offset 8 in the advertisement, six once the company identifier is gone.
      read_user_sequences(payload, 6, 2, flags);
      return true;
    }

    case 0x09: {
      // Two bytes per registered user on top of a nine byte base.
      const uint8_t users = flag_byte & 0x03;
      if (length != static_cast<size_t>(9 + users * 2))
        return false;
      apply_flag_byte(flag_byte, flags);
      // One byte further along than format 8, the extra byte being this
      // format's longer base.
      read_user_sequences(payload, 7, 2, flags);
      return true;
    }

    default:
      return false;
  }
}

}  // namespace esphome::omron
