// Splitting one command across four TX channels.
//
// The four-channel transport the classic half of the catalog declares is
// unproven on hardware, so these tests are the only thing that executes it.

#include <cassert>
#include <cstdint>
#include <vector>

#include "omron_command_writer.h"
#include "test_support.h"

using namespace esphome::omron;

void test_command_writer_single_channel_sends_everything_at_once() {
  const std::vector<uint8_t> command(20, 0xAB);
  OmronCommandWriter writer;
  writer.begin(command, 1, false);

  WriteFragment fragment{};
  assert(writer.next(fragment));
  assert(fragment.channel == 0 && fragment.offset == 0);
  // The whole command, not a stride: a modern profile has one characteristic and
  // the twenty-byte token has to arrive as one write.
  assert(fragment.length == 20 && fragment.last);
  writer.advance(fragment);
  assert(writer.complete());
  assert(!writer.next(fragment));

  // The unlock characteristic is its own channel whatever the profile declares,
  // so a classic profile's key command is not split either.
  OmronCommandWriter unlock;
  unlock.begin(std::span<const uint8_t>(command).first(17), 4, true);
  assert(unlock.next(fragment) && fragment.length == 17 && fragment.channel == 0);
}

void test_command_writer_splits_across_four_channels() {
  // Forty bytes over four channels: 16, 16, 8. Each byte carries its own index
  // so a fragment landing on the wrong channel or at the wrong offset shows up
  // as a value rather than as a length.
  std::vector<uint8_t> command(40);
  for (size_t i = 0; i < command.size(); i++)
    command[i] = static_cast<uint8_t>(i);

  OmronCommandWriter writer;
  writer.begin(command, 4, false);

  WriteFragment fragment{};
  assert(writer.next(fragment));
  assert(fragment.channel == 0 && fragment.offset == 0 && fragment.length == 16 && !fragment.last);
  assert(writer.data()[fragment.offset] == 0);
  writer.advance(fragment);

  assert(writer.next(fragment));
  assert(fragment.channel == 1 && fragment.offset == 16 && fragment.length == 16 && !fragment.last);
  assert(writer.data()[fragment.offset] == 16);
  writer.advance(fragment);

  assert(writer.next(fragment));
  // The tail is what is left, not a padded stride.
  assert(fragment.channel == 2 && fragment.offset == 32 && fragment.length == 8 && fragment.last);
  assert(writer.data()[fragment.offset] == 32);
  writer.advance(fragment);

  assert(writer.complete());
  assert(!writer.next(fragment));
}

void test_command_writer_refuses_a_command_with_no_channel_left() {
  // Sixty-five bytes over four sixteen-byte channels. The fifth fragment has
  // nowhere to go, and saying so is different from a write that failed: the
  // profile's channel count cannot carry a command this long, which is a
  // configuration fault and not the link's doing.
  const std::vector<uint8_t> command(65, 0x11);
  OmronCommandWriter writer;
  writer.begin(command, 4, false);

  WriteFragment fragment{};
  for (int i = 0; i < 4; i++) {
    assert(writer.next(fragment));
    assert(fragment.channel == static_cast<uint8_t>(i) && fragment.length == 16);
    writer.advance(fragment);
  }
  assert(!writer.complete());
  assert(!writer.next(fragment));

  // Exactly sixty-four fits, with the last fragment closing the command.
  OmronCommandWriter exact;
  exact.begin(std::span<const uint8_t>(command).first(64), 4, false);
  for (int i = 0; i < 4; i++) {
    assert(exact.next(fragment));
    exact.advance(fragment);
  }
  assert(fragment.channel == 3 && fragment.last);
  assert(exact.complete());
}

void test_command_writer_edge_cases() {
  WriteFragment fragment{};
  OmronCommandWriter idle;
  assert(!idle.active() && !idle.next(fragment));

  const std::vector<uint8_t> command(8, 0x22);
  OmronCommandWriter writer;
  // A profile declaring no channels is treated as one rather than dividing by
  // zero on the way to finding out.
  writer.begin(command, 0, false);
  assert(writer.next(fragment) && fragment.length == 8);

  // Cleared mid-command, which is what a session reset does: nothing is left
  // outstanding and the next begin() starts from zero.
  writer.clear();
  assert(!writer.active() && !writer.next(fragment));

  // Resuming lands on the channel the offset implies, not on a counter that a
  // reset would have lost.
  std::vector<uint8_t> long_command(48, 0x33);
  writer.begin(long_command, 4, false);
  assert(writer.next(fragment));
  writer.advance(fragment);
  assert(writer.next(fragment) && fragment.channel == 1);
  writer.advance(fragment);
  assert(writer.next(fragment) && fragment.channel == 2 && fragment.last);
}
