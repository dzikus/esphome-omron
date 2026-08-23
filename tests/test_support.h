#pragma once

// Shared fixtures, and the roll call of groups that live outside
// test_omron_protocol.cpp.
//
// Header-defined and inline on purpose: three short functions used by two
// translation units, where a separate .cpp would be more build plumbing than
// code.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "omron_protocol.h"

namespace esphome::omron {

inline bool nearly_equal(float lhs, float rhs, float epsilon = 0.0001f) {
  return std::fabs(lhs - rhs) <= epsilon;
}

inline void expect_string(const char *actual, const char *expected) {
  assert(actual != nullptr);
  assert(std::strcmp(actual, expected) == 0);
}

// A reply frame as the cuff would send it: header, payload, pad, checksum.
inline std::vector<uint8_t> make_response(PacketType type, uint16_t address = 0,
                                          const std::vector<uint8_t> &payload = {}, uint8_t status = 0) {
  assert(payload.size() <= 0xF7);
  std::vector<uint8_t> frame(payload.size() + 8, 0);
  const uint16_t raw_type = static_cast<uint16_t>(type);
  frame[0] = static_cast<uint8_t>(frame.size());
  frame[1] = static_cast<uint8_t>(raw_type >> 8);
  frame[2] = static_cast<uint8_t>(raw_type & 0xFF);
  frame[3] = static_cast<uint8_t>(address >> 8);
  frame[4] = static_cast<uint8_t>(address & 0xFF);
  frame[5] = static_cast<uint8_t>(payload.size());
  if (type == PacketType::END_RESPONSE) {
    assert(payload.empty());
    frame[6] = status;
  } else {
    std::copy(payload.begin(), payload.end(), frame.begin() + 6);
  }
  frame.back() = xor_bytes(std::span<const uint8_t>(frame).first(frame.size() - 1));
  return frame;
}

}  // namespace esphome::omron

// Defined in test_session.cpp. Everything that drives a whole session against a
// cuff that answers, plus the layer that decides which reading wins.
void test_session_replays_the_captured_frame_order();
void test_session_ignores_a_stray_frame_without_resending();
void test_session_with_unmoved_cursors_reads_only_two_frames();
void test_session_full_read_on_pairing_needs_both_the_option_and_the_flag();
void test_session_registration_writes_reach_the_wire();
void test_session_survives_the_reply_racing_the_write_ack();
void test_session_wire_state_guards();
void test_session_fails_when_the_link_refuses_the_write();
void test_session_pairing_programs_the_key_before_it_reads();
void test_session_classic_handshake_retries_on_its_own_clock();
void test_session_writes_a_birth_date_without_registering();
void test_harvest_picks_the_reading_the_entities_show();
void test_harvest_prefers_the_cursor_over_the_clock();
void test_harvest_cutoff_watermark_and_budget();

// Defined in test_bond_cleanup.cpp. Dropping this node's own bond record, where
// a machine that stops without clearing itself takes the client down with it.
void test_bond_cleanup_never_blocks_after_it_gives_up();
void test_bond_cleanup_waits_for_the_record_to_actually_go();
void test_bond_cleanup_clock_starts_at_the_first_tick();
void test_bond_cleanup_survives_a_list_that_will_not_answer();
void test_datetime_formatting();

// Defined in test_model_id.cpp. Which cuff is on the other end, from the string
// it reports about itself - and, more to the point, when that string is not
// enough to say.
void test_model_id_does_not_cry_wolf_on_the_verified_cuff();
void test_model_id_catches_the_wrong_half_of_a_shared_trade_name();
void test_model_id_refuses_to_guess();
void test_model_id_prefers_a_model_id_to_a_trade_name();
void test_model_id_tolerates_what_comes_off_the_wire();
void test_model_id_table_is_wired_to_the_catalog();
void test_model_id_map_comparison_looks_at_every_field();
void test_model_id_config_key_is_derived_not_listed();
void test_model_id_strings();

// Defined in test_subscriptions.cpp. Turning on the notify channels, where a
// descriptor that never answers stalls the session with nothing to report.
void test_subscriptions_retry_the_cccd_that_never_arrives();
void test_subscriptions_give_up_on_an_optional_target_and_carry_on();
void test_subscriptions_park_until_the_link_is_encrypted();
void test_subscriptions_ignore_events_meant_for_something_else();
void test_subscriptions_edge_cases();

// Defined in test_history_queue.cpp. Older readings on their way out, and the
// flash mark saying how far they got.
void test_history_queue_saves_the_watermark_only_once_it_is_earned();
void test_history_queue_paces_and_bounds_itself();
void test_history_queue_watermark_only_moves_forward();
void test_history_queue_edge_cases();

// Defined in test_publish.cpp. What goes into an entity, where a wrong field
// publishes a plausible number rather than failing.
void test_publish_capabilities_follow_the_catalog();
void test_publish_record_marks_only_what_a_record_carries();
void test_publish_standard_notification_must_name_its_owner();
void test_publish_standard_notification_ranges_and_status();
void test_publish_poll_outcome_names_the_right_cause();
void test_publish_settings_entities();
void test_publish_settings_where_the_block_keeps_no_counter();

// Defined in test_command_writer.cpp. Splitting one command across four TX
// channels - a transport the classic profiles declare and no cuff here has.
void test_command_writer_single_channel_sends_everything_at_once();
void test_command_writer_splits_across_four_channels();
void test_command_writer_refuses_a_command_with_no_channel_left();
void test_command_writer_edge_cases();
