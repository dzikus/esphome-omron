#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "omron_advertisement.h"
#include "omron_connection_gate.h"
#include "omron_diagnostics.h"
#include "omron_harvest.h"
#include "omron_log.h"
#include "omron_measurement.h"
#include "omron_memory.h"
#include "omron_metrics.h"
#include "omron_poll_plan.h"
#include "omron_profile_adapter.h"
#include "omron_profiles.h"
#include "omron_protocol.h"
#include "omron_record_store.h"
#include "omron_scheduler.h"
#include "omron_session.h"
#include "omron_standard_bp.h"
#include "omron_transaction.h"
#include "omron_unlock.h"
#include "test_support.h"

// Host-only suite over the pure layers. The fixture helpers below build frames
// and records; where a helper is the algebraic inverse of the code it exercises
// this is called out at the helper, because those asserts cannot fail on their
// own and need a captured trace from real hardware to become meaningful.

using namespace esphome::omron;

static void write_u16_le(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  assert(offset + 2 <= data.size());
  data[offset] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

static void write_u16_be(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  assert(offset + 2 <= data.size());
  data[offset] = static_cast<uint8_t>(value >> 8);
  data[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

static std::vector<uint8_t> make_classic_record(const OmronProfile &profile) {
  assert(profile.record_size >= 8);
  std::vector<uint8_t> record(profile.record_size, 0);
  record[0] = 95;  // 95 + 25 = 120 mmHg.
  record[1] = 80;
  record[2] = 60;
  record[3] = 24;
  // The four booleans are deliberately not all 1. Give neighbouring flags the
  // same value and a decoder reading bit 14 as bit 15, or cuff as battery,
  // still passes. movement=1/irregular=0 and cuff=1/battery=0 make an adjacent
  // swap fail.
  const uint16_t flags1 = static_cast<uint16_t>(10U | (15U << 5) | (6U << 10) | (0U << 14) | (1U << 15));
  const uint16_t flags2 = static_cast<uint16_t>(30U | (45U << 6) | (1U << 12) | (0U << 13) | (2U << 14));
  record[4] = static_cast<uint8_t>(flags1 & 0xFF);
  record[5] = static_cast<uint8_t>(flags1 >> 8);
  record[6] = static_cast<uint8_t>(flags2 & 0xFF);
  record[7] = static_cast<uint8_t>(flags2 >> 8);
  // Artifact and IHB share byte 9, artifact in the low nibble.
  record[9] = 0x35;
  // The sequence number goes where the profile says it lives, not at the tail.
  // Writing it at the tail was the old fixture, and it agreed with the old
  // decoder for the same wrong reason: both counted from the end. On this
  // hardware the tail holds the record's checksum.
  if (profile.record_sequence_offset != NO_RECORD_SEQUENCE &&
      static_cast<size_t>(profile.record_sequence_offset) + 2 <= record.size()) {
    record[profile.record_sequence_offset] = 0x34;
    record[profile.record_sequence_offset + 1] = 0x12;
  }
  return record;
}

// The HEM-6401T family's own shape: six plain date bytes, then the reading.
// Same values as the classic fixture, so both go through one set of assertions.
static std::vector<uint8_t> make_plain_date_record(const OmronProfile &profile) {
  std::vector<uint8_t> record(profile.record_size, 0);
  record[0] = 24;
  record[1] = 6;
  record[2] = 15;
  record[3] = 10;
  record[4] = 45;
  record[5] = 30;
  record[6] = 95;  // 95 + 25 = 120 mmHg.
  record[7] = 80;
  record[8] = 60;
  // Bits 0-1 are movement, bits 2-3 the irregular beat. One set and one clear,
  // so reading the pair swapped fails here rather than passing by symmetry.
  record[11] = 0x01;
  return record;
}

static std::vector<uint8_t> make_valid_record(const OmronProfile &profile) {
  switch (profile.record_format) {
    case RecordFormat::CLASSIC_VITAL_14:
      return make_classic_record(profile);
    case RecordFormat::CLASSIC_VITAL_24_GUARDED: {
      // Same fields, plus the validity byte the family gates them on. Zero is
      // the value that lets a reading through.
      auto record = make_classic_record(profile);
      record[17] = 0;
      return record;
    }
    case RecordFormat::PLAIN_DATE_VITAL:
      return make_plain_date_record(profile);
    case RecordFormat::UNSUPPORTED:
      return {};
  }
  return {};
}

static void assert_measurement_values(const OmronProfile &profile, const OmronMeasurement &measurement) {
  assert(measurement.systolic_mm_hg == 120);
  assert(measurement.diastolic_mm_hg == 80);
  assert(measurement.pulse_bpm == 60);
  assert(measurement.timestamp.year == 2024);
  assert(measurement.timestamp.month == 6);
  assert(measurement.timestamp.day == 15);
  assert(measurement.timestamp.hour == 10);
  assert(measurement.timestamp.minute == 45);
  assert(measurement.timestamp.second == 30);
  assert(!measurement.irregular_heartbeat);
  assert(measurement.movement_detected);
  if (profile.record_format == RecordFormat::PLAIN_DATE_VITAL) {
    // This family describes no cuff fit and none of the two-bit field the
    // others carry, and no family describes a battery, so these stay clear
    // instead of decoding whatever sits at those offsets.
    assert(!measurement.cuff_flag);
    assert(!measurement.battery_flag);
    assert(measurement.consecutive_measurement == 0);
    return;
  }
  assert(measurement.cuff_flag);
  assert(!measurement.battery_flag);
  // Two bits, and this fixture sets them to a value no adjacent field could
  // produce.
  assert(measurement.consecutive_measurement == 2);
  // Byte 9, artifact in the low nibble and IHB in the high one. The low one is
  // 5 and the high one 3, so reading them the wrong way round fails.
  assert(measurement.artifact_detection == 5);
  assert(measurement.ihb_detection == 3);
}

static void test_protocol_requests_and_parsing() {
  const std::array<uint8_t, 8> expected_read{0x08, 0x01, 0x00, 0x02, 0x60, 0x26, 0x00, 0x4D};
  const std::array<uint8_t, 8> expected_start{0x08, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x18};
  const std::array<uint8_t, 8> expected_end{0x08, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};
  assert(make_read_request(0x0260, 0x26) == expected_read);
  assert(make_start_request() == expected_start);
  assert(make_end_request() == expected_end);
  assert(xor_bytes(expected_read) == 0);

  // Captured from a real HEM-7155T in reply to make_start_request().
  // Byte 5 is the echoed transfer block size, not a payload length; parsing it as
  // a length rejected every start response and the session died on the handshake.
  const std::array<uint8_t, 8> real_start_reply{0x08, 0x80, 0x00, 0x00, 0x00, 0x10, 0x00, 0x98};
  assert(xor_bytes(real_start_reply) == 0);
  ResponseFrame start_parsed;
  assert(parse_response(real_start_reply, start_parsed) == ProtocolError::NONE);
  assert(start_parsed.type == PacketType::START_RESPONSE);
  assert(start_parsed.data.empty());

  const auto read_frame = make_response(PacketType::READ_RESPONSE, 0x0260, {0x12, 0x34, 0x56});
  ResponseFrame read;
  assert(parse_response(read_frame, read) == ProtocolError::NONE);
  assert(read.type == PacketType::READ_RESPONSE);
  assert(read.address == 0x0260);
  assert((read.data == std::vector<uint8_t>{0x12, 0x34, 0x56}));

  // Index and record reads share one start/end envelope, so more ranges get
  // appended once the planned blocks are done. Two appends in a row: the first
  // resumes reading, the second must still be accepted while READ_PENDING.
  OmronTransaction envelope;
  const std::array<uint8_t, 4> no_nonce{};
  assert(envelope.add_read_range(0x0260, 0x10, 0x10));
  assert(envelope.begin(TransactionUnlock::NONE, OmronBindKey{}, no_nonce));
  const auto envelope_start = make_response(PacketType::START_RESPONSE);
  assert(envelope.accept_frame(envelope_start) == ProtocolError::NONE);
  const auto index_reply = make_response(PacketType::READ_RESPONSE, 0x0260, std::vector<uint8_t>(0x10, 0x11));
  assert(envelope.accept_frame(index_reply) == ProtocolError::NONE);
  assert(envelope.state() == TransactionState::END_PENDING);
  assert(envelope.extend_reads(0x02E8, 0x10, 0x10));
  assert(envelope.state() == TransactionState::READ_PENDING);
  assert(envelope.extend_reads(0x06A8, 0x10, 0x10));
  assert(envelope.pending_command().address == 0x02E8);
  assert(!envelope.extend_reads(0x0300, 0, 0x10));

  // A write queued mid-session goes out after the last read and before the end
  // command, so one envelope covers reads and the clock write together.
  const std::vector<uint8_t> clock_frame = make_write_request(0x02D0, std::vector<uint8_t>(0x10, 0x5A));
  assert(!clock_frame.empty());
  assert(envelope.queue_write(0x02D0, clock_frame));
  assert(envelope.write_queued());
  assert(envelope.state() == TransactionState::READ_PENDING);

  const auto user1_reply = make_response(PacketType::READ_RESPONSE, 0x02E8, std::vector<uint8_t>(0x10, 0x22));
  assert(envelope.accept_frame(user1_reply) == ProtocolError::NONE);
  const auto user2_reply = make_response(PacketType::READ_RESPONSE, 0x06A8, std::vector<uint8_t>(0x10, 0x33));
  assert(envelope.accept_frame(user2_reply) == ProtocolError::NONE);
  assert(envelope.state() == TransactionState::WRITE_PENDING);
  assert(envelope.pending_command().kind == CommandKind::WRITE);
  assert(envelope.pending_command().address == 0x02D0);
  assert(envelope.pending_command().bytes == clock_frame);

  // An acknowledgement for a different address is not ours: it must not retire
  // the write, because that would report success for a write that went
  // somewhere else.
  const auto wrong_ack = make_response(PacketType::WRITE_RESPONSE, 0x0300);
  assert(envelope.accept_frame(wrong_ack) == ProtocolError::STRAY_FRAME);
  assert(envelope.state() == TransactionState::WRITE_PENDING);
  assert(envelope.write_queued());

  const auto write_ack = make_response(PacketType::WRITE_RESPONSE, 0x02D0);
  assert(envelope.accept_frame(write_ack) == ProtocolError::NONE);
  assert(envelope.state() == TransactionState::END_PENDING);
  assert(!envelope.write_queued());
  const auto envelope_end = make_response(PacketType::END_RESPONSE);
  assert(envelope.accept_frame(envelope_end) == ProtocolError::NONE);
  assert(envelope.state() == TransactionState::COMPLETE);

  ResponseFrame response;
  // A start response carries no payload. Byte 5 is the echoed block size, so
  // whatever a synthetic fixture puts after the header is not payload; the real
  // device frame pinned above is the authority here.
  const auto start_frame = make_response(PacketType::START_RESPONSE, 0, {0xAA});
  assert(parse_response(start_frame, response) == ProtocolError::NONE);
  assert(response.type == PacketType::START_RESPONSE);
  assert(response.data.empty());

  const auto end_frame = make_response(PacketType::END_RESPONSE, 0, {}, 7);
  assert(parse_response(end_frame, response) == ProtocolError::NONE);
  assert(response.type == PacketType::END_RESPONSE);
  assert(response.status == 7);
  assert(response.data.empty());

  assert(parse_response({}, response) == ProtocolError::FRAME_TOO_SHORT);
  std::array<uint8_t, 7> short_frame{};
  assert(parse_response(short_frame, response) == ProtocolError::FRAME_TOO_SHORT);

  auto bad_length = read_frame;
  bad_length[0] = static_cast<uint8_t>(bad_length.size() - 1);
  assert(parse_response(bad_length, response) == ProtocolError::LENGTH_MISMATCH);

  auto bad_crc = read_frame;
  bad_crc[6] ^= 1;
  assert(parse_response(bad_crc, response) == ProtocolError::CHECKSUM_MISMATCH);

  const auto unknown_type = make_response(static_cast<PacketType>(0x8200));
  assert(parse_response(unknown_type, response) == ProtocolError::UNEXPECTED_COMMAND);

  auto bad_payload_length = read_frame;
  bad_payload_length[5] = 4;
  bad_payload_length.back() = 0;
  bad_payload_length.back() =
      xor_bytes(std::span<const uint8_t>(bad_payload_length).first(bad_payload_length.size() - 1));
  assert(parse_response(bad_payload_length, response) == ProtocolError::PAYLOAD_LENGTH_MISMATCH);

  const std::array<uint8_t, 4> nonce{0x01, 0x23, 0x45, 0x67};
  const auto token_request = make_token_request(nonce);
  assert(token_request[0] == 0x11);
  assert(std::equal(nonce.begin(), nonce.end(), token_request.begin() + 1));
  assert(std::all_of(token_request.begin() + 5, token_request.end(), [](uint8_t value) { return value == 0; }));
  const std::array<uint8_t, 6> token_reply{0x91, 0x00, 0x01, 0x23, 0x45, 0x67};
  assert(validate_token_response(token_reply, nonce) == ProtocolError::NONE);
  assert(validate_token_response({}, nonce) == ProtocolError::INVALID_TOKEN_RESPONSE);
  auto wrong_token_reply = token_reply;
  wrong_token_reply[5] ^= 1;
  assert(validate_token_response(wrong_token_reply, nonce) == ProtocolError::INVALID_TOKEN_RESPONSE);

  expect_string(protocol_error_to_string(ProtocolError::NONE), "none");
  expect_string(protocol_error_to_string(ProtocolError::RETRY_EXHAUSTED), "retry attempts exhausted");
  expect_string(protocol_error_to_string(static_cast<ProtocolError>(0xFF)), "unknown protocol error");
}

static void test_protocol_assembler() {
  const std::vector<uint8_t> payload(38, 0x5A);
  const auto frame = make_response(PacketType::READ_RESPONSE, 0x1000, payload);
  assert(frame.size() == 46);

  OmronFrameAssembler assembler(4);
  const std::span<const uint8_t> whole{frame};
  assert(assembler.add_fragment(2, whole.subspan(32)) == AssembleResult::INCOMPLETE);
  assert(assembler.add_fragment(0, whole.first(16)) == AssembleResult::INCOMPLETE);
  assert(assembler.add_fragment(1, whole.subspan(16, 16)) == AssembleResult::COMPLETE);
  assert(assembler.error() == ProtocolError::NONE);
  assert(assembler.frame() == frame);

  // A second response must not combine with higher-channel fragments retained
  // from the first response.
  const auto second = make_response(PacketType::READ_RESPONSE, 0x2000, std::vector<uint8_t>(20, 0x33));
  const std::span<const uint8_t> rest{second};
  assert(assembler.add_fragment(0, rest.first(16)) == AssembleResult::INCOMPLETE);
  assert(assembler.add_fragment(1, rest.subspan(16)) == AssembleResult::COMPLETE);
  assert(assembler.frame() == second);

  auto corrupt = second;
  corrupt[6] ^= 1;
  OmronFrameAssembler single(1);
  assert(single.add_fragment(0, corrupt) == AssembleResult::ERROR);
  assert(single.error() == ProtocolError::CHECKSUM_MISMATCH);
  assert(single.add_fragment(0, second) == AssembleResult::COMPLETE);

  OmronFrameAssembler invalid_zero(0);
  assert(invalid_zero.add_fragment(0, second) == AssembleResult::ERROR);
  assert(invalid_zero.error() == ProtocolError::INVALID_CHANNEL);
  OmronFrameAssembler invalid_five(5);
  assert(invalid_five.add_fragment(0, rest.first(16)) == AssembleResult::ERROR);
  assert(invalid_five.error() == ProtocolError::INVALID_CHANNEL);
  assert(assembler.add_fragment(4, rest.first(1)) == AssembleResult::ERROR);
  assert(assembler.error() == ProtocolError::INVALID_CHANNEL);
  assert(assembler.add_fragment(0, {}) == AssembleResult::ERROR);

  std::array<uint8_t, 17> too_wide{};
  too_wide[0] = 17;
  OmronFrameAssembler classic(4);
  assert(classic.add_fragment(0, too_wide) == AssembleResult::ERROR);
  assert(classic.error() == ProtocolError::FRAGMENT_TOO_LARGE);

  std::array<uint8_t, 8> too_short{};
  too_short[0] = 7;
  assert(classic.add_fragment(0, too_short) == AssembleResult::ERROR);
  assert(classic.error() == ProtocolError::FRAME_TOO_SHORT);

  std::array<uint8_t, 16> too_many_channels{};
  too_many_channels[0] = 65;
  assert(classic.add_fragment(0, too_many_channels) == AssembleResult::ERROR);
  assert(classic.error() == ProtocolError::LENGTH_MISMATCH);

  std::array<uint8_t, 16> incomplete_first{};
  incomplete_first[0] = 24;
  std::array<uint8_t, 3> incomplete_second{};
  classic.reset();
  assert(classic.add_fragment(0, incomplete_first) == AssembleResult::INCOMPLETE);
  assert(classic.add_fragment(1, incomplete_second) == AssembleResult::ERROR);
  assert(classic.error() == ProtocolError::LENGTH_MISMATCH);
  classic.reset();
  assert(classic.error() == ProtocolError::NONE);
  assert(classic.frame().empty());
}

static void test_transaction_engine() {
  OmronTransaction empty;
  const std::array<uint8_t, 4> zero_nonce{};
  assert(!empty.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  assert(!empty.add_read_range(0, 0, 1));
  assert(!empty.add_read_range(0, 1, 0));
  assert(!empty.add_read_range(0xFFF0, 17, 1));
  assert(empty.add_read_range(0xFFFF, 1, 1));
  empty.clear_read_ranges();

  OmronTransaction transaction;
  assert(transaction.add_read_range(0x0100, 20, 8));
  assert(transaction.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  assert(transaction.state() == TransactionState::START_PENDING);
  assert(transaction.plan().size() == 3);
  assert(transaction.plan()[0].address == 0x0100 && transaction.plan()[0].length == 8);
  assert(transaction.plan()[1].address == 0x0108 && transaction.plan()[1].length == 8);
  assert(transaction.plan()[2].address == 0x0110 && transaction.plan()[2].length == 4);
  assert(!transaction.add_read_range(0x0200, 1, 1));

  PendingCommand command = transaction.pending_command();
  assert(command.kind == CommandKind::START);
  // Golden bytes rather than make_start_request(), which is the builder the
  // transaction calls internally.
  assert(command.bytes == std::vector<uint8_t>({0x08, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x18}));
  const auto start = make_response(PacketType::START_RESPONSE);
  assert(transaction.accept_frame(start) == ProtocolError::NONE);

  for (size_t index = 0; index < transaction.plan().size(); index++) {
    command = transaction.pending_command();
    const ReadBlock expected = transaction.plan()[index];
    assert(command.kind == CommandKind::READ);
    assert(command.address == expected.address);
    assert(command.expected_length == expected.length);
    const std::vector<uint8_t> data(expected.length, static_cast<uint8_t>(index + 1));
    const auto reply = make_response(PacketType::READ_RESPONSE, expected.address, data);
    assert(transaction.accept_frame(reply) == ProtocolError::NONE);
  }
  assert(transaction.state() == TransactionState::END_PENDING);
  assert(transaction.pending_command().kind == CommandKind::END);
  const auto end = make_response(PacketType::END_RESPONSE);
  assert(transaction.accept_frame(end) == ProtocolError::NONE);
  assert(transaction.state() == TransactionState::COMPLETE);
  assert(transaction.finished());
  assert(transaction.received_blocks().size() == 3);
  assert(transaction.received_blocks()[2].data.size() == 4);

  OmronTransaction retry;
  assert(retry.add_read_range(0x1000, 1, 1));
  assert(retry.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  for (uint8_t attempt = 1; attempt < 5; attempt++) {
    assert(retry.retry_pending());
    assert(retry.attempt() == attempt);
    assert(retry.pending_command().attempt == attempt);
  }
  assert(!retry.retry_pending());
  assert(retry.state() == TransactionState::FAILED);
  assert(retry.error() == ProtocolError::RETRY_EXHAUSTED);
  assert(!retry.retry_pending());
  retry.reset();
  assert(retry.state() == TransactionState::IDLE);
  assert(retry.plan().empty());
  assert(retry.received_blocks().empty());
  assert(retry.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));  // Configured read ranges survive reset().

  const std::array<uint8_t, 4> nonce{1, 2, 3, 4};
  OmronTransaction token;
  assert(token.add_read_range(0x0200, 1, 1));
  assert(token.begin(TransactionUnlock::TOKEN_KEY, OmronBindKey{}, nonce));
  command = token.pending_command();
  assert(command.kind == CommandKind::TOKEN);
  // Written out rather than compared against make_token_request(), which is the
  // same builder the transaction calls: opcode, the four nonce bytes, then
  // fifteen zeros to a total of twenty.
  const std::vector<uint8_t> expected_token{0x11, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  assert(command.bytes == expected_token);
  const std::array<uint8_t, 6> token_ack{0x91, 0x00, 1, 2, 3, 4};
  assert(token.accept_token_response(token_ack) == ProtocolError::NONE);
  assert(token.state() == TransactionState::START_PENDING);

  OmronTransaction bad_token;
  assert(bad_token.add_read_range(0x0200, 1, 1));
  assert(bad_token.begin(TransactionUnlock::TOKEN_KEY, OmronBindKey{}, nonce));
  auto invalid_token = token_ack;
  invalid_token[5] = 5;
  assert(bad_token.accept_token_response(invalid_token) == ProtocolError::INVALID_TOKEN_RESPONSE);
  assert(bad_token.finished());

  OmronBindKey key{};
  for (size_t index = 0; index < key.size(); index++)
    key[index] = static_cast<uint8_t>(index);
  OmronTransaction classic_key;
  assert(classic_key.add_read_range(0x0300, 1, 1));
  assert(classic_key.begin(TransactionUnlock::CUSTOM_KEY, key, zero_nonce));
  command = classic_key.pending_command();
  assert(command.kind == CommandKind::KEY_AUTH);
  // Spelled out for the same reason as the token frame above: opcode 0x01 then
  // the sixteen key bytes, seventeen total.
  const std::vector<uint8_t> expected_key{0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                          0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  assert(command.bytes == expected_key);
  const std::array<uint8_t, 1> key_ack{0x81};
  assert(classic_key.accept_key_response(key_ack) == ProtocolError::NONE);
  assert(classic_key.state() == TransactionState::START_PENDING);

  // The rejection paths were untested although CLASSIC_KEY is what the classic
  // HEM-7155T profile uses. Both must fail the transaction rather than advance
  // to START_PENDING and send a request the cuff will never answer.
  OmronTransaction rejected_key;
  assert(rejected_key.add_read_range(0x0300, 1, 1));
  assert(rejected_key.begin(TransactionUnlock::CUSTOM_KEY, key, zero_nonce));
  const std::array<uint8_t, 2> key_mismatch{0x81, 0x01};
  assert(rejected_key.accept_key_response(key_mismatch) == ProtocolError::UNLOCK_KEY_REJECTED);
  assert(rejected_key.finished());
  assert(rejected_key.state() == TransactionState::FAILED);

  OmronTransaction wrong_opcode;
  assert(wrong_opcode.add_read_range(0x0300, 1, 1));
  assert(wrong_opcode.begin(TransactionUnlock::CUSTOM_KEY, key, zero_nonce));
  const std::array<uint8_t, 1> not_an_unlock_reply{0x80};
  assert(wrong_opcode.accept_key_response(not_an_unlock_reply) == ProtocolError::INVALID_UNLOCK_RESPONSE);
  assert(wrong_opcode.finished());
  expect_string(protocol_error_to_string(ProtocolError::INVALID_UNLOCK_RESPONSE), "invalid unlock response");
  expect_string(protocol_error_to_string(ProtocolError::UNLOCK_KEY_REJECTED), "device rejected the bind key");

  // A reply for an address we are not waiting on is dropped as a stray frame,
  // not treated as a protocol violation. The transaction stays live so the
  // correct reply can still arrive; only a sustained flood is terminal.
  //
  // It answers STRAY_FRAME rather than NONE, and that distinction is the whole
  // point: the transaction stayed where it was, and a caller told "no error"
  // reads that as progress. Told NONE, the session clears its wait and re-sends
  // the command still in flight, once per stray, without ever touching the
  // attempt counter meant to bound that.
  OmronTransaction wrong_address;
  assert(wrong_address.add_read_range(0x0400, 2, 2));
  assert(wrong_address.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  assert(wrong_address.accept_frame(start) == ProtocolError::NONE);
  const auto wrong_address_reply = make_response(PacketType::READ_RESPONSE, 0x0401, {1, 2});
  assert(wrong_address.accept_frame(wrong_address_reply) == ProtocolError::STRAY_FRAME);
  assert(!wrong_address.finished());
  // Still on the same block: nothing was consumed.
  assert(wrong_address.received_blocks().empty());
  const auto right_address_reply = make_response(PacketType::READ_RESPONSE, 0x0400, {1, 2});
  assert(wrong_address.accept_frame(right_address_reply) == ProtocolError::NONE);
  assert(wrong_address.received_blocks().size() == 1);

  // Sustained garbage still terminates the transaction rather than hanging.
  OmronTransaction stray_flood;
  assert(stray_flood.add_read_range(0x0400, 2, 2));
  assert(stray_flood.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  assert(stray_flood.accept_frame(start) == ProtocolError::NONE);
  ProtocolError flood_error = ProtocolError::STRAY_FRAME;
  for (int i = 0; i < 32 && flood_error == ProtocolError::STRAY_FRAME; i++)
    flood_error = stray_flood.accept_frame(wrong_address_reply);
  assert(flood_error == ProtocolError::UNEXPECTED_COMMAND);
  assert(stray_flood.finished());

  OmronTransaction wrong_size;
  assert(wrong_size.add_read_range(0x0500, 2, 2));
  assert(wrong_size.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  assert(wrong_size.accept_frame(start) == ProtocolError::NONE);
  const auto wrong_size_reply = make_response(PacketType::READ_RESPONSE, 0x0500, {1});
  assert(wrong_size.accept_frame(wrong_size_reply) == ProtocolError::PAYLOAD_LENGTH_MISMATCH);

  OmronTransaction end_error;
  assert(end_error.add_read_range(0x0600, 1, 1));
  assert(end_error.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  assert(end_error.accept_frame(start) == ProtocolError::NONE);
  const auto one_byte = make_response(PacketType::READ_RESPONSE, 0x0600, {1});
  assert(end_error.accept_frame(one_byte) == ProtocolError::NONE);
  // A result code on the end opcode is the cuff refusing something, not a
  // broken transfer: the reads were each acknowledged and their data is already
  // held, so the transaction completes and carries the code out with it.
  const auto bad_end = make_response(PacketType::END_RESPONSE, 0, {}, 0xE5);
  assert(end_error.accept_frame(bad_end) == ProtocolError::DEVICE_REPORTED_ERROR);
  assert(end_error.state() == TransactionState::COMPLETE);
  assert(end_error.end_status() == 0xE5);
  assert(end_error.received_blocks().size() == 1);

  OmronTransaction retained_range;
  assert(retained_range.add_read_range(0x0700, 1, 1));
  assert(retained_range.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  retained_range.clear_read_ranges();  // No-op while active.
  retained_range.reset();
  assert(retained_range.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
  retained_range.reset();
  retained_range.clear_read_ranges();
  assert(!retained_range.begin(TransactionUnlock::NONE, OmronBindKey{}, zero_nonce));
}

static void test_connection_gate_permutations() {
  OmronConnectionGate no_bond;
  no_bond.reset(false);
  assert(no_bond.auth_ok());
  assert(no_bond.on_connect() == ConnectionAction::NONE);
  assert(no_bond.on_open(true) == ConnectionAction::START_DISCOVERY);
  assert(no_bond.open_ok());
  no_bond.mark_discovery_started();
  assert(no_bond.discovery_started());
  assert(no_bond.on_open(true) == ConnectionAction::NONE);
  assert(no_bond.on_auth_complete(false, false) == ConnectionAction::NONE);

  OmronConnectionGate open_then_auth;
  open_then_auth.reset(true);
  assert(open_then_auth.on_connect() == ConnectionAction::START_SECURITY);
  assert(open_then_auth.on_open(true) == ConnectionAction::NONE);
  assert(open_then_auth.open_ok() && !open_then_auth.auth_ok());
  assert(open_then_auth.on_auth_complete(true, true) == ConnectionAction::START_DISCOVERY);

  OmronConnectionGate auth_then_open;
  auth_then_open.reset(true);
  assert(auth_then_open.on_auth_complete(true, true) == ConnectionAction::NONE);
  assert(auth_then_open.auth_ok() && !auth_then_open.open_ok());
  assert(auth_then_open.on_open(true) == ConnectionAction::START_DISCOVERY);

  OmronConnectionGate auth_failed;
  auth_failed.reset(true);
  assert(auth_failed.on_open(true) == ConnectionAction::NONE);
  assert(auth_failed.on_auth_complete(false, false) == ConnectionAction::DISCONNECT);
  assert(auth_failed.failed());
  assert(auth_failed.on_auth_complete(true, true) == ConnectionAction::NONE);

  OmronConnectionGate missing_bond_bit;
  missing_bond_bit.reset(true);
  assert(missing_bond_bit.on_open(true) == ConnectionAction::NONE);
  assert(missing_bond_bit.on_auth_complete(true, false) == ConnectionAction::DISCONNECT);
  assert(missing_bond_bit.failed());

  OmronConnectionGate open_failed;
  open_failed.reset(true);
  assert(open_failed.on_auth_complete(true, true) == ConnectionAction::NONE);
  assert(open_failed.on_open(false) == ConnectionAction::DISCONNECT);
  assert(open_failed.failed());
  assert(open_failed.on_open(true) == ConnectionAction::NONE);
  open_failed.reset(true);
  assert(!open_failed.failed());
}

static BloodPressureCategory category_for(uint16_t systolic, uint16_t diastolic) {
  return calculate_derived_metrics(systolic, diastolic, 60).category;
}

static void test_metrics_and_boundaries() {
  // One shared corruption gate across the decoder, the standard-GATT path and
  // the derived metrics. Three copies with their own bounds would leave the
  // widest of them deciding, and the suppression branch behind a narrower one
  // could never fire.
  assert(plausible_vitals(OMRON_MIN_SYSTOLIC, OMRON_MIN_DIASTOLIC, OMRON_MIN_PULSE));
  assert(plausible_vitals(OMRON_MAX_SYSTOLIC, OMRON_MAX_DIASTOLIC, OMRON_MAX_PULSE));
  assert(!plausible_vitals(OMRON_MIN_SYSTOLIC - 1, OMRON_MIN_DIASTOLIC, OMRON_MIN_PULSE));
  assert(!plausible_vitals(OMRON_MAX_SYSTOLIC + 1, 80, 60));
  assert(!plausible_vitals(120, OMRON_MIN_DIASTOLIC - 1, 60));
  assert(!plausible_vitals(120, OMRON_MAX_DIASTOLIC + 1, 60));
  assert(!plausible_vitals(120, 80, OMRON_MIN_PULSE - 1));
  assert(!plausible_vitals(120, 80, OMRON_MAX_PULSE + 1));
  assert(!plausible_vitals(80, 80, 60));

  const DerivedMetrics metrics = calculate_derived_metrics(120, 80, 60);
  assert(nearly_equal(metrics.pulse_pressure, 40.0f));
  assert(nearly_equal(metrics.estimated_mean_arterial_pressure, 93.333333f));
  assert(nearly_equal(metrics.shock_index, 0.5f));
  assert(nearly_equal(metrics.rate_pressure_product, 7200.0f));
  assert(metrics.category == BloodPressureCategory::HYPERTENSION_STAGE_1);
  assert(calculate_derived_metrics(0, 0, 0).category == BloodPressureCategory::UNKNOWN);

  assert(category_for(119, 79) == BloodPressureCategory::NORMAL);
  assert(category_for(120, 79) == BloodPressureCategory::ELEVATED);
  assert(category_for(129, 79) == BloodPressureCategory::ELEVATED);
  assert(category_for(130, 79) == BloodPressureCategory::HYPERTENSION_STAGE_1);
  assert(category_for(120, 80) == BloodPressureCategory::HYPERTENSION_STAGE_1);
  assert(category_for(139, 89) == BloodPressureCategory::HYPERTENSION_STAGE_1);
  assert(category_for(140, 89) == BloodPressureCategory::HYPERTENSION_STAGE_2);
  assert(category_for(139, 90) == BloodPressureCategory::HYPERTENSION_STAGE_2);
  assert(category_for(180, 120) == BloodPressureCategory::HYPERTENSION_STAGE_2);
  assert(category_for(181, 80) == BloodPressureCategory::HYPERTENSIVE_CRISIS);
  assert(category_for(180, 121) == BloodPressureCategory::HYPERTENSIVE_CRISIS);

  expect_string(blood_pressure_category_to_string(BloodPressureCategory::NORMAL), "Normal");
  expect_string(blood_pressure_category_to_string(BloodPressureCategory::ELEVATED), "Elevated");
  expect_string(blood_pressure_category_to_string(BloodPressureCategory::HYPERTENSION_STAGE_1), "Hypertension Stage 1");
  expect_string(blood_pressure_category_to_string(BloodPressureCategory::HYPERTENSION_STAGE_2), "Hypertension Stage 2");
  expect_string(blood_pressure_category_to_string(BloodPressureCategory::HYPERTENSIVE_CRISIS), "Hypertensive Crisis");
  expect_string(blood_pressure_category_to_string(static_cast<BloodPressureCategory>(0xFF)), "Unknown");
}

static void test_memory_image_and_integers() {
  const std::array<uint8_t, 4> bytes{0x12, 0x34, 0x56, 0x78};
  uint32_t value = 0;
  assert(read_integer(bytes, 1, MemoryByteOrder::BIG, value) && value == 0x12);
  assert(read_integer(bytes, 2, MemoryByteOrder::BIG, value) && value == 0x1234);
  assert(read_integer(bytes, 4, MemoryByteOrder::BIG, value) && value == 0x12345678);
  assert(read_integer(bytes, 2, MemoryByteOrder::LITTLE, value) && value == 0x3412);
  assert(read_integer(bytes, 4, MemoryByteOrder::LITTLE, value) && value == 0x78563412);
  assert(!read_integer({}, 1, MemoryByteOrder::BIG, value));
  assert(!read_integer(bytes, 0, MemoryByteOrder::BIG, value));
  assert(!read_integer(bytes, 5, MemoryByteOrder::BIG, value));
  // A width the buffer cannot cover, which is the mismatch a pointer and a
  // separate length used to make expressible in the other direction too.
  assert(!read_integer(std::span<const uint8_t>(bytes).first(1), 2, MemoryByteOrder::BIG, value));

  OmronMemoryImage image;
  const std::array<uint8_t, 3> first{1, 2, 3};
  const std::array<uint8_t, 2> newer{9, 4};
  assert(!image.add_block(0, {}));
  assert(!image.add_block(0xFFFF, newer));
  assert(image.add_block(0x0100, first));
  assert(image.add_block(0x0102, newer));
  assert(image.contains(0x0100, 4));
  assert(!image.contains(0x0100, 5));
  assert(image.contains(0xFFFF, 0));
  assert(image.read(0xFFFF, std::span<uint8_t>{}));
  std::array<uint8_t, 4> result{};
  assert(image.read(0x0100, result));
  assert((result == std::array<uint8_t, 4>{1, 2, 9, 4}));
  assert((image.read(0x0100, 4) == std::vector<uint8_t>{1, 2, 9, 4}));
  assert(image.read(0x0200, 1).empty());
  image.clear();
  assert(image.blocks().empty());

  OmronMemoryImage gap;
  assert(gap.add_block(0x0100, std::span<const uint8_t>(first).first(2)));
  assert(gap.add_block(0x0103, std::span<const uint8_t>(first).first(1)));
  assert(!gap.contains(0x0100, 4));
}

static void test_ring_and_fingerprint() {
  RingLayout ring{0x1000, 60, 16, 0x00FF, -1, 0, 59};
  uint16_t slot = 0;
  assert(normalize_write_cursor(1, ring, slot) && slot == 0);
  assert(normalize_write_cursor(0, ring, slot) && slot == 59);
  assert(normalize_write_cursor(60, ring, slot) && slot == 59);
  assert(normalize_write_cursor(61, ring, slot) && slot == 0);
  assert(normalize_write_cursor(0x8001, ring, slot) && slot == 0);

  RingLayout invalid = ring;
  invalid.record_count = 0;
  assert(!normalize_write_cursor(1, invalid, slot));
  invalid = ring;
  invalid.record_size = 0;
  assert(!normalize_write_cursor(1, invalid, slot));
  invalid = ring;
  invalid.slot_min = 10;
  invalid.slot_max = 9;
  assert(!normalize_write_cursor(1, invalid, slot));

  RingLayout nonzero_range{0x1000, 3, 4, 0x00FF, -1, 10, 12};
  assert(normalize_write_cursor(11, nonzero_range, slot) && slot == 10);
  assert(normalize_write_cursor(10, nonzero_range, slot) && slot == 12);

  uint16_t address = 0;
  assert(record_address(ring, 0, address) && address == 0x1000);
  assert(record_address(ring, 59, address) && address == 0x13B0);
  assert(!record_address(ring, 60, address));
  RingLayout overflowing{0xFFF8, 2, 8, 0x00FF, 0, 0, 1};
  assert(record_address(overflowing, 0, address) && address == 0xFFF8);
  assert(!record_address(overflowing, 1, address));

  assert((newest_first_slots(1, ring, 3) == std::vector<uint16_t>{0, 59, 58}));
  assert(newest_first_slots(1, ring).size() == 60);
  assert(newest_first_slots(1, ring, 100).size() == 60);
  assert(newest_first_slots(1, invalid, 3).empty());

  RingLayout wide_mask{0, 300, 1, 0xFFFF, -1, 0, 299};
  assert(normalize_write_cursor(0x0101, wide_mask, slot) && slot == 256);

  const std::array<uint8_t, 4> record{1, 2, 3, 4};
  const uint32_t fingerprint = measurement_fingerprint("HEM-7155T", 0, 0x1000, record);
  assert(fingerprint == measurement_fingerprint("HEM-7155T", 0, 0x1000, record));
  assert(fingerprint != measurement_fingerprint("HEM-7155T", 1, 0x1000, record));
  assert(fingerprint != measurement_fingerprint("HEM-7155T", 0, 0x1001, record));
  assert(fingerprint != measurement_fingerprint("HEM-7155T-MW3", 0, 0x1000, record));
  auto changed = record;
  changed[3] = 5;
  assert(fingerprint != measurement_fingerprint("HEM-7155T", 0, 0x1000, changed));
  // No model and no record must still fold the user and address in, or two
  // different slots on a profile-less path would collide and the second reading
  // would be dropped as a duplicate.
  const uint32_t empty = measurement_fingerprint({}, 0, 0, {});
  assert(empty != measurement_fingerprint({}, 1, 0, {}));
  assert(empty != measurement_fingerprint({}, 0, 1, {}));
}

static void test_scheduler_new_burst_beats_the_interval() {
  // Measured: a transfer press produced a minute of advertisements that the
  // interval left over from the previous successful poll swallowed whole, and
  // the records only arrived after a second button was pressed in Home
  // Assistant. The cuff keeps its radio off until somebody presses a button or
  // finishes a measurement, so a burst after silence is a request and outranks
  // the interval.
  PollPolicy policy{1000, 15, 100, 5, 20, 500, 25};
  OmronPollScheduler scheduler(policy);
  scheduler.note_advertisement(100);
  scheduler.note_poll_started(100);
  scheduler.note_poll_finished(200, true);

  scheduler.note_advertisement(250);
  assert(scheduler.should_poll(250, false, false));
  scheduler.note_poll_started(250);
  scheduler.note_poll_finished(300, true);

  // The rest of that same burst is not a second request, or the cuff would be
  // reconnected for as long as it keeps talking.
  scheduler.note_advertisement(320);
  assert(!scheduler.should_poll(320, false, false));
}

// The run that exposed it, in the log's own numbers scaled to this policy: a
// session ends, the cuff's own tail arrives a moment later, and the press that
// follows has to still count as a request. Before the tail was ignored, that
// press produced no gap, no invitation, and a 66-second burst went unanswered.
static void test_scheduler_press_after_session_tail() {
  PollPolicy policy{5000, 15, 100, 5, 20, 500, 50};
  OmronPollScheduler scheduler(policy);
  scheduler.note_advertisement(1000);
  scheduler.note_poll_started(1000);
  scheduler.note_poll_finished(1070, true);

  // Tail of the session that just ended, 4 ms later. Must not move the silence
  // clock, or it hides the gap the press is about to open.
  scheduler.note_advertisement(1074);
  assert(!scheduler.should_poll(1074, false, false));

  // The press. Only 63 ms after the tail - under the freshness window - but the
  // clock still reads 1000, so the gap is 137 and the invitation stands.
  scheduler.note_advertisement(1137);
  assert(scheduler.should_poll(1137, false, false));
  scheduler.note_poll_started(1137);
  scheduler.note_poll_finished(1200, true);
}

// There is deliberately no sequence-number gate here. Comparing the numbers the
// advertisement carries against the ones the last successful poll consumed
// costs a working short press of P: with nothing newly measured, the frame
// after a press is bit for bit the cuff's idle chatter, so the press does
// nothing.
//
// It also solves a problem this component does not have. The burst of sessions
// such a gate was built to stop came from the clear of the unsent counter never
// reaching the cuff. A served cuff stops advertising within a tenth of a
// second, which is the whole cadence mechanism this device needs.
static void test_scheduler_every_burst_after_silence_earns_a_session() {
  PollPolicy policy{5000, 15, 100, 5, 20, 500, 50};
  OmronPollScheduler scheduler(policy);

  // First burst.
  scheduler.note_advertisement(1000);
  assert(scheduler.should_poll(1000, false, false));
  scheduler.note_poll_started(1000);
  scheduler.note_poll_finished(1070, true);

  // A frame repeating what was just collected starts another session. On the
  // real cuff this frame does not exist - a cuff that has been served stops
  // advertising - so what this pins down is the rule, not a cost: nothing about
  // a readable burst is refused for repeating itself.
  scheduler.note_advertisement(1200);
  assert(scheduler.should_poll(1200, false, false));
  scheduler.note_poll_started(1200);
  scheduler.note_poll_finished(1260, true);

  // What bounds it is the minimum gap: a frame arriving too soon after the last
  // poll started is refused however fresh the burst looks.
  scheduler.note_advertisement(1270);
  assert(!scheduler.should_poll(1270, false, false));

  // A press through Home Assistant still bypasses everything.
  scheduler.request_poll();
  assert(scheduler.should_poll(1275, false, false));
  scheduler.clear_request();

  // Pairing mode, on a cuff already awake whose radio has not gone quiet.
  // Without the argument - the advertisement asking in its own words - the node
  // ignores a cuff blinking -P- at it until somebody presses a button in Home
  // Assistant. It outranks the silence window.
  scheduler.note_poll_started(1300);
  scheduler.note_poll_finished(1360, true);
  scheduler.note_advertisement(1400, false);
  const bool without_request = scheduler.should_poll(1400, false, false);
  scheduler.note_advertisement(1420, true);
  assert(scheduler.should_poll(1420, false, false));
  scheduler.note_poll_started(1420);
  scheduler.note_poll_finished(1480, false);
  // Still asking after a failed attempt: a pairing session that did not complete
  // is exactly the case that has to be allowed to try again.
  scheduler.note_advertisement(1600, true);
  assert(scheduler.should_poll(1600, false, false));
  // Named so the compiler cannot drop the call above; what it returns depends on
  // the silence window and is not what this case is about.
  (void)without_request;
}

static void test_scheduler_wrap_and_backoff() {
  PollPolicy policy{1000, 15, 10000, 5, 20};
  OmronPollScheduler scheduler(policy);
  assert(!scheduler.should_poll(100, false, false));
  scheduler.note_advertisement(100);
  assert(scheduler.advertisement_is_fresh(100));
  assert(scheduler.should_poll(100, false, false));
  assert(!scheduler.should_poll(100, true, false));
  assert(!scheduler.should_poll(100, false, true));
  scheduler.note_poll_started(100);
  assert(!scheduler.should_poll(114, false, false));
  assert(scheduler.should_poll(115, false, false));
  scheduler.note_poll_finished(200, true);
  assert(!scheduler.should_poll(1199, false, false));
  assert(scheduler.should_poll(1200, false, false));
  scheduler.request_poll();
  assert(scheduler.should_poll(200, false, false));
  scheduler.clear_request();
  assert(!scheduler.should_poll(200, false, false));

  // A cuff that drops the link while bonding gets reconnected, and the minimum
  // gap must not hold that retry back: the gap stops a chatty cuff being
  // connected on every advertisement, whereas here the cuff is awake and
  // waiting, and will be asleep again long before 15 ticks pass.
  PollPolicy drop_policy{1000, 15, 10000, 5, 20, 3};
  OmronPollScheduler dropped(drop_policy);
  dropped.note_advertisement(100);
  dropped.note_poll_started(100);
  assert(!dropped.should_poll(101, false, false));
  dropped.request_retry(101);
  // Not instant: reconnecting while the controller still holds the old link
  // starts the next pairing on top of the previous teardown.
  assert(!dropped.should_poll(103, false, false));
  assert(dropped.should_poll(104, false, false));
  // Still nothing while the link is up or the bond is being removed.
  assert(!dropped.should_poll(104, true, false));
  assert(!dropped.should_poll(104, false, true));
  // Consumed by the connect it triggers, so one drop buys one reconnect.
  dropped.note_poll_started(104);
  assert(!dropped.should_poll(105, false, false));

  OmronPollScheduler freshness(policy);
  freshness.note_advertisement(std::numeric_limits<uint32_t>::max() - 5U);
  assert(freshness.advertisement_is_fresh(3));
  assert(!freshness.advertisement_is_fresh(9994));

  PollPolicy retry_policy{100, 0, 1000, 5, 20};
  OmronPollScheduler retry(retry_policy);
  retry.note_advertisement(0);
  retry.note_poll_finished(0, false);
  assert(retry.retry_delay_ms() == 5);
  assert(!retry.should_poll(4, false, false));
  assert(retry.should_poll(5, false, false));
  retry.note_poll_finished(5, false);
  assert(retry.retry_delay_ms() == 10);
  retry.note_poll_finished(15, false);
  assert(retry.retry_delay_ms() == 20);
  retry.note_poll_finished(35, false);
  assert(retry.retry_delay_ms() == 20);
  retry.note_poll_finished(55, true);
  assert(retry.retry_delay_ms() == 0);

  PollPolicy clamped_policy{100, 0, 1000, 50, 20};
  OmronPollScheduler clamped(clamped_policy);
  clamped.note_poll_finished(0, false);
  assert(clamped.retry_delay_ms() == 20);

  PollPolicy overflow_policy{100, 0, 1000, 0x80000000UL, 0xFFFFFFFFUL};
  OmronPollScheduler overflow(overflow_policy);
  overflow.note_poll_finished(0, false);
  assert(overflow.retry_delay_ms() == 0x80000000UL);
  overflow.note_poll_finished(1, false);
  assert(overflow.retry_delay_ms() == 0xFFFFFFFFUL);

  OmronPollScheduler minimum_gap(policy);
  minimum_gap.note_advertisement(std::numeric_limits<uint32_t>::max() - 5U);
  minimum_gap.note_poll_started(std::numeric_limits<uint32_t>::max() - 5U);
  assert(!minimum_gap.should_poll(8, false, false));
  assert(minimum_gap.should_poll(9, false, false));
}

static std::vector<uint8_t> make_standard_bp_payload(uint8_t flags) {
  std::vector<uint8_t> payload{flags, 120, 0, 80, 0, 93, 0};
  if ((flags & 0x02) != 0) {
    payload.push_back(0xE8);
    payload.push_back(0x07);  // 2024
    payload.insert(payload.end(), {2, 29, 10, 20, 30});
  }
  if ((flags & 0x04) != 0) {
    payload.push_back(60);
    payload.push_back(0);
  }
  if ((flags & 0x08) != 0)
    payload.push_back(2);
  if ((flags & 0x10) != 0) {
    payload.push_back(0x27);
    payload.push_back(0);
  }
  return payload;
}

static void test_standard_bp_and_sfloat() {
  float value = 0.0f;
  assert(decode_ieee11073_sfloat(0x0078, value) && nearly_equal(value, 120.0f));
  assert(decode_ieee11073_sfloat(0x0FFB, value) && nearly_equal(value, -5.0f));
  assert(decode_ieee11073_sfloat(0xF07B, value) && nearly_equal(value, 12.3f, 0.0002f));
  assert(decode_ieee11073_sfloat(0x1002, value) && nearly_equal(value, 20.0f));
  for (uint16_t reserved : {static_cast<uint16_t>(0x07FF), static_cast<uint16_t>(0x0800), static_cast<uint16_t>(0x07FE),
                            static_cast<uint16_t>(0x0802), static_cast<uint16_t>(0x0801)})
    assert(!decode_ieee11073_sfloat(reserved, value));

  StandardBloodPressureMeasurement measurement;
  const auto basic = make_standard_bp_payload(0);
  assert(parse_standard_blood_pressure_measurement(basic, measurement) == StandardBpError::NONE);
  assert(!measurement.units_kpa);
  assert(measurement.has_systolic && nearly_equal(measurement.systolic, 120.0f));
  assert(measurement.has_diastolic && nearly_equal(measurement.diastolic, 80.0f));
  assert(measurement.has_mean_arterial_pressure && nearly_equal(measurement.mean_arterial_pressure, 93.0f));
  assert(!measurement.has_timestamp && !measurement.has_pulse_rate && !measurement.has_user_id &&
         !measurement.has_measurement_status);

  const auto complete = make_standard_bp_payload(0x1F);
  assert(parse_standard_blood_pressure_measurement(complete, measurement) == StandardBpError::NONE);
  assert(measurement.units_kpa);
  assert(measurement.has_timestamp && measurement.timestamp.year == 2024 && measurement.timestamp.month == 2 &&
         measurement.timestamp.day == 29);
  assert(measurement.has_pulse_rate && nearly_equal(measurement.pulse_rate, 60.0f));
  assert(measurement.has_user_id && measurement.user_id == 2);
  assert(measurement.has_measurement_status && measurement.measurement_status == 0x0027);
  assert(measurement.body_movement_detected());
  assert(measurement.cuff_too_loose());
  assert(measurement.irregular_pulse_detected());
  assert(measurement.improper_position_detected());

  // Blood Pressure Feature, 0x2A49. The only statement a cuff makes about its
  // own detections without being identified first, and what decides which
  // entities a live-only session may claim.
  StandardBpFeatures features{};
  const std::array<uint8_t, 2> none{0x00, 0x00};
  assert(parse_standard_bp_features(none, features));
  assert(!features.body_movement && !features.cuff_fit && !features.irregular_pulse);
  assert(!features.pulse_rate_range && !features.measurement_position && !features.multiple_bond);
  assert(features.raw == 0x0000);

  // One bit at a time, so a mask that reads the wrong bit cannot hide behind a
  // neighbour. This is the whole content of the characteristic.
  const std::array<uint8_t, 2> all{0x3F, 0x00};
  assert(parse_standard_bp_features(all, features));
  assert(features.body_movement && features.cuff_fit && features.irregular_pulse);
  assert(features.pulse_rate_range && features.measurement_position && features.multiple_bond);

  struct FeatureBit {
    uint8_t low;
    bool StandardBpFeatures::*field;
  };
  static const FeatureBit FEATURE_BITS[] = {
      {0x01, &StandardBpFeatures::body_movement},        {0x02, &StandardBpFeatures::cuff_fit},
      {0x04, &StandardBpFeatures::irregular_pulse},      {0x08, &StandardBpFeatures::pulse_rate_range},
      {0x10, &StandardBpFeatures::measurement_position}, {0x20, &StandardBpFeatures::multiple_bond},
  };
  for (const FeatureBit &bit : FEATURE_BITS) {
    const std::array<uint8_t, 2> one{bit.low, 0x00};
    StandardBpFeatures single{};
    assert(parse_standard_bp_features(one, single));
    assert(single.*(bit.field));
    int set_count = 0;
    for (const FeatureBit &other : FEATURE_BITS)
      set_count += single.*(other.field) ? 1 : 0;
    assert(set_count == 1);
  }

  // Little-endian, and a reserved bit is a newer device rather than a broken
  // one: kept in raw, ignored everywhere else.
  const std::array<uint8_t, 2> reserved{0x00, 0x80};
  assert(parse_standard_bp_features(reserved, features));
  assert(features.raw == 0x8000);
  assert(!features.body_movement && !features.multiple_bond);

  // Fixed width in the specification. Anything else is a device doing something
  // this parser has not been shown, and guessing at it would set feature bits
  // that decide which entities exist.
  const std::array<uint8_t, 3> too_long{0x01, 0x00, 0x00};
  assert(!parse_standard_bp_features(too_long, features));
  assert(!parse_standard_bp_features(std::span<const uint8_t>(none).first(1), features));
  assert(!parse_standard_bp_features({}, features));

  assert(parse_standard_blood_pressure_measurement({}, measurement) == StandardBpError::TOO_SHORT);
  std::array<uint8_t, 6> short_payload{};
  assert(parse_standard_blood_pressure_measurement(short_payload, measurement) == StandardBpError::TOO_SHORT);

  for (uint8_t optional_flag : {static_cast<uint8_t>(0x02), static_cast<uint8_t>(0x04), static_cast<uint8_t>(0x08),
                                static_cast<uint8_t>(0x10)}) {
    auto truncated = make_standard_bp_payload(0);
    truncated[0] = optional_flag;
    assert(parse_standard_blood_pressure_measurement(truncated, measurement) ==
           StandardBpError::TRUNCATED_OPTIONAL_FIELD);
  }

  // A NaN in one subfield marks that subfield absent. It must not discard the
  // rest of the reading: MAP is NaN on every cuff that does not measure it, so
  // rejecting the notification for it costs the pressures as well.
  auto nan_map = basic;
  nan_map[5] = 0xFF;
  nan_map[6] = 0x07;
  assert(parse_standard_blood_pressure_measurement(nan_map, measurement) == StandardBpError::NONE);
  assert(!measurement.has_mean_arterial_pressure);
  assert(measurement.has_systolic && nearly_equal(measurement.systolic, 120.0f));

  // A suffix the flags did not declare. Strict parsing refuses it, and the live
  // path asks for the lenient reading instead: the pressures sit at fixed
  // offsets from the front, so trailing bytes cannot have moved them, and
  // dropping the notification would cost the whole reading for a tail nobody
  // needed.
  auto with_tail = basic;
  std::vector<uint8_t> padded(with_tail.begin(), with_tail.end());
  padded.push_back(0xAA);
  assert(parse_standard_blood_pressure_measurement(padded, measurement) == StandardBpError::TRAILING_DATA);
  assert(parse_standard_blood_pressure_measurement(padded, measurement, false) == StandardBpError::NONE);
  assert(measurement.has_systolic && nearly_equal(measurement.systolic, 120.0f));
  assert(measurement.has_diastolic && nearly_equal(measurement.diastolic, 80.0f));

  auto nan_systolic = basic;
  nan_systolic[1] = 0xFF;
  nan_systolic[2] = 0x07;
  assert(parse_standard_blood_pressure_measurement(nan_systolic, measurement) == StandardBpError::NONE);
  assert(!measurement.has_systolic && measurement.has_diastolic);

  // Nothing usable left, so this one really is an error.
  auto nan_both = basic;
  nan_both[1] = 0xFF;
  nan_both[2] = 0x07;
  nan_both[3] = 0xFF;
  nan_both[4] = 0x07;
  assert(parse_standard_blood_pressure_measurement(nan_both, measurement) == StandardBpError::INVALID_SFLOAT);

  auto invalid_pulse = make_standard_bp_payload(0x04);
  invalid_pulse[7] = 0xFF;
  invalid_pulse[8] = 0x07;
  assert(parse_standard_blood_pressure_measurement(invalid_pulse, measurement) == StandardBpError::NONE);
  assert(!measurement.has_pulse_rate);

  // An unusable date drops the timestamp and keeps the reading.
  auto invalid_date = make_standard_bp_payload(0x02);
  invalid_date[9] = 2;
  invalid_date[10] = 30;  // 30 February
  assert(parse_standard_blood_pressure_measurement(invalid_date, measurement) == StandardBpError::NONE);
  assert(!measurement.has_timestamp);
  auto old_date = make_standard_bp_payload(0x02);
  old_date[7] = 0x2D;
  old_date[8] = 0x06;  // 1581, before the Gregorian epoch Date Time allows
  assert(parse_standard_blood_pressure_measurement(old_date, measurement) == StandardBpError::NONE);
  assert(!measurement.has_timestamp);
  auto unknown_year = make_standard_bp_payload(0x02);
  unknown_year[7] = 0;
  unknown_year[8] = 0;
  assert(parse_standard_blood_pressure_measurement(unknown_year, measurement) == StandardBpError::NONE);
  assert(!measurement.has_timestamp);
  auto zero_month = make_standard_bp_payload(0x02);
  zero_month[9] = 0;
  assert(parse_standard_blood_pressure_measurement(zero_month, measurement) == StandardBpError::NONE);
  assert(!measurement.has_timestamp);

  auto trailing = basic;
  trailing.push_back(0);
  assert(parse_standard_blood_pressure_measurement(trailing, measurement) == StandardBpError::TRAILING_DATA);
  assert(parse_standard_blood_pressure_measurement(trailing, measurement, false) == StandardBpError::NONE);

  expect_string(standard_bp_error_to_string(StandardBpError::NONE), "none");
  expect_string(standard_bp_error_to_string(StandardBpError::INVALID_SFLOAT), "invalid SFLOAT");
  expect_string(standard_bp_error_to_string(static_cast<StandardBpError>(0xFF)), "unknown");
}

static void test_profiles_and_aliases() {
  static constexpr std::array<OmronProfileId, 38> EXPECTED_IDS{{
      OmronProfileId::HEM_6161T,
      OmronProfileId::HEM_6232T,
      OmronProfileId::HEM_7142T2,
      // Each ring-depth split sits immediately after the family it was cut
      // from, because this list asserts catalog order. Same map, different
      // record count.
      OmronProfileId::HEM_716BT2_DEEP,
      OmronProfileId::HEM_7146T,
      OmronProfileId::HEM_7151T,
      OmronProfileId::HEM_7155T,
      OmronProfileId::HEM_7155T_MW,
      OmronProfileId::HEM_7155T_K4,
      OmronProfileId::HEM_7155T_MW3,
      OmronProfileId::HEM_7320T,
      OmronProfileId::HEM_7322T,
      OmronProfileId::HEM_7342T,
      OmronProfileId::HEM_7530T,
      OmronProfileId::HEM_7600T,
      OmronProfileId::HEM_7600T_DEEP,
      OmronProfileId::HEM_9601T,
      OmronProfileId::HEM_9700T,
      OmronProfileId::HEM_7191T1,
      OmronProfileId::HEM_7440T1,
      OmronProfileId::HEM_6401T,
      OmronProfileId::HEM_6410T,
      OmronProfileId::HEM_6231T,
      OmronProfileId::HEM_6320T,
      OmronProfileId::HEM_6321T,
      OmronProfileId::HEM_7136T,
      OmronProfileId::HEM_7150T,
      OmronProfileId::HEM_7157T_DEEP,
      OmronProfileId::HEM_7188T1,
      OmronProfileId::HEM_7361T,
      OmronProfileId::HEM_7380T1,
      OmronProfileId::HEM_7382T1,
      OmronProfileId::HEM_7386T1,
      // Each of these is commonly filed under a family whose memory map it
      // does not share.
      OmronProfileId::HEM_1026T2,
      OmronProfileId::HEM_7188T1_LE,
      OmronProfileId::HEM_7196T1,
      OmronProfileId::HEM_7377T1,
      OmronProfileId::HEM_7511T,
  }};
  // HARDWARE_VERIFIED means a reading came off a cuff this project owns, and
  // exactly one profile qualifies. The count is asserted rather than the
  // absence, so adding a second one is a deliberate edit here and not something
  // that can be inherited from a copied entry.
  size_t hardware_verified = 0;
  for (size_t index = 0; index < EXPECTED_IDS.size(); index++) {
    const OmronProfile *profile = profile_at(index);
    assert(profile != nullptr);
    if (profile->confidence == OmronProfileConfidence::HARDWARE_VERIFIED)
      hardware_verified++;
  }
  assert(hardware_verified == 1);

  // Every OS-bonding profile carries the token. The two are the same transport:
  // a cuff that bonds through the OS authenticates each session with the 0x11
  // frame and a four-byte nonce, while the older transport exchanges a stored
  // key instead and never sees a token. Four of these profiles are commonly
  // listed without the token, one of them the sibling of the cuff here, which
  // answers it.
  for (size_t index = 0; index < EXPECTED_IDS.size(); index++) {
    const OmronProfile *profile = profile_at(index);
    if (profile->security_mode != SecurityMode::OS_BOND)
      continue;
    assert(profile->unlock_mode == UnlockMode::TOKEN_KEY);
    assert(profile->token_required);
  }
  assert(get_profile(OmronProfileId::HEM_7155T_MW3).confidence == OmronProfileConfidence::HARDWARE_VERIFIED);
  assert(get_profile(OmronProfileId::HEM_7361T).confidence == OmronProfileConfidence::REFERENCE_ONLY);
  assert(get_profile(OmronProfileId::HEM_7155T).confidence == OmronProfileConfidence::REFERENCE_TESTED);
  expect_string(profile_confidence_to_string(OmronProfileConfidence::REFERENCE_ONLY),
                "transcribed from a catalog, unverified");
  assert(profile_count() == EXPECTED_IDS.size());
  assert(profile_at(profile_count()) == nullptr);
  assert(profile_at(std::numeric_limits<size_t>::max()) == nullptr);
  assert(get_profile(static_cast<OmronProfileId>(0xFF)).id == OmronProfileId::UNSUPPORTED);

  for (size_t index = 0; index < EXPECTED_IDS.size(); index++) {
    const OmronProfile *profile = profile_at(index);
    assert(profile != nullptr);
    assert(profile->id == EXPECTED_IDS[index]);
    assert(&get_profile(profile->id) == profile);
    assert(profile->model != nullptr && profile->model[0] != '\0');
    assert(profile_for_model(profile->model) == profile);
    assert(profile_matches_model(*profile, profile->model));
    // DIS is a fixed-width field, so a model id comes back with whatever the
    // cuff pads it with. Built character by character because the obvious
    // literal stops at its own embedded nul and never appends one, which leaves
    // the padding this actually has to survive untested.
    std::string padded(profile->model);
    padded.append({' ', '\t', '\r', '\n', '\0'});
    assert(profile_for_model(padded) == profile);

    assert(profile->gatt != nullptr);
    assert(profile->gatt->parent_service_uuid != nullptr);
    assert(profile->gatt->rx_channel_count > 0 && profile->gatt->rx_channel_count <= OMRON_MAX_GATT_CHANNELS);
    assert(profile->gatt->tx_channel_count > 0 && profile->gatt->tx_channel_count <= OMRON_MAX_GATT_CHANNELS);
    for (size_t channel = 0; channel < profile->gatt->rx_channel_count; channel++)
      assert(profile->gatt->rx_channel_uuids[channel] != nullptr);
    for (size_t channel = 0; channel < profile->gatt->tx_channel_count; channel++)
      assert(profile->gatt->tx_channel_uuids[channel] != nullptr);
    assert(profile->gatt->unlock_characteristic_uuid != nullptr);

    assert(profile->record_format != RecordFormat::UNSUPPORTED);
    assert(profile->record_size > 0);
    assert(profile->transmission_block_size >= profile->record_size);
    assert(profile->settings_index_region_size > 0);
    assert(profile->user_count > 0 && profile->user_count <= OMRON_MAX_USERS);
    if (profile->token_required) {
      assert(profile->unlock_mode == UnlockMode::TOKEN_KEY);
      assert(requires_os_bond(*profile));
    }
    if (remove_bond_after_session(*profile))
      assert(requires_os_bond(*profile));
    assert(requires_protocol_unlock(*profile) == (profile->unlock_mode != UnlockMode::NONE));

    // Through the ring the adapter builds, which is the only way the component
    // addresses a record. A second address calculation taking the profile
    // directly would be the one under test and not the one that runs.
    PollLayout planned{};
    assert(make_poll_layout(*profile, planned) == ProfileAdapterError::NONE);
    for (size_t user = 0; user < profile->user_count; user++) {
      const OmronUserMemoryLayout &layout = profile->users[user];
      assert(layout.record_count > 0);
      assert(static_cast<uint16_t>(layout.write_cursor_offset) + 2 <= profile->settings_index_region_size);
      const RingLayout &ring = planned.users[user].ring;
      uint16_t address = 0;
      assert(record_address(ring, 0, address));
      assert(address == layout.record_start_address);
      assert(record_address(ring, static_cast<uint16_t>(layout.record_count - 1), address));
      assert(!record_address(ring, layout.record_count, address));
    }

    for (size_t alias = 0; alias < profile->equivalent_model_id_count; alias++) {
      const char *model_alias = profile->equivalent_model_ids[alias];
      assert(model_alias != nullptr);
      assert(profile_matches_model(*profile, model_alias));
      assert(profile_for_model(model_alias) == profile);
    }

    for (size_t other = index + 1; other < EXPECTED_IDS.size(); other++)
      assert(std::strcmp(profile->model, profile_at(other)->model) != 0);
  }

  assert(profile_for_model({}) == nullptr);
  assert(profile_for_model("") == nullptr);
  assert(profile_for_model("not-an-omron") == nullptr);
  assert(profile_for_model("HEM-7155T_ESL")->id == OmronProfileId::HEM_7155T);
  assert(profile_for_model("HEM-7155T_ESL1")->id == OmronProfileId::HEM_7155T_MW3);
  assert(profile_for_model("HEM-7155T_K4-ESL")->id == OmronProfileId::HEM_7155T_K4);
}

static void test_measurement_decoders() {
  for (size_t index = 0; index < profile_count(); index++) {
    const OmronProfile &profile = *profile_at(index);
    const auto record = make_valid_record(profile);
    assert(record.size() == profile.record_size);
    OmronMeasurement measurement;
    assert(parse_measurement_record(record, profile, measurement) == MeasurementParseError::NONE);
    assert_measurement_values(profile, measurement);
    // Tied to the catalog rather than to the record format: the same layout
    // appears on models that carry a sequence number and on models that do not.
    if (profile.record_sequence_offset != NO_RECORD_SEQUENCE &&
        profile.record_format == RecordFormat::CLASSIC_VITAL_14) {
      assert(measurement.has_record_id);
      assert(measurement.record_id == 0x1234);
    } else if (profile.record_sequence_offset == NO_RECORD_SEQUENCE) {
      assert(!measurement.has_record_id);
    }
  }

  const OmronProfile &classic = get_profile(OmronProfileId::HEM_7155T);
  auto record = make_classic_record(classic);
  OmronMeasurement measurement;
  assert(parse_measurement_record(std::span<const uint8_t>(record).first(record.size() - 1), classic, measurement) ==
         MeasurementParseError::LENGTH_MISMATCH);
  const OmronProfile &unsupported = get_profile(OmronProfileId::UNSUPPORTED);
  assert(parse_measurement_record(record, unsupported, measurement) == MeasurementParseError::UNSUPPORTED_FORMAT);

  std::vector<uint8_t> empty(classic.record_size, 0);
  assert(parse_measurement_record(empty, classic, measurement) == MeasurementParseError::EMPTY_SLOT);
  std::fill(empty.begin(), empty.end(), 0xFF);
  assert(parse_measurement_record(empty, classic, measurement) == MeasurementParseError::EMPTY_SLOT);
  record = make_classic_record(classic);
  record[0] = 0xE2;
  assert(parse_measurement_record(record, classic, measurement) == MeasurementParseError::EMPTY_SLOT);

  record = make_classic_record(classic);
  record[1] = 130;
  assert(parse_measurement_record(record, classic, measurement) == MeasurementParseError::INVALID_MEASUREMENT);
  record = make_classic_record(classic);
  uint16_t flags1 = static_cast<uint16_t>(record[4] | (static_cast<uint16_t>(record[5]) << 8));
  flags1 = static_cast<uint16_t>((flags1 & ~(0x0FU << 10)) | (13U << 10));
  record[4] = static_cast<uint8_t>(flags1 & 0xFF);
  record[5] = static_cast<uint8_t>(flags1 >> 8);
  assert(parse_measurement_record(record, classic, measurement) == MeasurementParseError::INVALID_DATE);

  record = make_classic_record(classic);
  uint16_t flags2 = static_cast<uint16_t>(record[6] | (static_cast<uint16_t>(record[7]) << 8));
  flags2 = static_cast<uint16_t>((flags2 & ~0x0FFFU) | 63U | (62U << 6));
  record[6] = static_cast<uint8_t>(flags2 & 0xFF);
  record[7] = static_cast<uint8_t>(flags2 >> 8);
  assert(parse_measurement_record(record, classic, measurement) == MeasurementParseError::NONE);
  assert(measurement.timestamp.second == 59 && measurement.timestamp.minute == 59);

  // There are no bit-packed fixtures because there are no bit-packed decoders.
  // Every model here is CLASSIC_VITAL_14 apart from the 6401-family wrist units
  // with the byte-aligned layout above, so second clamping is exercised through
  // that one and nowhere else.

  assert(is_valid_datetime({2000, 2, 29, 23, 59, 59}));
  assert(!is_valid_datetime({1900, 2, 29, 0, 0, 0}));
  assert(!is_valid_datetime({0, 1, 1, 0, 0, 0}));

  expect_string(measurement_parse_error_to_string(MeasurementParseError::NONE), "none");
  expect_string(measurement_parse_error_to_string(MeasurementParseError::INVALID_MEASUREMENT),
                "invalid measurement values");
  expect_string(measurement_parse_error_to_string(static_cast<MeasurementParseError>(0xFF)),
                "unknown measurement parse error");
}

static void test_device_clock_decoder() {
  // Synthetic window: the checksum is computed with the same formula the
  // decoder verifies, so this proves the field offsets and the guards, not the
  // checksum itself. A window captured from a cuff would close that gap.
  uint8_t window[16] = {};
  window[8] = 25;  // 2025
  window[9] = 8;
  window[10] = 4;
  window[11] = 20;
  window[12] = 47;
  window[13] = 13;
  uint32_t sum = 0;
  for (size_t i = 0; i < 14; i++)
    sum += window[i];
  window[14] = static_cast<uint8_t>(sum & 0xFF);

  OmronDateTime clock{};
  assert(parse_device_clock(window, 8, clock) == ClockParseError::NONE);
  assert(clock.year == 2025 && clock.month == 8 && clock.day == 4);
  assert(clock.hour == 20 && clock.minute == 47 && clock.second == 13);

  assert(parse_device_clock(window, NO_CLOCK, clock) == ClockParseError::UNSUPPORTED_LAYOUT);
  assert(parse_device_clock(std::span<const uint8_t>(window).first(14), 8, clock) == ClockParseError::LENGTH_MISMATCH);

  // The offset is read, not assumed. Every profile shipping today states eight,
  // so a decoder that ignored the argument would pass every other assertion
  // here and every gate on the catalog. This is a window built at twelve.
  uint8_t shifted[20] = {};
  std::memcpy(shifted + 12, window + 8, 6);
  uint32_t shifted_sum = 0;
  for (size_t i = 0; i < 18; i++)
    shifted_sum += shifted[i];
  shifted[18] = static_cast<uint8_t>(shifted_sum & 0xFF);
  OmronDateTime moved{};
  assert(parse_device_clock(shifted, 12, moved) == ClockParseError::NONE);
  assert(moved.year == 2025 && moved.month == 8 && moved.day == 4);
  assert(moved.hour == 20 && moved.minute == 47 && moved.second == 13);
  // Same bytes read at the offset the rest of the catalog uses: six bytes of
  // zero, which is not a date. Reading one window at the other's offset is the
  // failure this whole field exists to stop.
  assert(parse_device_clock(shifted, 8, moved) == ClockParseError::INVALID_DATE);

  // A window that cannot hold the fields its profile claims is refused rather
  // than decoded past its end.
  assert(parse_device_clock(window, 12, clock) == ClockParseError::LENGTH_MISMATCH);

  // And no profile may claim one. The offset comes from the model's own
  // definition and the region from ours, so the two can disagree in the
  // catalog without either being obviously wrong on its own.
  for (size_t index = 0; index < profile_count(); index++) {
    const OmronProfile *entry = profile_at(index);
    if (entry->clock_fields_offset == NO_CLOCK)
      continue;
    assert(entry->time_region_end > entry->time_region_start);
    const size_t window_size = static_cast<size_t>(entry->time_region_end - entry->time_region_start);
    assert(static_cast<size_t>(entry->clock_fields_offset) + 6 + 2 <= window_size);
  }

  uint8_t corrupted[16];
  std::memcpy(corrupted, window, sizeof(window));
  corrupted[14] ^= 0xFF;
  assert(parse_device_clock(corrupted, 8, clock) == ClockParseError::CHECKSUM_MISMATCH);

  // Captured from a cuff whose clock was never set: the checksum is correct and
  // the seconds field holds 0x3F, which is not a time. The fields still come
  // back so the caller can show what the device actually holds.
  const uint8_t never_set[16] = {0xC8, 0xA9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x13, 0x01, 0x01, 0x00, 0x00, 0x3F, 0xC5, 0x00};
  OmronDateTime unset{};
  assert(parse_device_clock(never_set, 8, unset) == ClockParseError::INVALID_DATE);
  assert(unset.year == 2019 && unset.month == 1 && unset.day == 1);
  assert(unset.hour == 0 && unset.minute == 0 && unset.second == 63);

  // Drift is a difference of two local wall clocks, so it must come out of pure
  // arithmetic. Routing it through mktime made it depend on libc's TZ, and on a
  // node where TZ was unset a cuff one second behind reported 7199 seconds
  // ahead: exactly the local offset, measured on hardware.
  const OmronDateTime node{2026, 8, 4, 21, 19, 23};
  const OmronDateTime cuff{2026, 8, 4, 21, 19, 22};
  assert(civil_seconds(cuff) - civil_seconds(node) == -1);
  assert(civil_seconds(OmronDateTime{2026, 1, 1, 0, 0, 0}) - civil_seconds(OmronDateTime{2025, 12, 31, 23, 59, 59}) ==
         1);
  // A leap day must not shift the count.
  assert(civil_seconds(OmronDateTime{2024, 3, 1, 0, 0, 0}) - civil_seconds(OmronDateTime{2024, 2, 29, 0, 0, 0}) ==
         86400);
  assert(civil_seconds(OmronDateTime{2023, 3, 1, 0, 0, 0}) - civil_seconds(OmronDateTime{2023, 2, 28, 0, 0, 0}) ==
         86400);
  // Unix epoch, the anchor the constant 719468 encodes.
  assert(civil_seconds(OmronDateTime{1970, 1, 1, 0, 0, 0}) == 0);

  // The clock window is an offset inside the settings block, so its address
  // only exists once the block address is added: 0x0260 + 0x2C.
  PollLayout layout;
  assert(make_poll_layout(get_profile(OmronProfileId::HEM_7155T_MW3), layout) == ProfileAdapterError::NONE);
  assert(layout.clock_address == 0x028C);
  assert(layout.clock_size == 0x10);
}

static void test_advertisement_flags() {
  OmronAdvertisementFlags flags;

  // Captured from the cuff during a pairing-mode session, clock still unset.
  // This is the byte pair that explains the whole trigger model: the device
  // advertises a reason, and invalid_time was that reason all evening.
  const uint8_t captured[8] = {0x01, 0x2D, 0x09, 0x00, 0x09, 0x0B, 0x00, 0x0B};
  assert(parse_advertisement_flags(captured, flags));
  assert(flags.format == 0x01);
  assert(flags.user_register_count == 1);
  assert(flags.invalid_time && flags.pairing_mode && !flags.forced_transfer);
  assert(flags.wants_session());

  // Same cuff with the clock set and nothing to report: no invitation, so no
  // connection. Clearing 0x04 and 0x08 leaves 0x21.
  uint8_t quiet[8];
  std::memcpy(quiet, captured, sizeof(captured));
  quiet[1] = 0x21;
  assert(parse_advertisement_flags(quiet, flags));
  assert(!flags.invalid_time && !flags.pairing_mode);
  assert(!flags.wants_session());

  // A finished measurement asks for a transfer on its own.
  quiet[1] = 0x41;
  assert(parse_advertisement_flags(quiet, flags));
  assert(flags.forced_transfer && flags.wants_session());

  // The same captured frame carries a sequence number per user, which is how a
  // host knows who has a new reading without connecting: "user N's sequence is
  // not the one I last saw". Two bytes little-endian, stride three from offset
  // two, one more block than the field in the flag byte. 0x2D & 3 is one, so
  // two blocks: 09 00 09 | 0B 00 0B.
  assert(parse_advertisement_flags(captured, flags));
  assert(flags.sequence_count == 2);
  assert(flags.user_sequence[0] == 9);
  assert(flags.user_sequence[1] == 11);

  // The shortest frame this format accepts still carries both, because the
  // second block's last byte lands on the final index rather than past it.
  assert(parse_advertisement_flags(std::span<const uint8_t>(captured).first(7), flags));
  assert(flags.sequence_count == 2 && flags.user_sequence[1] == 11);

  // Format 0x01 declares three bytes per registered user; one user needs 7.
  assert(!parse_advertisement_flags(std::span<const uint8_t>(captured).first(6), flags));
  assert(parse_advertisement_flags(std::span<const uint8_t>(captured).first(7), flags));

  // Older format, three bytes, and the flag byte sits where it always does.
  // It carries no transfer bit at all, so a cuff on it can only ask through the
  // other two.
  const uint8_t legacy[3] = {0x03, 0x1E, 0x2A};
  assert(parse_advertisement_flags(legacy, flags));
  assert(flags.format == 0x03 && flags.user_register_count == 2);
  assert(flags.invalid_time && flags.pairing_mode && !flags.forced_transfer);
  assert(flags.wants_session());
  assert(!parse_advertisement_flags(std::span<const uint8_t>(legacy).first(2), flags));

  // Fixed-length formats reject anything but their exact size, so a truncated
  // advertisement cannot be mistaken for one with no flags set.
  const uint8_t bls[13] = {0x08, 0x01};
  assert(parse_advertisement_flags(bls, flags));
  assert(!parse_advertisement_flags(std::span<const uint8_t>(bls).first(12), flags));
  const uint8_t newest[11] = {0x09, 0x01};
  assert(parse_advertisement_flags(newest, flags));
  assert(!parse_advertisement_flags(std::span<const uint8_t>(newest).first(10), flags));

  // An unknown format is not an invitation.
  const uint8_t unknown[4] = {0x77, 0xFF, 0xFF, 0xFF};
  assert(!parse_advertisement_flags(unknown, flags));
  assert(!parse_advertisement_flags({}, flags));
  assert(!parse_advertisement_flags(std::span<const uint8_t>(captured).first(1), flags));
}

static void test_clock_write_builder() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  // The window the cuff actually returned, clock never set.
  const uint8_t window[16] = {0xC8, 0xA9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x13, 0x01, 0x01, 0x00, 0x00, 0x3F, 0xC5, 0x00};
  const OmronDateTime target{2026, 8, 4, 21, 5, 30};

  std::vector<uint8_t> request;
  uint16_t address = 0;
  assert(build_clock_write_request(mw3, window, target, request, address) == ClockWriteError::NONE);
  // settings_write_address 0x02A4 plus the region offset 0x2C. Deliberately not
  // the read address: reading came from 0x028C.
  assert(address == 0x02D0);
  assert(address != mw3.settings_read_address + mw3.time_region_start);

  assert(request.size() == sizeof(window) + WRITE_REQUEST_OVERHEAD);
  assert(request[0] == request.size());
  assert(request[1] == 0x01 && request[2] == 0xC0);
  assert(request[3] == 0x02 && request[4] == 0xD0);
  assert(request[5] == sizeof(window));
  assert(xor_bytes(request) == 0);

  // Payload starts at 6: bytes outside the clock fields are returned untouched,
  // the six fields carry the new time, and the checksum is recomputed.
  const uint8_t *payload = request.data() + 6;
  assert(payload[0] == 0xC8 && payload[1] == 0xA9);
  assert(payload[8] == 26 && payload[9] == 8 && payload[10] == 4);
  assert(payload[11] == 21 && payload[12] == 5 && payload[13] == 30);
  uint32_t sum = 0;
  for (size_t i = 0; i < 14; i++)
    sum += payload[i];
  assert(payload[14] == static_cast<uint8_t>(sum & 0xFF));
  // The rebuilt window must decode back to the time that was asked for.
  OmronDateTime round_trip{};
  assert(parse_device_clock(std::span<const uint8_t>(payload, sizeof(window)), 8, round_trip) == ClockParseError::NONE);
  assert(round_trip.year == 2026 && round_trip.month == 8 && round_trip.day == 4);
  assert(round_trip.hour == 21 && round_trip.minute == 5 && round_trip.second == 30);

  // Refusals. Every one of these would otherwise aim a write somewhere.
  assert(build_clock_write_request(mw3, {}, target, request, address) == ClockWriteError::NULL_ARGUMENT);
  assert(build_clock_write_request(mw3, std::span<const uint8_t>(window).first(15), target, request, address) ==
         ClockWriteError::WINDOW_LENGTH);
  assert(build_clock_write_request(mw3, window, OmronDateTime{2026, 13, 4, 21, 5, 30}, request, address) ==
         ClockWriteError::INVALID_TIME);
  assert(build_clock_write_request(mw3, window, OmronDateTime{1999, 8, 4, 21, 5, 30}, request, address) ==
         ClockWriteError::INVALID_TIME);
  assert(request.empty());

  OmronProfile no_write = mw3;
  no_write.settings_write_address = 0;
  assert(build_clock_write_request(no_write, window, target, request, address) == ClockWriteError::WRITE_UNSUPPORTED);

  OmronProfile other_layout = mw3;
  other_layout.clock_fields_offset = NO_CLOCK;
  assert(build_clock_write_request(other_layout, window, target, request, address) ==
         ClockWriteError::UNSUPPORTED_LAYOUT);
}

// Registering a user takes two runs and the end command closes the session, so
// both have to leave on one connection. Without this the second is dropped
// ("could not be queued") and the index region goes out alone.
static void test_transaction_carries_two_writes_before_end() {
  OmronTransaction envelope;
  assert(envelope.add_read_range(0x0260, 0x10, 0x10));
  assert(envelope.begin(TransactionUnlock::NONE, OmronBindKey{}, {0x00, 0x00, 0x00, 0x00}));
  const auto start_reply = make_response(PacketType::START_RESPONSE, 0x0000);
  assert(envelope.accept_frame(start_reply) == ProtocolError::NONE);

  const std::vector<uint8_t> index = make_write_request(0x02A4, std::vector<uint8_t>(24, 0x11));
  const std::vector<uint8_t> block = make_write_request(0x02C6, std::vector<uint8_t>(10, 0x22));

  // Queued after the reads are already done, which is when the client actually
  // builds them: the first call resumes the envelope into WRITE_PENDING and the
  // second has to be accepted from there, not rejected for being in the wrong
  // state. On hardware that rejection sent the region without its block.
  const auto read_reply = make_response(PacketType::READ_RESPONSE, 0x0260, std::vector<uint8_t>(0x10, 0x44));
  assert(envelope.accept_frame(read_reply) == ProtocolError::NONE);
  assert(envelope.state() == TransactionState::END_PENDING);
  assert(envelope.queue_write(0x02A4, index));
  assert(envelope.state() == TransactionState::WRITE_PENDING);
  assert(envelope.queue_write(0x02C6, block));

  // The region ahead of the blocks goes first, in buffer order.
  assert(envelope.state() == TransactionState::WRITE_PENDING);
  assert(envelope.pending_command().address == 0x02A4);
  const auto index_ack = make_response(PacketType::WRITE_RESPONSE, 0x02A4);
  assert(envelope.accept_frame(index_ack) == ProtocolError::NONE);

  // Acknowledged, and the block follows on the same connection rather than the
  // session ending under it.
  assert(envelope.state() == TransactionState::WRITE_PENDING);
  assert(envelope.pending_command().address == 0x02C6);
  assert(envelope.pending_command().bytes == block);
  const auto block_ack = make_response(PacketType::WRITE_RESPONSE, 0x02C6);
  assert(envelope.accept_frame(block_ack) == ProtocolError::NONE);
  assert(envelope.state() == TransactionState::END_PENDING);
  assert(!envelope.write_queued());
}

static void test_write_frame_is_built_when_it_is_sent() {
  OmronTransaction envelope;
  assert(envelope.add_read_range(0x0260, 0x10, 0x10));
  assert(envelope.begin(TransactionUnlock::NONE, OmronBindKey{}, {0x00, 0x00, 0x00, 0x00}));
  const auto start_reply = make_response(PacketType::START_RESPONSE, 0x0000);
  assert(envelope.accept_frame(start_reply) == ProtocolError::NONE);
  const auto read_reply = make_response(PacketType::READ_RESPONSE, 0x0260, std::vector<uint8_t>(0x10, 0x44));
  assert(envelope.accept_frame(read_reply) == ProtocolError::NONE);

  // Stands in for the wall clock: whatever the builder reads when it runs is
  // what lands on the wire. The cuff's clock write was a second stale because
  // the value was sampled when the read phase ended rather than at this point.
  uint8_t tick = 0x01;
  const auto build_clock_run = [&tick]() { return make_write_request(0x02D0, std::vector<uint8_t>(16, tick)); };
  assert(envelope.queue_write(0x02D0, build_clock_run));
  assert(envelope.state() == TransactionState::WRITE_PENDING);

  // Queued at one value, sent at another. Without the rebuild the value from
  // queue time goes out, which for a clock means a stale one.
  tick = 0x02;
  assert(envelope.refresh_pending_write());
  const auto sent = envelope.pending_command().bytes;
  assert(sent == make_write_request(0x02D0, std::vector<uint8_t>(16, 0x02)));

  // A retry gets the time of the retry, not of the first attempt.
  tick = 0x03;
  assert(envelope.refresh_pending_write());
  assert(envelope.pending_command().bytes == make_write_request(0x02D0, std::vector<uint8_t>(16, 0x03)));

  // A builder that answers for a different address is refused outright: the
  // engine composes nothing and must not be talked into writing elsewhere, and
  // a frame whose address disagrees would never match its own reply either.
  const auto build_elsewhere = []() { return make_write_request(0x02A4, std::vector<uint8_t>(16, 0xEE)); };
  OmronTransaction hijacked;
  assert(hijacked.add_read_range(0x0260, 0x10, 0x10));
  assert(hijacked.begin(TransactionUnlock::NONE, OmronBindKey{}, {0x00, 0x00, 0x00, 0x00}));
  assert(hijacked.accept_frame(start_reply) == ProtocolError::NONE);
  assert(hijacked.accept_frame(read_reply) == ProtocolError::NONE);
  const std::vector<uint8_t> honest = make_write_request(0x02D0, std::vector<uint8_t>(16, 0x11));
  assert(hijacked.queue_write(0x02D0, honest));
  assert(!hijacked.refresh_pending_write());  // no builder at all
  assert(hijacked.pending_command().bytes == honest);

  OmronTransaction reused;
  assert(reused.add_read_range(0x0260, 0x10, 0x10));
  assert(reused.begin(TransactionUnlock::NONE, OmronBindKey{}, {0x00, 0x00, 0x00, 0x00}));
  assert(reused.accept_frame(start_reply) == ProtocolError::NONE);
  assert(reused.accept_frame(read_reply) == ProtocolError::NONE);
  assert(!reused.queue_write(0x02D0, WriteFrameBuilder{}));  // an absent builder queues nothing
  assert(!reused.queue_write(0x02D0, build_elsewhere));      // nor does one aimed elsewhere
  assert(!reused.write_queued());
  assert(!reused.refresh_pending_write());

  // The case the queue-time check cannot see: a builder honest when queued and
  // answering elsewhere later. The frame from queue time is what goes out.
  uint16_t target = 0x02D0;
  const auto build_drifting = [&target]() { return make_write_request(target, std::vector<uint8_t>(16, 0x22)); };
  OmronTransaction drifting;
  assert(drifting.add_read_range(0x0260, 0x10, 0x10));
  assert(drifting.begin(TransactionUnlock::NONE, OmronBindKey{}, {0x00, 0x00, 0x00, 0x00}));
  assert(drifting.accept_frame(start_reply) == ProtocolError::NONE);
  assert(drifting.accept_frame(read_reply) == ProtocolError::NONE);
  assert(drifting.queue_write(0x02D0, build_drifting));
  target = 0x02A4;
  assert(!drifting.refresh_pending_write());
  assert(drifting.pending_command().bytes == make_write_request(0x02D0, std::vector<uint8_t>(16, 0x22)));

  // A builder with nothing to say leaves the queue as it was rather than
  // sending an empty frame, and one that has nothing to say at queue time never
  // gets queued in the first place.
  OmronTransaction silent;
  assert(silent.add_read_range(0x0260, 0x10, 0x10));
  assert(silent.begin(TransactionUnlock::NONE, OmronBindKey{}, {0x00, 0x00, 0x00, 0x00}));
  assert(silent.accept_frame(start_reply) == ProtocolError::NONE);
  assert(silent.accept_frame(read_reply) == ProtocolError::NONE);
  assert(!silent.queue_write(0x02D0, []() { return std::vector<uint8_t>{}; }));
  assert(!silent.write_queued());
}

static void test_user_block_geometry_against_captured_settings() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  const std::vector<uint8_t> captured = {0x09, 0x80, 0x0d, 0x00, 0x00, 0x80, 0x00, 0x80, 0x09, 0x00, 0x00,
                                         0x80, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
                                         0x00, 0x00, 0x01, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00};
  // Same buffer one connection earlier: user 1 still erased, and an erased block
  // carries a checksum too.
  const std::vector<uint8_t> erased_user1 = {0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x00};

  const uint8_t expected_offsets[2] = {24, 34};
  for (uint8_t user = 1; user <= 2; user++) {
    OmronSettingsBlock block{};
    assert(user_settings_block(mw3, user, block));
    assert(block.offset == expected_offsets[user - 1] && block.size == 10);
    uint32_t sum = 0;
    for (size_t i = block.offset; i < static_cast<size_t>(block.offset) + block.size - 2; i++)
      sum += captured[i];
    assert(captured[block.offset + block.size - 2] == static_cast<uint8_t>(sum & 0xFF));
  }

  uint32_t erased_sum = 0;
  for (size_t i = 0; i < erased_user1.size() - 2; i++)
    erased_sum += erased_user1[i];
  assert(erased_user1[erased_user1.size() - 2] == static_cast<uint8_t>(erased_sum & 0xFF));

  // The last user block has to stop where the clock starts, or the write span
  // would either cut a block short or run into a region the cuff maintains.
  OmronSettingsBlock last{};
  assert(user_settings_block(mw3, mw3.user_count, last));
  assert(static_cast<size_t>(last.offset) + last.size == mw3.time_region_start);
  assert(captured.size() == mw3.time_region_start);

  // The version counter, pinned to a real registration. This is the buffer as
  // it stood just before user 2 registered; that block's 01 00 00 00 became
  // 02 00 00 00 with every other byte untouched, so the counter is little
  // endian and one step is one step. This builder has to land on the same four
  // bytes from the same input.
  const std::vector<uint8_t> before_user2 = {0x09, 0x80, 0x0d, 0x00, 0x09, 0x80, 0x01, 0x00, 0x09, 0x00, 0x00,
                                             0x80, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe,
                                             0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00};
  // The full buffer including the clock, so the run builder sees what a session
  // actually reads. Clock as captured, marker byte clear, as every read shows it.
  std::vector<uint8_t> full_buffer = before_user2;
  const uint8_t clock_as_read[16] = {0xc8, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x1a, 0x08, 0x06, 0x17, 0x0d, 0x2d, 0xe9, 0x00};
  full_buffer.insert(full_buffer.end(), clock_as_read, clock_as_read + sizeof(clock_as_read));

  // Registering user 2 is two frames: 24 bytes at 0x02A4, then 26 at 0x02C6
  // carrying the block with its counter one step on and the clock behind it.
  // User 1's block lies between, so these cannot merge.
  const OmronDateTime target{2026, 8, 6, 21, 18, 24};
  SessionSettingsUpdate update{};
  update.user_number = 2;
  update.register_block = true;
  update.clock = target;
  std::vector<SettingsWriteFrame> writes;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  assert(writes.size() == 2);
  assert(writes[0].address == 0x02A4 && writes[0].frame[5] == 24);
  assert(writes[1].address == 0x02C6 && writes[1].frame[5] == 26);
  const uint8_t *pointers = writes[0].frame.data() + 6;
  assert(pointers[6] == 0x00 && pointers[7] == 0x80);  // user 2's field claimed
  assert(pointers[4] == 0x09 && pointers[5] == 0x80);  // user 1's left alone
  const uint8_t *run = writes[1].frame.data() + 6;
  assert(run[4] == 0x02 && run[5] == 0x00 && run[6] == 0x00 && run[7] == 0x00);
  uint32_t block_sum = 0;
  for (size_t i = 0; i < 8; i++)
    block_sum += run[i];
  assert(run[8] == static_cast<uint8_t>(block_sum & 0xFF));
  assert(run[10 + 4] == 0x02);  // marker bit for user 2
  assert(run[10 + 8] == 26 && run[10 + 9] == 8 && run[10 + 10] == 6);
  assert(run[10 + 11] == 21 && run[10 + 12] == 18 && run[10 + 13] == 24);
  uint32_t clock_sum = 0;
  for (size_t i = 10; i < 10 + 14; i++)
    clock_sum += run[i];
  assert(run[10 + 14] == static_cast<uint8_t>(clock_sum & 0xFF));
  for (const auto &write : writes)
    assert(xor_bytes(write.frame) == 0);

  // Registering user 1: one 34-byte run at 0x02A4, because that user's block
  // touches the pointer region, then the clock alone at 0x02D0. Same rule as
  // above, different shape - not a 24-byte run followed by an invented one at
  // 0x02BC.
  update.user_number = 1;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  assert(writes.size() == 2);
  assert(writes[0].address == 0x02A4 && writes[0].frame[5] == 34);
  assert(writes[1].address == 0x02D0 && writes[1].frame[5] == 16);
  assert((writes[0].frame.data() + 6)[4] == 0x00 && (writes[0].frame.data() + 6)[5] == 0x80);
  assert((writes[1].frame.data() + 6)[4] == 0x01);  // marker bit for user 1

  // A block that does not add up as read is refused. Not-all-zero is not enough
  // on its own: it catches a dump of zeros and nothing else, while the failure
  // that has actually happened twice is a block read under the wrong base
  // address, full of real bytes that belong to something else.
  //
  // It matters most here of all places. This is the frame that steps the version
  // counter, the cuff accepts it because the counter moved rather than because
  // the bytes made sense, and what it overwrites exists nowhere else.
  {
    // Summed here rather than by calling the builder's own helper. A fixture
    // built with the inverse of the function under test proves nothing, and
    // this suite has already been caught doing that once.
    const auto stored_sum = [](const std::vector<uint8_t> &buffer, const OmronSettingsBlock &target) {
      uint32_t total = 0;
      for (size_t i = target.offset; i + 2 < static_cast<size_t>(target.offset) + target.size; i++)
        total += buffer[i];
      return static_cast<uint8_t>(total & 0xFF);
    };
    const auto checksum_byte = [](const OmronSettingsBlock &target) {
      return static_cast<size_t>(target.offset) + target.size - 2;
    };

    std::vector<uint8_t> corrupted = full_buffer;
    OmronSettingsBlock user2{};
    assert(user_settings_block(mw3, 2, user2));
    // As read, it reproduces: this buffer came off the cuff.
    assert(corrupted[checksum_byte(user2)] == stored_sum(corrupted, user2));
    // One byte of the block moves and the stored sum no longer describes it.
    corrupted[user2.offset + 1] = static_cast<uint8_t>(corrupted[user2.offset + 1] + 1);
    assert(corrupted[checksum_byte(user2)] != stored_sum(corrupted, user2));

    SessionSettingsUpdate refuse{};
    refuse.user_number = 2;
    refuse.register_block = true;
    refuse.clock = target;
    std::vector<SettingsWriteFrame> refused;
    assert(build_session_settings_writes(mw3, refuse, corrupted, refused) == SettingsWriteError::CHECKSUM_MISMATCH);
    assert(refused.empty());

    // The other user's block being wrong is not this write's problem: a session
    // registering user 2 must not be held up by bytes it is not touching.
    std::vector<uint8_t> other = full_buffer;
    OmronSettingsBlock user1{};
    assert(user_settings_block(mw3, 1, user1));
    other[user1.offset + 1] = static_cast<uint8_t>(other[user1.offset + 1] + 1);
    assert(build_session_settings_writes(mw3, refuse, other, refused) == SettingsWriteError::NONE);

    // And an ordinary session is unaffected either way - it never touches a
    // user block, so a corrupt one is not its business.
    refuse.register_block = false;
    assert(build_session_settings_writes(mw3, refuse, corrupted, refused) == SettingsWriteError::NONE);
  }
  update.user_number = 1;
  update.register_block = true;

  // Registered as user 2, but this session read user 1's records too. Both
  // counters have to come back cleared, or the cuff goes on advertising for the
  // user nobody told it about.
  update.register_block = false;
  update.user_number = 2;
  update.collected_users = 0x03;
  full_buffer[4] = 0x01;  // user 1: one measurement not yet collected
  full_buffer[5] = 0x00;
  full_buffer[6] = 0x01;  // user 2: same
  full_buffer[7] = 0x00;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  const uint8_t *region = writes[0].frame.data() + 6;
  assert(region[4] == 0x00 && region[5] == 0x80);
  assert(region[6] == 0x00 && region[7] == 0x80);

  // With only user 2 collected, user 1's counter is left exactly as read.
  // Clearing a counter for records this session never fetched would tell the
  // cuff a lie.
  update.collected_users = 0x02;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  region = writes[0].frame.data() + 6;
  assert(region[4] == 0x01 && region[5] == 0x00);
  assert(region[6] == 0x00 && region[7] == 0x80);
  full_buffer[4] = 0x00;
  full_buffer[5] = 0x80;
  full_buffer[6] = 0x00;
  full_buffer[7] = 0x80;
  update.collected_users = 0;

  // An ordinary session leaves the block alone: the pointer region and the
  // marked clock, nothing else.
  update.register_block = false;
  update.user_number = 2;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  assert(writes.size() == 2);
  assert(writes[0].address == 0x02A4 && writes[0].frame[5] == 24);
  assert(writes[1].address == 0x02D0 && writes[1].frame[5] == 16);
  const uint8_t *untouched = writes[1].frame.data() + 6;
  assert(untouched[8] == 26 && untouched[11] == 21);  // clock still set

  // No time to offer leaves the clock exactly as read, marker and checksum aside.
  update.clock.reset();
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  const uint8_t *as_read = writes[1].frame.data() + 6;
  for (size_t i = 8; i < 14; i++)
    assert(as_read[i] == clock_as_read[i]);

  // A block that reads empty is a read that did not happen. Registering over it
  // writes a block of zeros into somebody's settings.
  const std::vector<uint8_t> zeros(60, 0x00);
  update.register_block = true;
  assert(build_session_settings_writes(mw3, update, zeros, writes) == SettingsWriteError::EMPTY_BLOCK);
  assert(build_session_settings_writes(mw3, update, {}, writes) == SettingsWriteError::NULL_ARGUMENT);
  update.user_number = 3;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::UNSUPPORTED_MODEL);

  // A clock window too short for the six fields plus the checksum that closes
  // it. Refused rather than written: the fields would land past the end of the
  // window and over whatever settings follow, with the checksum on top of one
  // of them.
  update.user_number = 2;
  update.register_block = false;
  OmronProfile short_clock = mw3;
  short_clock.time_region_end = static_cast<uint8_t>(short_clock.time_region_start + 15);
  assert(build_session_settings_writes(short_clock, update, full_buffer, writes) ==
         SettingsWriteError::UNSUPPORTED_MODEL);
  short_clock.time_region_end = static_cast<uint8_t>(short_clock.time_region_start + 16);
  assert(build_session_settings_writes(short_clock, update, full_buffer, writes) == SettingsWriteError::NONE);
  update.register_block = true;
  update.user_number = 3;

  // The birth date reader against the writer, through a real registration run:
  // whatever the builder puts in the block has to come back out of it. A reader
  // that disagreed with the writer would report a landed write as a failed one,
  // and that readback is now the only evidence a user sees in Home Assistant.
  // Date invented for the test; the real one belongs in secrets, not in a repo.
  const OmronDateTime born{1970, 3, 9, 0, 0, 0};
  update.user_number = 2;
  update.register_block = true;
  update.birth_date = born;
  assert(build_session_settings_writes(mw3, update, full_buffer, writes) == SettingsWriteError::NONE);
  // Rebuild the settings image the way the cuff would hold it after that write,
  // so the reader is fed a buffer rather than a frame.
  std::vector<uint8_t> after_write = full_buffer;
  const uint8_t *written_run = writes[1].frame.data() + 6;
  for (size_t i = 0; i < 26; i++)
    after_write[34 + i] = written_run[i];

  OmronDateTime read_back{};
  assert(user_birth_date(mw3, 2, after_write, read_back));
  assert(read_back.year == 1970 && read_back.month == 3 && read_back.day == 9);
  // The counter moved with it, which is what makes the cuff take the block.
  assert(user_settings_version(mw3, 2, after_write) == user_settings_version(mw3, 2, full_buffer) + 1);

  // Two different empty states, and they must not be confused. A block the cuff
  // has set up but nobody has given a date to reads 00 01 01, which is a real
  // date and the honest answer "not set". A block still erased from the factory
  // reads ff ff 00, which is not a date at all and must not publish as one.
  OmronDateTime never_set{};
  assert(user_birth_date(mw3, 1, captured, never_set));
  assert(never_set.year == 1900 && never_set.month == 1 && never_set.day == 1);
  assert(!user_birth_date(mw3, 1, full_buffer, never_set));

  // Three bytes that are not a date are refused instead of published. Month 13
  // and day 32 both have to fail, or a garbled read renders as a plausible one.
  std::vector<uint8_t> corrupt = full_buffer;
  corrupt[34 + 1] = 13;
  assert(!user_birth_date(mw3, 2, corrupt, never_set));
  corrupt[34 + 1] = 2;
  corrupt[34 + 2] = 30;  // February never has thirty days
  assert(!user_birth_date(mw3, 2, corrupt, never_set));

  // Nothing to read from is not the same as a date of zero.
  assert(!user_birth_date(mw3, 2, {}, never_set));
  assert(!user_birth_date(mw3, 2, std::span<const uint8_t>(full_buffer).first(35), never_set));
  assert(!user_birth_date(mw3, 3, full_buffer, never_set));
}

// Block geometry is read out of each profile rather than being a constant.
// These are the three outcomes it has to produce.
static void test_user_block_geometry_is_derived_per_profile() {
  OmronSettingsBlock block{};

  // The cuff here: a 24-byte pointer region and 10-byte blocks, so 24 and 34 -
  // the two offsets its own settings dumps were measured at.
  const OmronProfile &measured = get_profile(OmronProfileId::HEM_7155T_MW3);
  assert(user_settings_block(measured, 1, block) && block.offset == 24 && block.size == 10);
  assert(user_settings_block(measured, 2, block) && block.offset == 34 && block.size == 10);

  // The 14-byte family, the largest in the catalog: a 16-byte pointer region
  // puts its blocks at 16 and 30. A constant fixed at 24 and 34 reads the wrong
  // bytes on every variant of it, which is the majority of the catalog, and is
  // why the offset is derived.
  const OmronProfile &wide = get_profile(OmronProfileId::HEM_7155T);
  assert(user_settings_block(wide, 1, block) && block.offset == 16 && block.size == 14);
  assert(user_settings_block(wide, 2, block) && block.offset == 30 && block.size == 14);

  // Six-byte blocks: 8-byte pointer region, so 8 and 14. They hold a birth date
  // and nothing else, so the block is located and the date is read, while the
  // flag and the version counter are refused - and with them any registration,
  // since a registration is a bump of a counter this model has not got.
  const OmronProfile &narrow = get_profile(OmronProfileId::HEM_7322T);
  assert(user_settings_block(narrow, 1, block) && block.offset == 8 && block.size == 6);
  assert(user_settings_block(narrow, 2, block) && block.offset == 14 && block.size == 6);
  bool flag = false;
  std::vector<uint8_t> settings(30, 0x11);
  settings[8] = 66;  // 1900 + 66
  settings[9] = 4;
  settings[10] = 17;
  assert(!user_registered_flag(narrow, 1, settings, flag));
  assert(user_settings_version(narrow, 1, settings) == 0);
  OmronDateTime born{};
  assert(user_birth_date(narrow, 1, settings, born));
  assert(born.year == 1966 && born.month == 4 && born.day == 17);

  // A profile that states no clock region has no place to put the blocks.
  assert(!user_settings_block(get_profile(OmronProfileId::HEM_6401T), 1, block));

  // Where a user's unsent counter sits is the catalog's to say. Every profile
  // the write path accepts today puts user 1 at 4 and user 2 at 6, so a builder
  // computing 4 + 2n is right by accident; the one family that says otherwise,
  // 6401 at offset 14, is only kept out of trouble by an unrelated guard,
  // because it carries no clock region and its writes are refused before the
  // offset is used. Give that family a clock and the clear would land ten bytes
  // early, in somebody else's field.
  //
  // Moving the field on a profile that IS writable is the only way to see the
  // difference, because on every real one the two answers agree.
  OmronProfile relocated = get_profile(OmronProfileId::HEM_7155T_MW3);
  relocated.users[1].unread_counter_offset = 20;
  std::vector<uint8_t> region(60, 0x00);
  region[6] = 0x07;   // where the arithmetic would have written
  region[20] = 0x07;  // where this profile now says user 2's counter lives
  SessionSettingsUpdate moved{};
  moved.user_number = 2;
  std::vector<SettingsWriteFrame> moved_writes;
  assert(build_session_settings_writes(relocated, moved, region, moved_writes) == SettingsWriteError::NONE);
  assert(!moved_writes.empty() && moved_writes[0].address == relocated.settings_write_address);
  assert(moved_writes[0].frame[5] >= 22);
  const uint8_t *moved_pointers = moved_writes[0].frame.data() + 6;
  assert(moved_pointers[20] == 0x00 && moved_pointers[21] == 0x80);
  assert(moved_pointers[6] == 0x07 && moved_pointers[7] == 0x00);

  // And the same for the other user's counter, which a session clears for
  // everybody whose records it read rather than only for its own.
  relocated.users[0].unread_counter_offset = 18;
  region[4] = 0x03;
  region[18] = 0x03;
  moved.collected_users = 0x01;
  assert(build_session_settings_writes(relocated, moved, region, moved_writes) == SettingsWriteError::NONE);
  moved_pointers = moved_writes[0].frame.data() + 6;
  assert(moved_pointers[18] == 0x00 && moved_pointers[19] == 0x80);
  assert(moved_pointers[4] == 0x03 && moved_pointers[5] == 0x00);

  // The shipped profiles still have to give the offsets a real cuff uses, which
  // is why the wrong arithmetic went unnoticed for so long.
  assert(measured.users[0].unread_counter_offset == 4 && measured.users[1].unread_counter_offset == 6);
  assert(get_profile(OmronProfileId::HEM_6401T).users[0].unread_counter_offset == 14);

  // The index cursor is read the way each model sends it. Fourteen profiles swap
  // the index data in pairs, which for a 16-bit cursor is a big-endian read.
  // The cuff here is not one of them and must stay little.
  assert(cursor_memory_order(get_profile(OmronProfileId::HEM_7322T)) == MemoryByteOrder::BIG);
  assert(cursor_memory_order(get_profile(OmronProfileId::HEM_7600T)) == MemoryByteOrder::BIG);
  assert(cursor_memory_order(get_profile(OmronProfileId::HEM_7155T_MW3)) == MemoryByteOrder::LITTLE);
  assert(cursor_memory_order(get_profile(OmronProfileId::HEM_7155T_K4)) == MemoryByteOrder::LITTLE);
}

static void test_hem_6401_family_splits_into_two_profiles() {
  const OmronProfile &small = get_profile(OmronProfileId::HEM_6401T);
  const OmronProfile &large = get_profile(OmronProfileId::HEM_6410T);

  // The blood-pressure area of HEM-6401T-Z, then of HEM-6410T-Z. One entry for
  // both would put HEM-6410T-Z on the first pair of numbers and read 16 bytes
  // out of a 32-byte record.
  assert(small.users[0].record_start_address == 0x1350);
  assert(small.record_size == 16);
  assert(large.users[0].record_start_address == 0x5590);
  assert(large.record_size == 32);
  assert(profile_for_model("HEM-6410T-Z")->id == OmronProfileId::HEM_6410T);
  assert(profile_for_model("HEM-6402T-Z")->id == OmronProfileId::HEM_6401T);
  assert(profile_for_model("HEM-6411T-MAJ")->id == OmronProfileId::HEM_6410T);

  for (const OmronProfile *profile : {&small, &large}) {
    assert(profile->user_count == 1);
    assert(profile->settings_read_address == 0x0100);
    assert(profile->settings_index_region_size == 16);
    // Cursor, unsent counter and ring depth for that area.
    assert(profile->users[0].write_cursor_offset == 6);
    assert(profile->users[0].unread_counter_offset == 14);
    assert(profile->users[0].record_count == 100);
    // Nothing here writes to these devices: the clock is the first settings
    // block rather than the last, which is not the geometry the settings writer
    // was measured against.
    assert(profile->clock_fields_offset == NO_CLOCK);
    assert(profile->record_sequence_offset == NO_RECORD_SEQUENCE);
  }
  assert(small.settings_write_address == 0x0160);
  assert(large.settings_write_address == 0x0170);

  // A record decoded field by field, with the flag pair set the way Omron's
  // 6410T file says means "no value" - a 2, not a 1. Reading these as non-zero
  // would publish a missing measurement as a warning.
  std::vector<uint8_t> record(small.record_size, 0);
  record[0] = 25;
  record[1] = 12;
  record[2] = 31;
  record[3] = 23;
  record[4] = 58;
  record[5] = 59;
  record[6] = 108;  // 108 + 25 = 133 mmHg.
  record[7] = 87;
  record[8] = 71;
  record[11] = static_cast<uint8_t>((2U << 2) | 2U);
  OmronMeasurement measurement;
  assert(parse_measurement_record(record, small, measurement) == MeasurementParseError::NONE);
  assert(measurement.systolic_mm_hg == 133);
  assert(measurement.diastolic_mm_hg == 87);
  assert(measurement.pulse_bpm == 71);
  assert(measurement.timestamp.year == 2025);
  assert(measurement.timestamp.month == 12);
  assert(measurement.timestamp.day == 31);
  assert(measurement.timestamp.hour == 23);
  assert(measurement.timestamp.minute == 58);
  assert(measurement.timestamp.second == 59);
  assert(!measurement.movement_detected);
  assert(!measurement.irregular_heartbeat);
  assert(!measurement.has_record_id);

  // The same 13 bytes in a 32-byte record, which is the whole reason one decoder
  // serves both maps. The tail is left at whatever the longer record carries.
  std::vector<uint8_t> wide(large.record_size, 0);
  for (size_t i = 0; i < 13; i++)
    wide[i] = record[i];
  wide[24] = 0x11;
  wide[28] = 0x22;
  OmronMeasurement from_wide;
  assert(parse_measurement_record(wide, large, from_wide) == MeasurementParseError::NONE);
  assert(from_wide.systolic_mm_hg == 133);
  assert(from_wide.timestamp.day == 31);
  assert(!from_wide.has_record_id);

  // An erased slot and a slot holding only a date: neither is a reading.
  std::vector<uint8_t> empty(small.record_size, 0xFF);
  assert(parse_measurement_record(empty, small, measurement) == MeasurementParseError::EMPTY_SLOT);
  std::vector<uint8_t> date_only(small.record_size, 0);
  date_only[0] = 25;
  date_only[1] = 6;
  date_only[2] = 1;
  assert(parse_measurement_record(date_only, small, measurement) == MeasurementParseError::INVALID_MEASUREMENT);
}

static void test_profile_adapter_and_poll_plan() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  PollLayout layout;
  assert(make_poll_layout(mw3, layout) == ProfileAdapterError::NONE);
  assert(layout.index_address == 0x0260);
  // 24 bytes of pointer region, which is what a settings dump off this cuff
  // shows before the first user block.
  assert(layout.index_size == 24);
  assert(layout.transfer_block_size == 0x38);
  // The default reaches the whole ring: every record the cursor accounts for,
  // even when the cuff reports none outstanding. Sixty slots here, so fifty
  // nine behind the newest.
  assert(layout.backtrack_records == 59);
  assert(mw3.users[0].record_count == 60);
  // Still bounded by what was actually written - the plan trims against the
  // cursor - so this is "do not stop early", not "read sixty slots".
  PollLayout capped;
  assert(make_poll_layout(mw3, capped, 15) == ProfileAdapterError::NONE);
  assert(capped.backtrack_records == 15);
  // And an explicit zero still means the newest reading and nothing else.
  PollLayout newest_only;
  assert(make_poll_layout(mw3, newest_only, 0) == ProfileAdapterError::NONE);
  assert(newest_only.backtrack_records == 0);
  assert(layout.users[0].enabled && layout.users[1].enabled);
  // User 2's cursor is at 0x02. This asserted 8 for as long as the catalog
  // carried the reference's MW3 value, and a real cuff disagreed: a user-2
  // measurement stepped index byte 0x02 from 0x0B to 0x0C while 0x08 stood
  // still, so the poll kept re-reading the same stale slot.
  assert(layout.users[0].cursor_offset == 0 && layout.users[1].cursor_offset == 2);
  assert(layout.users[0].cursor_width == 2 && layout.users[1].cursor_width == 2);
  assert(layout.users[0].cursor_order == MemoryByteOrder::LITTLE);
  // The index pointer defaults to the record order but is a separate key, so a
  // future family with a big-endian cursor over little-endian records can be
  // expressed without touching the adapter.
  OmronProfile split_order = get_profile(OmronProfileId::HEM_7155T_MW3);
  assert(split_order.byte_order == ByteOrder::LITTLE);
  assert(cursor_memory_order(split_order) == MemoryByteOrder::LITTLE);
  split_order.cursor_byte_order = CursorByteOrder::BIG;
  assert(cursor_memory_order(split_order) == MemoryByteOrder::BIG);
  PollLayout split_layout;
  assert(make_poll_layout(split_order, split_layout) == ProfileAdapterError::NONE);
  assert(split_layout.users[0].cursor_order == MemoryByteOrder::BIG);
  assert(layout.users[0].ring.records_address == 0x02E8);
  assert(layout.users[1].ring.records_address == 0x06A8);
  assert(layout.users[0].ring.record_count == 60 && layout.users[0].ring.record_size == 16);
  assert(layout.users[0].ring.cursor_mask == 0x00FF && layout.users[0].ring.cursor_bias == -1);
  assert(layout.users[0].ring.slot_min == 0 && layout.users[0].ring.slot_max == 59);
  assert(!layout.users[2].enabled && !layout.users[3].enabled);

  // The classic-stack variant of the cuff here. Its numbers are pinned to
  // literals rather than left to a self-satisfying invariant, because the label
  // HEM-7155T-ESL maps to this profile or to MW3 depending on firmware, and
  // picking the wrong one silently reads the wrong region.
  const OmronProfile &classic_7155t = get_profile(OmronProfileId::HEM_7155T);
  PollLayout classic_layout;
  assert(make_poll_layout(classic_7155t, classic_layout) == ProfileAdapterError::NONE);
  assert(classic_layout.index_address == 0x0010);
  assert(classic_layout.users[0].ring.records_address == 0x0098);
  assert(classic_layout.users[1].ring.records_address == 0x0458);
  assert(classic_layout.users[0].ring.record_count == 60 && classic_layout.users[1].ring.record_count == 60);
  assert(classic_layout.users[0].ring.record_size == 16);
  assert(classic_layout.users[0].cursor_order == MemoryByteOrder::LITTLE);

  PollLayout unused;
  assert(make_poll_layout(get_profile(OmronProfileId::UNSUPPORTED), unused) ==
         ProfileAdapterError::UNSUPPORTED_PROFILE);
  OmronProfile invalid = mw3;
  invalid.user_count = 0;
  assert(make_poll_layout(invalid, unused) == ProfileAdapterError::INVALID_USER_COUNT);
  invalid = mw3;
  invalid.user_count = 3;
  assert(make_poll_layout(invalid, unused) == ProfileAdapterError::INVALID_USER_COUNT);
  invalid = mw3;
  invalid.settings_index_region_size = 0;
  assert(make_poll_layout(invalid, unused) == ProfileAdapterError::INVALID_INDEX_SIZE);
  invalid = mw3;
  invalid.transmission_block_size = 0;
  assert(make_poll_layout(invalid, unused) == ProfileAdapterError::INVALID_BLOCK_SIZE);
  invalid = mw3;
  invalid.record_size = 0;
  assert(make_poll_layout(invalid, unused) == ProfileAdapterError::INVALID_RECORD_LAYOUT);
  invalid = mw3;
  invalid.users[0].record_count = 0;
  assert(make_poll_layout(invalid, unused) == ProfileAdapterError::INVALID_RECORD_LAYOUT);

  expect_string(profile_adapter_error_to_string(ProfileAdapterError::NONE), "none");
  expect_string(profile_adapter_error_to_string(ProfileAdapterError::INVALID_RECORD_LAYOUT), "invalid record layout");
  expect_string(profile_adapter_error_to_string(static_cast<ProfileAdapterError>(0xFF)), "unknown");

  ReadRange index_range;
  assert(build_index_read(layout, index_range));
  assert(index_range.address == layout.index_address && index_range.length == layout.index_size &&
         index_range.block_size == layout.transfer_block_size);
  PollLayout invalid_index = layout;
  invalid_index.index_size = 0;
  assert(!build_index_read(invalid_index, index_range));
  invalid_index = layout;
  invalid_index.transfer_block_size = 0;
  assert(!build_index_read(invalid_index, index_range));

  layout.backtrack_records = 2;
  std::vector<uint8_t> index_data(layout.index_size, 0);
  write_u16_le(index_data, layout.users[0].cursor_offset, 1);
  write_u16_le(index_data, layout.users[1].cursor_offset, 0);
  std::vector<UserRecordPlan> plans;
  assert(build_record_plan(layout, index_data, plans));
  assert(plans.size() == 2);
  assert(plans[0].user == 0 && plans[0].raw_cursor == 1);
  // One record written, so one slot to read. This asserted {0, 59, 58} until a
  // cuff was asked for exactly that: slots 59 and 58 had never been written,
  // the read came back as a header with no payload, and the session died on a
  // length mismatch after every real record had already been fetched.
  assert((plans[0].slots == std::vector<uint16_t>{0}));
  uint16_t slot_zero_address = 0;
  assert(record_address(layout.users[0].ring, 0, slot_zero_address));
  assert(plans[0].reads.size() == 1);
  assert(plans[0].reads[0].address == slot_zero_address && plans[0].reads[0].length == 16);
  // A cursor of zero says nothing about how many records exist, so the depth
  // stands and the wrapped slots are still read. Two reads: the newest slot
  // alone first, then the remaining two coalesced. One 48-byte read of all three
  // in address order would put the newest reading last on the wire.
  assert(plans[1].user == 1 && plans[1].raw_cursor == 0);
  assert((plans[1].slots == std::vector<uint16_t>{59, 58, 57}));
  assert(plans[1].reads.size() == 2);
  uint16_t newest_address = 0;
  uint16_t rest_address = 0;
  assert(record_address(layout.users[1].ring, 59, newest_address));
  assert(record_address(layout.users[1].ring, 57, rest_address));
  assert(plans[1].reads[0].address == newest_address && plans[1].reads[0].length == 16);
  assert(plans[1].reads[1].address == rest_address && plans[1].reads[1].length == 32);

  // The real index block, captured during the first session
  // that authenticated and read records: nine stored for user 1, thirteen for
  // user 2. Asking for more history than that must not walk off the end of what
  // has been written, because the cuff answers a read of virgin slots with a
  // header and no payload, and the transaction can only see a length mismatch.
  // That aborted a session which had already fetched every real record.
  PollLayout history_plan = layout;
  history_plan.backtrack_records = 15;
  // Sixteen bytes as the log recorded them, then eight of padding: the pointer
  // region is 24 bytes on this variant, its own file says so and so does the
  // settings dump, but only the first sixteen were ever printed. Nothing the
  // plan reads lives past offset 7, so the padding changes no expectation here -
  // it exists because the builder refuses a buffer shorter than the region.
  std::vector<uint8_t> captured_index{0x09, 0x80, 0x0D, 0x00, 0x09, 0x80, 0x01, 0x00,
                                      0x09, 0x00, 0x00, 0x80, 0x0D, 0x00, 0x00, 0x00};
  captured_index.resize(history_plan.index_size, 0x00);
  assert(history_plan.index_size == 24);
  assert(build_record_plan(history_plan, captured_index, plans));
  assert(plans[0].slots.size() == 9);
  assert(plans[0].slots.front() == 8 && plans[0].slots.back() == 0);
  assert(plans[1].slots.size() == 13);
  assert(plans[1].slots.front() == 12 && plans[1].slots.back() == 0);
  // Reads stay inside the written region: user 1 ends at slot 8, whose address
  // is well below the 0x0638 that the cuff refused to serve.
  for (const ReadBlock &block : plans[0].reads)
    assert(block.address + block.length <= 0x02E8 + 9 * 16);

  // A ring that has wrapped keeps the full depth: the cursor no longer counts
  // records, every slot holds one, and the request is capped by the ring size.
  PollLayout wrapped_plan = layout;
  wrapped_plan.backtrack_records = 15;
  std::vector<uint8_t> wrapped(wrapped_plan.index_size, 0);
  write_u16_le(wrapped, wrapped_plan.users[0].cursor_offset, 60);
  write_u16_le(wrapped, wrapped_plan.users[1].cursor_offset, 60);
  assert(build_record_plan(wrapped_plan, wrapped, plans));
  assert(plans[0].slots.size() == 16);
  assert(plans[0].slots.front() == 59 && plans[0].slots.back() == 44);

  assert(!build_record_plan(layout, {}, plans));
  assert(!build_record_plan(layout, std::span<const uint8_t>(index_data).first(index_data.size() - 1), plans));
  PollLayout invalid_cursor = layout;
  invalid_cursor.users[1].cursor_offset = static_cast<uint8_t>(invalid_cursor.index_size - 1);
  assert(!build_record_plan(invalid_cursor, index_data, plans));
  PollLayout no_users{};
  no_users.index_size = 1;
  no_users.transfer_block_size = 1;
  std::array<uint8_t, 1> one_index{};
  assert(!build_record_plan(no_users, one_index, plans));

  // Synthetic, because no catalogued profile is big-endian any more. The
  // big-endian cursor path still exists in the adapter, so it is still
  // exercised - just not by pretending a real model uses it.
  OmronProfile big_profile = get_profile(OmronProfileId::HEM_6161T);
  big_profile.byte_order = ByteOrder::BIG;
  PollLayout big_layout;
  assert(make_poll_layout(big_profile, big_layout) == ProfileAdapterError::NONE);
  assert(big_layout.users[0].cursor_order == MemoryByteOrder::BIG);
  std::vector<uint8_t> big_index(big_layout.index_size, 0);
  write_u16_be(big_index, big_layout.users[0].cursor_offset, 1);
  assert(build_record_plan(big_layout, big_index, plans));
  assert(plans.size() == 1 && plans[0].slots.size() == 1 && plans[0].slots[0] == 0);
}

static void test_record_store() {
  OmronRecordStore store;
  assert(store.is_new(0, 0x12345678));
  assert(store.accept(0, 0x12345678));
  assert(!store.is_new(0, 0x12345678));
  assert(!store.accept(0, 0x12345678));
  assert(store.accept(0, 0x87654321));
  assert(!store.is_new(0, 0x87654321));
  // Per user, so one person measuring twice does not suppress the other's
  // unchanged latest reading.
  const uint8_t last_user = OmronRecordStore::MAX_USERS - 1;
  assert(store.is_new(last_user, 0x87654321));
  // The store holds exactly as many slots as the catalog can address.
  static_assert(OmronRecordStore::MAX_USERS == OMRON_MAX_USERS, "record store must cover every addressable user");
  // An out-of-range user is not "already seen". Reporting it as a duplicate
  // would swallow the indexing bug; accept() still refuses to store it.
  assert(store.is_new(OmronRecordStore::MAX_USERS, 1));
  assert(!store.accept(OmronRecordStore::MAX_USERS, 1));
  assert(store.is_new(OmronRecordStore::MAX_USERS, 1));
}

static void test_unlock_frames_and_replies() {
  OmronBindKey key{};
  for (size_t index = 0; index < key.size(); index++)
    key[index] = static_cast<uint8_t>(0xA0 + index);
  const auto auth = make_key_auth_request(key);
  assert(auth[0] == 0x01);
  assert(std::equal(key.begin(), key.end(), auth.begin() + 1));
  const auto confirm = make_confirm_encryption_request();
  assert(confirm[0] == 0x02);
  assert(std::all_of(confirm.begin() + 1, confirm.end(), [](uint8_t value) { return value == 0; }));
  const auto program = make_program_key_request(key);
  assert(program[0] == 0x00);
  assert(std::equal(key.begin(), key.end(), program.begin() + 1));

  assert(classify_unlock_reply({}) == UnlockReply::INVALID);
  const std::array<uint8_t, 2> programmed{0x80, 0x00};
  const std::array<uint8_t, 2> accepted{0x81, 0x00};
  const std::array<uint8_t, 2> ready{0x82, 0x00};
  const std::array<uint8_t, 2> token{0x91, 0x00};
  const std::array<uint8_t, 2> token_failed{0x91, 0x01};
  assert(classify_unlock_reply(programmed) == UnlockReply::KEY_PROGRAMMED);
  assert(classify_unlock_reply(accepted) == UnlockReply::KEY_ACCEPTED);
  assert(classify_unlock_reply(ready) == UnlockReply::ENCRYPTION_CONFIRMED);
  assert(classify_unlock_reply(token) == UnlockReply::TOKEN_ACCEPTED);
  assert(classify_unlock_reply(std::span<const uint8_t>(token).first(1)) == UnlockReply::INVALID);
  assert(classify_unlock_reply(token_failed) == UnlockReply::INVALID);

  // Byte 1 carries the outcome. Reading only byte 0 reported a rejected key as
  // success and the session then died several frames later as an unexpected
  // command, which points the diagnosis at bonding instead of the key.
  const std::array<uint8_t, 2> key_mismatch{0x81, 0x01};
  const std::array<uint8_t, 2> precondition{0x81, 0x04};
  assert(classify_unlock_reply(key_mismatch) == UnlockReply::KEY_REJECTED);
  assert(classify_unlock_reply(precondition) == UnlockReply::KEY_REJECTED);
  // A one-byte reply carries no status, so the opcode echo stands on its own.
  const std::array<uint8_t, 1> bare_accept{0x81};
  assert(classify_unlock_reply(bare_accept) == UnlockReply::KEY_ACCEPTED);
  expect_string(unlock_status_to_string(0x00), "accepted");
  expect_string(unlock_status_to_string(0x01), "bind key does not match the one stored on the device");
  expect_string(unlock_status_to_string(0x04), "security precondition not met");
  expect_string(unlock_status_to_string(0x77), "unknown status");

  // The 0x82 reply is the cuff's verdict on link encryption, not on a key, and
  // a bad verdict does not end the session: log the code and carry on to
  // authenticate. Classifying it as a rejection stalls every session that did
  // not begin in pairing mode.
  const std::array<uint8_t, 2> encryption_timeout{0x82, 0x0F};
  const std::array<uint8_t, 2> encryption_lost{0x82, 0x08};
  assert(classify_unlock_reply(encryption_timeout) == UnlockReply::ENCRYPTION_CONFIRMED);
  assert(classify_unlock_reply(encryption_lost) == UnlockReply::ENCRYPTION_CONFIRMED);
  expect_string(encryption_status_to_string(0x00), "up");
  expect_string(encryption_status_to_string(0x01), "cancelled");
  expect_string(encryption_status_to_string(0x08), "peer lost its pairing information");
  expect_string(encryption_status_to_string(0x17), "peer lost its pairing information");
  expect_string(encryption_status_to_string(0x0F), "timed out");
  expect_string(encryption_status_to_string(0x55), "failed");
}

// Bytes this suite did not produce.
//
// Every other fixture here is built by the test out of the same helpers the
// implementation uses: make_response builds frames with the same xor_bytes that
// later checks them. Those tests hold the code to its own arithmetic, so a
// mutated field offset can pass them green, and nothing but captured traffic
// closes that.
//
// These frames are captured traffic: notifications logged verbatim off the
// HEM-7155T-MW3 in this room, and the values asserted are the ones Home
// Assistant published in that same session. Change an offset in the decoder and
// these fail, whatever the builders do.
static void test_captured_frames_from_the_cuff() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);

  // Read response for 0x0348, three sixteen-byte records of user 1. The third
  // is the newest at that point, and it is the reading the entity showed:
  // 125/82/78 stamped 2019-01-01T00:00:59, which is a measurement taken before
  // the cuff's clock was ever set.
  const std::vector<uint8_t> user1_frame = {
      0x38, 0x81, 0x00, 0x03, 0x48, 0x30, 0x66, 0x53, 0x48, 0x13, 0x20, 0x04, 0x3F, 0x10, 0x00, 0x00, 0x07, 0x00, 0x00,
      0x01, 0x8F, 0x00, 0x66, 0x56, 0x52, 0x13, 0x20, 0x04, 0x3F, 0x10, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x9D, 0x00,
      0x64, 0x52, 0x4E, 0x13, 0x20, 0x04, 0x3F, 0x10, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01, 0x94, 0x00, 0x00, 0x3C};
  assert(xor_bytes(user1_frame) == 0);

  ResponseFrame response{};
  assert(parse_response(user1_frame, response) == ProtocolError::NONE);
  assert(response.type == PacketType::READ_RESPONSE);
  assert(response.address == 0x0348);
  assert(response.data.size() == 0x30);

  OmronMeasurement newest{};
  assert(parse_measurement_record(std::span<const uint8_t>(response.data).subspan(32, mw3.record_size), mw3, newest) ==
         MeasurementParseError::NONE);
  assert(newest.systolic_mm_hg == 125 && newest.diastolic_mm_hg == 82 && newest.pulse_bpm == 78);
  assert(newest.timestamp.year == 2019 && newest.timestamp.month == 1 && newest.timestamp.day == 1);
  assert(newest.timestamp.hour == 0 && newest.timestamp.minute == 0 && newest.timestamp.second == 59);

  // The two records ahead of it in the same frame, decoded from the same bytes.
  OmronMeasurement older{};
  assert(parse_measurement_record(std::span<const uint8_t>(response.data).first(mw3.record_size), mw3, older) ==
         MeasurementParseError::NONE);
  assert(older.systolic_mm_hg == 127 && older.diastolic_mm_hg == 83 && older.pulse_bpm == 72);

  // User 2's newest, out of the read at 0x0768: 115/76/86 at 2026-08-04
  // 22:21:59, which is a stamp from after the clock was set and therefore
  // exercises a different year than the record above.
  const std::vector<uint8_t> user2_frame = {0x18, 0x81, 0x00, 0x07, 0x68, 0x10, 0x5A, 0x4C, 0x56, 0x1A, 0x96, 0x20,
                                            0x7B, 0x15, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x02, 0x6B, 0x00, 0x00, 0x00};
  assert(xor_bytes(user2_frame) == 0);
  assert(parse_response(user2_frame, response) == ProtocolError::NONE);
  assert(response.address == 0x0768);

  OmronMeasurement user2{};
  assert(parse_measurement_record(std::span<const uint8_t>(response.data).first(mw3.record_size), mw3, user2) ==
         MeasurementParseError::NONE);
  assert(user2.systolic_mm_hg == 115 && user2.diastolic_mm_hg == 76 && user2.pulse_bpm == 86);
  assert(user2.timestamp.year == 2026 && user2.timestamp.month == 8 && user2.timestamp.day == 4);
  assert(user2.timestamp.hour == 22 && user2.timestamp.minute == 21 && user2.timestamp.second == 59);

  // The read at 0x0768 from the session that produced 130/91/102.
  // This frame is here for the record tail: the three records are user 2's
  // thirteenth, fourteenth and fifteenth, and their numbers sit at offset 10.
  // Byte 14 is the checksum over the bytes before it, and reading that as the
  // number is the bug this frame pins - it would report 107, 226 and 77.
  const std::vector<uint8_t> user2_tail_frame = {
      0x38, 0x81, 0x00, 0x07, 0x68, 0x30, 0x5A, 0x4C, 0x56, 0x1A, 0x96, 0x20, 0x7B, 0x15, 0x00, 0x00, 0x0D, 0x00, 0x00,
      0x02, 0x6B, 0x00, 0x62, 0x51, 0x6A, 0x1A, 0x0F, 0x21, 0x54, 0x17, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x02, 0xE2, 0x00,
      0x69, 0x5B, 0x66, 0x1A, 0x2C, 0x21, 0x90, 0x1B, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x02, 0x4D, 0x00, 0x00, 0x48};
  assert(xor_bytes(user2_tail_frame) == 0);
  assert(parse_response(user2_tail_frame, response) == ProtocolError::NONE);
  assert(response.address == 0x0768 && response.data.size() == 0x30);

  const uint16_t expected_numbers[3] = {13, 14, 15};
  const uint8_t expected_checksums[3] = {0x6B, 0xE2, 0x4D};
  for (size_t slot = 0; slot < 3; slot++) {
    const std::span<const uint8_t> record =
        std::span<const uint8_t>(response.data).subspan(slot * mw3.record_size, mw3.record_size);
    OmronMeasurement decoded{};
    assert(parse_measurement_record(record, mw3, decoded) == MeasurementParseError::NONE);
    assert(decoded.has_record_id);
    assert(decoded.record_id == expected_numbers[slot]);
    // Independently: byte 14 really is the sum of everything before it, so the
    // old reading was not an off-by-one but a different field entirely.
    uint32_t sum = 0;
    for (size_t index = 0; index + 2 < mw3.record_size; index++)
      sum += record[index];
    assert(static_cast<uint8_t>(sum & 0xFF) == expected_checksums[slot]);
    assert(record[14] == expected_checksums[slot]);
    // And byte 13 carries the user this area belongs to.
    assert(record[13] == 2);
  }

  // The newest of the three is the reading Home Assistant published that day.
  OmronMeasurement latest{};
  assert(parse_measurement_record(std::span<const uint8_t>(response.data).subspan(2 * mw3.record_size, mw3.record_size),
                                  mw3, latest) == MeasurementParseError::NONE);
  assert(latest.systolic_mm_hg == 130 && latest.diastolic_mm_hg == 91 && latest.pulse_bpm == 102);
  assert(latest.timestamp.year == 2026 && latest.timestamp.month == 8 && latest.timestamp.day == 9);
  assert(latest.timestamp.hour == 12 && latest.timestamp.minute == 46 && latest.timestamp.second == 16);
  assert(latest.cuff_flag && !latest.movement_detected && !latest.irregular_heartbeat);

  // The clock window, read at 0x028C in the same session and published as
  // 2026-08-08T00:17:43.
  const std::vector<uint8_t> clock_frame = {0x18, 0x81, 0x00, 0x02, 0x8C, 0x10, 0xC8, 0xA8, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x1A, 0x08, 0x08, 0x00, 0x11, 0x2B, 0xD6, 0x00, 0x00, 0x91};
  assert(xor_bytes(clock_frame) == 0);
  assert(parse_response(clock_frame, response) == ProtocolError::NONE);
  OmronDateTime clock{};
  assert(parse_device_clock(response.data, mw3.clock_fields_offset, clock) == ClockParseError::NONE);
  assert(clock.year == 2026 && clock.month == 8 && clock.day == 8);
  assert(clock.hour == 0 && clock.minute == 17 && clock.second == 43);

  // The handshake, byte for byte as the cuff answered it. Our own request for
  // the start opcode is in the log immediately above each of these.
  const std::vector<uint8_t> start_reply = {0x08, 0x80, 0x00, 0x00, 0x00, 0x10, 0x00, 0x98};
  const std::vector<uint8_t> end_reply = {0x08, 0x8F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87};
  assert(parse_response(start_reply, response) == ProtocolError::NONE);
  assert(response.type == PacketType::START_RESPONSE);
  assert(parse_response(end_reply, response) == ProtocolError::NONE);
  assert(response.type == PacketType::END_RESPONSE);
  assert(response.status == 0x00);

  // A settings region as it read before this component ever wrote a birth date
  // into it. Both user blocks are at
  // their factory 1900-01-01, which is what keeps a real date out of the repo.
  const std::vector<uint8_t> settings = {0x09, 0x80, 0x0D, 0x00, 0x00, 0x80, 0x00, 0x80, 0x09, 0x00, 0x00,
                                         0x80, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
                                         0x00, 0x00, 0x01, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0B, 0x00};
  OmronDateTime born{};
  assert(user_birth_date(mw3, 1, settings, born));
  assert(born.year == 1900 && born.month == 1 && born.day == 1);
  assert(user_settings_version(mw3, 1, settings) == 1);
  assert(user_settings_version(mw3, 2, settings) == 9);
  bool registered = true;
  assert(user_registered_flag(mw3, 2, settings, registered));
  assert(!registered);  // the per-user bit reads zero even for a registered user

  // The advertisement that woke that session, manufacturer data as logged. Its
  // two sequence numbers are the same nine and thirteen the index block gives
  // above, which is what lets a host tell who has a new reading without
  // connecting to anything.
  const std::vector<uint8_t> advertisement = {0x01, 0x01, 0x09, 0x00, 0x09, 0x0D, 0x00, 0x0D};
  OmronAdvertisementFlags flags{};
  assert(parse_advertisement_flags(advertisement, flags));
  assert(flags.format == 0x01);
  assert(flags.sequence_count == 2);
  assert(flags.user_sequence[0] == 9 && flags.user_sequence[1] == 13);
  assert(!flags.pairing_mode && !flags.wants_session());
}

static void test_gatt_constants() {
  // Every UUID below is written out in full rather than read back from the
  // tables under test, so a typo in any channel fails here. These are the
  // tables the client actually dials; there is deliberately no second copy to
  // drift against.
  const OmronGattCapabilities &classic = OMRON_CLASSIC_GATT;
  const OmronGattCapabilities &modern = OMRON_MODERN_GATT;

  expect_string(classic.parent_service_uuid, "ecbe3980-c9a2-11e1-b1bd-0002a5d5c51b");
  assert(classic.rx_channel_count == 4);
  assert(classic.tx_channel_count == 4);
  expect_string(classic.rx_channel_uuids[0], "49123040-aee8-11e1-a74d-0002a5d5c51b");
  expect_string(classic.rx_channel_uuids[1], "4d0bf320-aee8-11e1-a0d9-0002a5d5c51b");
  expect_string(classic.rx_channel_uuids[2], "5128ce60-aee8-11e1-b84b-0002a5d5c51b");
  expect_string(classic.rx_channel_uuids[3], "560f1420-aee8-11e1-8184-0002a5d5c51b");
  expect_string(classic.tx_channel_uuids[0], "db5b55e0-aee7-11e1-965e-0002a5d5c51b");
  expect_string(classic.tx_channel_uuids[1], "e0b8a060-aee7-11e1-92f4-0002a5d5c51b");
  expect_string(classic.tx_channel_uuids[2], "0ae12b00-aee8-11e1-a192-0002a5d5c51b");
  expect_string(classic.tx_channel_uuids[3], "10e1ba60-aee8-11e1-89e5-0002a5d5c51b");
  expect_string(classic.unlock_characteristic_uuid, "b305b680-aee7-11e1-a730-0002a5d5c51b");

  expect_string(modern.parent_service_uuid, "0000fe4a-0000-1000-8000-00805f9b34fb");
  assert(modern.rx_channel_count == 1);
  assert(modern.tx_channel_count == 1);
  expect_string(modern.rx_channel_uuids[0], "49123040-aee8-11e1-a74d-0002a5d5c51b");
  expect_string(modern.tx_channel_uuids[0], "db5b55e0-aee7-11e1-965e-0002a5d5c51b");
  // The modern stack legitimately reuses the classic unlock characteristic.
  expect_string(modern.unlock_characteristic_uuid, "b305b680-aee7-11e1-a730-0002a5d5c51b");
  for (size_t channel = 1; channel < modern.rx_channel_uuids.size(); channel++) {
    assert(modern.rx_channel_uuids[channel] == nullptr);
    assert(modern.tx_channel_uuids[channel] == nullptr);
  }
}

static void test_diagnostics() {
  OmronDiagnostics diagnostics;
  assert(diagnostics.phase == SessionPhase::IDLE);
  diagnostics.last_protocol_error = ProtocolError::CHECKSUM_MISMATCH;
  diagnostics.begin_session(std::numeric_limits<uint32_t>::max() - 5U);
  assert(diagnostics.phase == SessionPhase::CONNECTING);
  assert(diagnostics.connection_attempts == 1);
  assert(diagnostics.last_protocol_error == ProtocolError::NONE);
  diagnostics.finish_session(3, true);
  assert(diagnostics.last_poll_duration_ms == 9);
  assert(diagnostics.successful_polls == 1);
  assert(diagnostics.phase == SessionPhase::COMPLETE);
  diagnostics.begin_session(100);
  diagnostics.finish_session(125, false);
  assert(diagnostics.last_poll_duration_ms == 25);
  assert(diagnostics.successful_polls == 1);
  assert(diagnostics.phase == SessionPhase::ERROR);
}

static void test_measurement_fields_and_bond_policy() {
  // Every model reports movement and irregular, so any profile that decodes a
  // record must claim both. One that claimed neither would publish two entities
  // that can never be true.
  size_t decoding = 0;
  for (size_t index = 0; index < profile_count(); index++) {
    const OmronProfile *profile = profile_at(index);
    assert(profile != nullptr);
    if (profile->record_format == RecordFormat::UNSUPPORTED) {
      assert(profile->measurement_fields == 0);
      continue;
    }
    decoding++;
    assert((profile->measurement_fields & MEASUREMENT_FIELD_MOVEMENT) != 0);
    assert((profile->measurement_fields & MEASUREMENT_FIELD_IRREGULAR) != 0);
    // Nothing may claim a bit outside the set.
    assert((profile->measurement_fields & ~MEASUREMENT_FIELDS_ALL) == 0);
    // No profile may ship PER_SESSION: these cuffs keep a bond, and the claim
    // that this family does not is unsourced.
    assert(profile->bond_policy != BondPolicy::PER_SESSION);
  }
  assert(decoding > 30);

  // The six families whose own files describe less than the rest. Each is one
  // entity that would read a permanent OFF instead of staying unavailable.
  struct Absent {
    OmronProfileId id;
    MeasurementFields missing;
  };
  const std::array<Absent, 6> ABSENT = {{
      {OmronProfileId::HEM_6161T, MEASUREMENT_FIELD_ARTIFACT},
      {OmronProfileId::HEM_6320T, MEASUREMENT_FIELD_CONSECUTIVE},
      {OmronProfileId::HEM_6321T, MEASUREMENT_FIELD_CONSECUTIVE},
      {OmronProfileId::HEM_6401T, MEASUREMENT_FIELD_CUFF | MEASUREMENT_FIELD_CONSECUTIVE},
      {OmronProfileId::HEM_6410T, MEASUREMENT_FIELD_CUFF | MEASUREMENT_FIELD_CONSECUTIVE},
      {OmronProfileId::HEM_7151T, MEASUREMENT_FIELD_IHB},
  }};
  for (const Absent &entry : ABSENT) {
    const OmronProfile &profile = get_profile(entry.id);
    assert((profile.measurement_fields & entry.missing) == 0);
    // And nothing else went missing with it.
    assert(profile.measurement_fields == (MEASUREMENT_FIELDS_ALL & ~entry.missing));
  }

  // The cuff in this room describes every one of them, which is why its entities
  // are all live. HEM-7149T2-E is the trap alongside it: it describes the cuff
  // bit under the second of two interchangeable names, so it reports cuff fit
  // like everything else and must not be read as lacking the field.
  assert(get_profile(OmronProfileId::HEM_7155T_MW3).measurement_fields == MEASUREMENT_FIELDS_ALL);
  const OmronProfile *fits = profile_for_model("HEM-7149T2-E");
  assert(fits != nullptr && (fits->measurement_fields & MEASUREMENT_FIELD_CUFF) != 0);
}

// The harvest and session groups live in test_session.cpp.

// Counted rather than announced up front: a binary that aborts halfway prints
// nothing, and a binary that silently ran no groups at all cannot be mistaken
// for a full pass.
static int run_group(void (*group)()) {
  group();
  return 1;
}

int main() {
  // The component logs through the same macros here as on the device, into a
  // sink that prints nothing unless it is asked to. This is the only caller of
  // that switch.
  host_log_enable(std::getenv("OMRON_TEST_LOG") != nullptr);
  int groups = 0;
  groups += run_group(test_protocol_requests_and_parsing);
  groups += run_group(test_protocol_assembler);
  groups += run_group(test_transaction_engine);
  groups += run_group(test_connection_gate_permutations);
  groups += run_group(test_metrics_and_boundaries);
  groups += run_group(test_memory_image_and_integers);
  groups += run_group(test_ring_and_fingerprint);
  groups += run_group(test_scheduler_press_after_session_tail);
  groups += run_group(test_scheduler_every_burst_after_silence_earns_a_session);
  groups += run_group(test_scheduler_wrap_and_backoff);
  groups += run_group(test_scheduler_new_burst_beats_the_interval);
  groups += run_group(test_standard_bp_and_sfloat);
  groups += run_group(test_profiles_and_aliases);
  groups += run_group(test_measurement_decoders);
  groups += run_group(test_user_block_geometry_is_derived_per_profile);
  groups += run_group(test_hem_6401_family_splits_into_two_profiles);
  groups += run_group(test_profile_adapter_and_poll_plan);
  groups += run_group(test_device_clock_decoder);
  groups += run_group(test_clock_write_builder);
  groups += run_group(test_transaction_carries_two_writes_before_end);
  groups += run_group(test_write_frame_is_built_when_it_is_sent);
  groups += run_group(test_user_block_geometry_against_captured_settings);
  groups += run_group(test_advertisement_flags);
  groups += run_group(test_record_store);
  groups += run_group(test_unlock_frames_and_replies);
  groups += run_group(test_captured_frames_from_the_cuff);
  // Defined in test_session.cpp; declared in test_support.h.
  groups += run_group(test_session_replays_the_captured_frame_order);
  groups += run_group(test_bond_cleanup_never_blocks_after_it_gives_up);
  groups += run_group(test_bond_cleanup_waits_for_the_record_to_actually_go);
  groups += run_group(test_bond_cleanup_clock_starts_at_the_first_tick);
  groups += run_group(test_bond_cleanup_survives_a_list_that_will_not_answer);
  groups += run_group(test_datetime_formatting);
  groups += run_group(test_model_id_does_not_cry_wolf_on_the_verified_cuff);
  groups += run_group(test_model_id_catches_the_wrong_half_of_a_shared_trade_name);
  groups += run_group(test_model_id_refuses_to_guess);
  groups += run_group(test_model_id_prefers_a_model_id_to_a_trade_name);
  groups += run_group(test_model_id_tolerates_what_comes_off_the_wire);
  groups += run_group(test_model_id_table_is_wired_to_the_catalog);
  groups += run_group(test_model_id_map_comparison_looks_at_every_field);
  groups += run_group(test_model_id_config_key_is_derived_not_listed);
  groups += run_group(test_model_id_strings);
  groups += run_group(test_subscriptions_retry_the_cccd_that_never_arrives);
  groups += run_group(test_subscriptions_give_up_on_an_optional_target_and_carry_on);
  groups += run_group(test_subscriptions_park_until_the_link_is_encrypted);
  groups += run_group(test_subscriptions_ignore_events_meant_for_something_else);
  groups += run_group(test_subscriptions_edge_cases);
  groups += run_group(test_history_queue_saves_the_watermark_only_once_it_is_earned);
  groups += run_group(test_history_queue_paces_and_bounds_itself);
  groups += run_group(test_history_queue_watermark_only_moves_forward);
  groups += run_group(test_history_queue_edge_cases);
  groups += run_group(test_publish_capabilities_follow_the_catalog);
  groups += run_group(test_publish_record_marks_only_what_a_record_carries);
  groups += run_group(test_publish_standard_notification_must_name_its_owner);
  groups += run_group(test_publish_standard_notification_ranges_and_status);
  groups += run_group(test_publish_poll_outcome_names_the_right_cause);
  groups += run_group(test_publish_settings_entities);
  groups += run_group(test_publish_settings_where_the_block_keeps_no_counter);
  groups += run_group(test_command_writer_single_channel_sends_everything_at_once);
  groups += run_group(test_command_writer_splits_across_four_channels);
  groups += run_group(test_command_writer_refuses_a_command_with_no_channel_left);
  groups += run_group(test_command_writer_edge_cases);
  groups += run_group(test_session_ignores_a_stray_frame_without_resending);
  groups += run_group(test_session_with_unmoved_cursors_reads_only_two_frames);
  groups += run_group(test_session_full_read_on_pairing_needs_both_the_option_and_the_flag);
  groups += run_group(test_session_registration_writes_reach_the_wire);
  groups += run_group(test_session_survives_the_reply_racing_the_write_ack);
  groups += run_group(test_session_wire_state_guards);
  groups += run_group(test_session_fails_when_the_link_refuses_the_write);
  groups += run_group(test_session_pairing_programs_the_key_before_it_reads);
  groups += run_group(test_session_classic_handshake_retries_on_its_own_clock);
  groups += run_group(test_session_writes_a_birth_date_without_registering);
  groups += run_group(test_measurement_fields_and_bond_policy);
  groups += run_group(test_harvest_picks_the_reading_the_entities_show);
  groups += run_group(test_harvest_prefers_the_cursor_over_the_clock);
  groups += run_group(test_harvest_cutoff_watermark_and_budget);
  groups += run_group(test_gatt_constants);
  groups += run_group(test_diagnostics);
  std::printf("ok: %d test groups passed\n", groups);
  return 0;
}
