// A whole session, driven against a cuff that answers, and the layer that
// decides which reading a person sees.
//
// Every group in test_omron_protocol.cpp exercises one function, and a whole
// class of fault survives that: the clock read landing after the records
// instead of behind the settings frame, or a gate that leaves a button press
// doing nothing. Neither is a wrong answer from any function - both are a wrong
// *sequence*, and a sequence has to be tested at the level where it exists.
//
// The frames and records asserted here were read off a real cuff, not built by
// the same code they check.

#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "omron_harvest.h"
#include "omron_measurement.h"
#include "omron_memory.h"
#include "omron_poll_plan.h"
#include "omron_profile_adapter.h"
#include "omron_profiles.h"
#include "omron_protocol.h"
#include "omron_session.h"
#include "omron_unlock.h"
#include "test_support.h"

using namespace esphome::omron;

namespace {

// The settings region as the cuff read it, byte for byte, with both user blocks
// at their factory 1900-01-01. Cursors are nine and thirteen, matching the
// advertisement of the same session.
const std::vector<uint8_t> CAPTURED_SETTINGS = {0x09, 0x80, 0x0D, 0x00, 0x00, 0x80, 0x00, 0x80, 0x09, 0x00, 0x00,
                                                0x80, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
                                                0x00, 0x00, 0x01, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0B, 0x00};

// The sixteen bytes at 0x028C from the same session; the clock reads
// 2026-08-08T00:17:43.
const std::vector<uint8_t> CAPTURED_CLOCK = {0xC8, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x1A, 0x08, 0x08, 0x00, 0x11, 0x2B, 0xD6, 0x00};

// Golden request frames, transcribed from the captures rather than rebuilt with
// the builders under test.
const std::vector<uint8_t> START_FRAME = {0x08, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x18};
const std::vector<uint8_t> SETTINGS_READ_FRAME = {0x08, 0x01, 0x00, 0x02, 0x60, 0x2C, 0x00, 0x47};
const std::vector<uint8_t> CLOCK_READ_FRAME = {0x08, 0x01, 0x00, 0x02, 0x8C, 0x10, 0x00, 0x97};
const std::vector<uint8_t> END_FRAME = {0x08, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};

// Read at 0x0768: user 2's records thirteen, fourteen and fifteen. Payload
// only, header stripped.
const std::vector<uint8_t> USER2_RECORDS = {0x5A, 0x4C, 0x56, 0x1A, 0x96, 0x20, 0x7B, 0x15, 0x00, 0x00, 0x0D, 0x00,
                                            0x00, 0x02, 0x6B, 0x00, 0x62, 0x51, 0x6A, 0x1A, 0x0F, 0x21, 0x54, 0x17,
                                            0x00, 0x00, 0x0E, 0x00, 0x00, 0x02, 0xE2, 0x00, 0x69, 0x5B, 0x66, 0x1A,
                                            0x2C, 0x21, 0x90, 0x1B, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x02, 0x4D, 0x00};

// Read at 0x0348: three of user 1's, all stamped 2019-01-01 because the cuff's
// clock had never been set.
const std::vector<uint8_t> USER1_RECORDS = {0x66, 0x53, 0x48, 0x13, 0x20, 0x04, 0x3F, 0x10, 0x00, 0x00, 0x07, 0x00,
                                            0x00, 0x01, 0x8F, 0x00, 0x66, 0x56, 0x52, 0x13, 0x20, 0x04, 0x3F, 0x10,
                                            0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x9D, 0x00, 0x64, 0x52, 0x4E, 0x13,
                                            0x20, 0x04, 0x3F, 0x10, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01, 0x94, 0x00};

// Answers frames out of a memory image and keeps everything it was asked, in
// order. Replies are queued rather than delivered inline: a reply handed back
// from inside session_write would re-enter a session still in the middle of
// sending, which is a shape the real link cannot produce either.
class FakeCuff : public OmronSessionHost {
 public:
  std::vector<std::vector<uint8_t>> sent{};
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> writes{};
  bool settings_reported{false};
  bool transfer_complete{false};
  const char *failure{nullptr};
  bool has_wall_clock{false};
  OmronDateTime wall_clock{};
  // Fixed, so the very first frame of a session is reproducible.
  std::array<uint8_t, 4> nonce{{0xDE, 0xAD, 0xBE, 0xEF}};
  uint16_t settings_read_base{0};
  uint16_t settings_write_base{0};
  // Advanced by hand where a test needs a timer to come due.
  uint32_t now_ms{1000};
  // A cuff that stays silent through the classic handshake, which is the case
  // the retry exists for.
  bool answer_handshake{true};
  // Status byte behind 0x80 when the key is offered. Non-zero is a refusal, and
  // the cuff latches that state until it is power-cycled and put back into
  // pairing mode - so what the session does next matters.
  uint8_t key_program_status{0x00};
  // Confirms encryption and then says nothing to the key itself. Its own switch
  // rather than answer_handshake, because the two steps fail separately and the
  // second one has no reply to hang a retry off either.
  bool answer_key_program{true};
  // How many notify channels this cuff answers on. One for the modern families,
  // four for classic, and it decides how a reply reaches the session.
  uint8_t rx_channels{1};
  // Report protocol writes as still needing their acknowledgement rather than
  // as already on the wire. That is what a write-with-response really does.
  // Without it session_write always answers ON_THE_WIRE, which leaves
  // on_write_response and the whole reply-before-ack path dead code as far as
  // this suite is concerned.
  bool acknowledge_protocol_writes{false};
  int pending_acks{0};
  int overlapping_writes{0};

  void poke(uint16_t address, const std::vector<uint8_t> &bytes) {
    for (size_t index = 0; index < bytes.size(); index++)
      this->memory_[static_cast<uint16_t>(address + index)] = bytes[index];
  }

  // An erased cell reads 0xFF, which is what an untouched slot in this cuff's
  // ring actually holds.
  std::vector<uint8_t> peek(uint16_t address, size_t length) const {
    std::vector<uint8_t> out(length, 0xFF);
    for (size_t index = 0; index < length; index++) {
      auto found = this->memory_.find(static_cast<uint16_t>(address + index));
      if (found != this->memory_.end())
        out[index] = found->second;
    }
    return out;
  }

  // Delivers exactly one queued reply. A test that needs to speak while a
  // command is still in flight cannot use pump, which runs the session to a
  // standstill before it gets a word in.
  bool step(OmronSession &session) {
    if (this->queue_.empty())
      return false;
    const auto reply = this->queue_.front();
    this->queue_.erase(this->queue_.begin());
    if (reply.first) {
      session.on_unlock_notification(reply.second);
    } else if (this->rx_channels <= 1) {
      session.on_protocol_notification(0, reply.second);
    } else {
      // The classic families spread one reply across four notify channels,
      // sixteen bytes each. Handing the whole frame to channel 0 is not a
      // simplification, it is a shape the wire cannot produce - and the
      // assembler says so, with "fragment too large". Only the multi-channel
      // tests reach this path; the classic session test stops at the handshake.
      const size_t width = 16;
      size_t channel = 0;
      for (size_t offset = 0; offset < reply.second.size(); offset += width, channel++) {
        const size_t remaining = reply.second.size() - offset;
        session.on_protocol_notification(
            static_cast<uint8_t>(channel),
            std::span<const uint8_t>(reply.second).subspan(offset, remaining < width ? remaining : width));
      }
    }
    return true;
  }

  void pump(OmronSession &session) {
    size_t guard = 0;
    while (this->step(session))
      assert(++guard < 500);  // a session that never ends is a failure, not a hang
  }

  // Only the read frames, which is what "how deep did this session go" means.
  size_t read_frames() const {
    size_t count = 0;
    for (const auto &frame : this->sent) {
      if (frame.size() >= 3 && frame[1] == 0x01 && frame[2] == 0x00)
        count++;
    }
    return count;
  }

  WriteDispatch session_write(SessionChannel channel, std::span<const uint8_t> data, bool prefer_no_response) override {
    (void)prefer_no_response;
    this->sent.emplace_back(data.begin(), data.end());
    if (channel == SessionChannel::UNLOCK) {
      assert(!data.empty());
      if (data[0] == 0x11) {  // the token, echoed back with its nonce
        assert(data.size() >= 5);
        this->queue_.push_back({true, {0x91, 0x00, data[1], data[2], data[3], data[4]}});
      } else if (data[0] == 0x02 && this->answer_handshake) {
        this->queue_.push_back({true, {0x82, 0x00}});
      } else if (data[0] == 0x00 && this->answer_handshake && this->answer_key_program) {
        this->queue_.push_back({true, {0x80, this->key_program_status}});
      } else if (data[0] == 0x01 && this->answer_handshake) {
        this->queue_.push_back({true, {0x81, 0x00}});
      }
      return WriteDispatch::ON_THE_WIRE;
    }
    if (this->acknowledge_protocol_writes) {
      // The invariant this fake exists to police: a command must not go out
      // while the previous one is still waiting for its acknowledgement. Two
      // writes outstanding at once is what the session's reply-before-ack
      // handling prevents, and it is invisible in the frames themselves - the
      // same bytes come out either way, just with two in flight instead of one.
      if (this->pending_acks > 0)
        this->overlapping_writes++;
      this->pending_acks++;
      this->queue_.push_back({false, this->answer_(data)});
      return WriteDispatch::AWAITING_RESPONSE;
    }
    this->queue_.push_back({false, this->answer_(data)});
    return WriteDispatch::ON_THE_WIRE;
  }

  // Runs a whole session where every protocol write is acknowledged separately,
  // with the caller choosing whether the cuff's reply lands before or after that
  // acknowledgement. Both orders happen on a real link and the session must not
  // be able to tell them apart.
  void pump_acknowledged(OmronSession &session, bool reply_first) {
    size_t guard = 0;
    while (!this->queue_.empty() || this->pending_acks > 0) {
      assert(++guard < 500);
      // Only the acknowledgements outstanding when this pass began. Taking the
      // ones this pass creates too was the whole trouble with the first version
      // of this helper: a write sent and acknowledged inside one iteration is
      // never in flight when its reply lands, so both orders below collapsed
      // into the same one and the race was never produced at all.
      const int outstanding = this->pending_acks;
      if (reply_first) {
        if (!this->queue_.empty())
          this->step(session);
        for (int i = 0; i < outstanding; i++)
          this->acknowledge_(session);
      } else {
        for (int i = 0; i < outstanding; i++)
          this->acknowledge_(session);
        if (!this->queue_.empty())
          this->step(session);
      }
    }
  }

  uint32_t session_now_ms() override { return this->now_ms; }
  bool session_wall_clock(OmronDateTime &now) override {
    if (!this->has_wall_clock)
      return false;
    now = this->wall_clock;
    return true;
  }
  bool session_random_nonce(std::span<uint8_t> data) override {
    assert(data.size() == this->nonce.size());
    std::ranges::copy(this->nonce, data.begin());
    return true;
  }
  bool session_request_link_encryption() override { return true; }
  void session_settings_read(const std::vector<uint8_t> &settings) override {
    assert(settings.size() == CAPTURED_SETTINGS.size());
    this->settings_reported = true;
  }
  void session_transfer_complete() override { this->transfer_complete = true; }
  void session_failed(const char *reason, int code) override {
    (void)code;
    this->failure = reason;
  }
  const char *session_address() override { return "fake"; }

 protected:
  void acknowledge_(OmronSession &session) {
    if (this->pending_acks <= 0)
      return;
    this->pending_acks--;
    session.on_write_response(SessionChannel::PROTOCOL, WriteOutcome::OK, 0);
  }

  std::vector<uint8_t> answer_(std::span<const uint8_t> frame) {
    assert(frame.size() >= 8);
    assert(xor_bytes(frame) == 0);  // every frame this component sends must check out
    const uint16_t type = static_cast<uint16_t>((frame[1] << 8) | frame[2]);
    const uint16_t address = static_cast<uint16_t>((frame[3] << 8) | frame[4]);
    const uint8_t count = frame[5];
    switch (static_cast<PacketType>(type)) {
      case PacketType::START_REQUEST:
        return make_response(PacketType::START_RESPONSE);
      case PacketType::READ_REQUEST:
        return make_response(PacketType::READ_RESPONSE, address, this->peek(address, count));
      case PacketType::WRITE_REQUEST: {
        const std::span<const uint8_t> body = frame.subspan(6, count);
        const std::vector<uint8_t> payload(body.begin(), body.end());
        this->writes.emplace_back(address, payload);
        // A write at the write base shows up at the read base, which is the one
        // thing measured about these two addresses: same buffer, two aliases.
        if (this->settings_write_base != 0 && address >= this->settings_write_base) {
          const uint16_t mirrored =
              static_cast<uint16_t>(address - this->settings_write_base + this->settings_read_base);
          this->poke(mirrored, payload);
        }
        return make_response(PacketType::WRITE_RESPONSE, address);
      }
      case PacketType::END_REQUEST:
        return make_response(PacketType::END_RESPONSE);
      default:
        assert(false);
        return {};
    }
  }

  std::map<uint16_t, uint8_t> memory_{};
  // true for the unlock channel, which is where the token answer arrives.
  std::vector<std::pair<bool, std::vector<uint8_t>>> queue_{};
};

// A cuff loaded with the captured session, plus enough of each ring for the
// planner to have something to fetch.
void load_captured_cuff(FakeCuff &cuff, const OmronProfile &profile) {
  cuff.settings_read_base = profile.settings_read_address;
  cuff.settings_write_base = profile.settings_write_address;
  if (profile.gatt != nullptr)
    cuff.rx_channels = profile.gatt->rx_channel_count;
  cuff.poke(profile.settings_read_address, CAPTURED_SETTINGS);
  // The clock sits at the settings base plus the profile's time region, which
  // is 0x028C on this one and is where the capture read it.
  cuff.poke(static_cast<uint16_t>(profile.settings_read_address + profile.time_region_start), CAPTURED_CLOCK);
  // User 1 stands at nine records, user 2 at thirteen, so their newest slots are
  // eight and twelve. Real record bytes, so nothing downstream has to special
  // case an all-0xFF slot.
  const std::vector<uint8_t> record = {0x66, 0x53, 0x48, 0x13, 0x20, 0x04, 0x3F, 0x10,
                                       0x00, 0x00, 0x07, 0x00, 0x00, 0x01, 0x8F, 0x00};
  for (uint16_t slot = 0; slot < 9; slot++)
    cuff.poke(static_cast<uint16_t>(profile.users[0].record_start_address + slot * profile.record_size), record);
  for (uint16_t slot = 0; slot < 13; slot++)
    cuff.poke(static_cast<uint16_t>(profile.users[1].record_start_address + slot * profile.record_size), record);
}

OmronSessionConfig captured_session_config(const OmronProfile &profile, uint8_t history_records) {
  OmronSessionConfig config;
  config.profile = &profile;
  assert(make_poll_layout(profile, config.layout, history_records) == ProfileAdapterError::NONE);
  return config;
}

int64_t epoch_of(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  return civil_seconds(OmronDateTime{year, month, day, hour, minute, second});
}

// Slots newest first, which is the order the planner produces and the order the
// newest-record rule depends on.
UserRecordPlan plan_for(uint8_t user, std::vector<uint16_t> slots) {
  UserRecordPlan plan;
  plan.user = user;
  plan.slots = std::move(slots);
  return plan;
}

HarvestRequest harvest_request_for(const OmronProfile &profile, const PollLayout &layout,
                                   const OmronMemoryImage &memory, const std::vector<UserRecordPlan> &plans) {
  HarvestRequest request;
  request.profile = &profile;
  request.layout = &layout;
  request.memory = &memory;
  request.plans = &plans;
  request.history_records = 15;
  request.history_budget = 128;
  return request;
}

}  // namespace

void test_session_replays_the_captured_frame_order() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(captured_session_config(mw3, 3));

  session.begin(true);
  cuff.pump(session);

  assert(cuff.failure == nullptr);
  assert(cuff.transfer_complete);
  assert(cuff.settings_reported);
  assert(cuff.sent.size() >= 6);

  // The token, with the nonce this session was handed. Twenty bytes: opcode,
  // four nonce bytes, fifteen zeros.
  assert(cuff.sent[0].size() == 20);
  assert(cuff.sent[0][0] == 0x11);
  assert(cuff.sent[0][1] == 0xDE && cuff.sent[0][2] == 0xAD && cuff.sent[0][3] == 0xBE && cuff.sent[0][4] == 0xEF);

  // The four frames that precede any record read, in order.
  // Frame four is the one that regressed unnoticed: the clock read drifted to
  // last, behind every record, and no test at function level could see it.
  assert(cuff.sent[1] == START_FRAME);
  assert(cuff.sent[2] == SETTINGS_READ_FRAME);
  assert(cuff.sent[3] == CLOCK_READ_FRAME);
  assert(cuff.sent.back() == END_FRAME);

  // Everything in between is a record read, and the newest record of the first
  // user goes out on its own so the entity value arrives in the first of them.
  const auto newest_read = make_read_request(0x0368, 0x10);
  assert(cuff.sent[4] == std::vector<uint8_t>(newest_read.begin(), newest_read.end()));
  for (size_t index = 4; index + 1 < cuff.sent.size(); index++) {
    const auto &frame = cuff.sent[index];
    assert(frame[1] == 0x01 && frame[2] == 0x00);
    const uint16_t address = static_cast<uint16_t>((frame[3] << 8) | frame[4]);
    assert(address >= mw3.users[0].record_start_address);
  }
  assert(cuff.writes.empty());  // nothing configured to write in this session
}

void test_session_ignores_a_stray_frame_without_resending() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(captured_session_config(mw3, 3));

  // Far enough in that a read is on the wire and unanswered: token, then START,
  // then the settings read whose reply is still queued.
  session.begin(true);
  assert(cuff.step(session));  // token reply -> START goes out
  assert(cuff.step(session));  // START reply  -> the settings read goes out
  const size_t in_flight = cuff.sent.size();
  assert(cuff.sent.back() == SETTINGS_READ_FRAME);

  // A well formed read reply for an address nothing is waiting on. The cuff
  // sends one of these when an earlier block is answered twice, and the
  // transaction has always dropped it - but it reported the drop as NONE, and
  // the session read that as "the reply came", cleared its wait and re-sent the
  // read that was still in flight. Eight times over, since a stray never
  // touches the attempt counter that bounds a retry.
  const auto stray = make_response(PacketType::READ_RESPONSE, 0x1234, {0x00, 0x00});
  session.on_protocol_notification(0, stray);

  assert(cuff.sent.size() == in_flight);  // nothing went out on the strength of it
  assert(diagnostics.unexpected_replies == 1);
  assert(!session.failed());
  assert(session.waiting_for_reply());  // still waiting for the real answer

  // A frame the transaction cannot parse has to be dropped the same way. A bad
  // checksum never gets this far - the assembler refuses it and ends the session
  // - but an opcode nothing recognises assembles cleanly and reaches the
  // transaction, which consumes nothing. A session told the command was answered
  // would retire the read still in flight and carry on without the block.
  const auto unknown_opcode = make_response(static_cast<PacketType>(0x8200));
  session.on_protocol_notification(0, unknown_opcode);
  assert(cuff.sent.size() == in_flight);
  assert(diagnostics.unexpected_replies == 2);
  assert(!session.failed());
  assert(session.waiting_for_reply());

  // And the real answer, when it comes, is still accepted: a stray must cost
  // the session nothing at all.
  cuff.pump(session);
  assert(cuff.failure == nullptr);
  assert(cuff.transfer_complete);
  assert(cuff.sent.back() == END_FRAME);
  assert(diagnostics.unexpected_replies == 2);
}

void test_session_with_unmoved_cursors_reads_only_two_frames() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(captured_session_config(mw3, 3));

  session.begin(true);
  cuff.pump(session);
  assert(cuff.failure == nullptr);
  session.finish(true);

  // Same cuff, same rings, nothing measured in between. Measured on hardware:
  // two read frames and 4.9 seconds.
  cuff.sent.clear();
  session.reset();
  session.begin(true);
  cuff.pump(session);

  assert(cuff.failure == nullptr);
  assert(cuff.transfer_complete);
  assert(cuff.read_frames() == 2);
  assert(cuff.sent.size() == 5);
  assert(cuff.sent[1] == START_FRAME);
  assert(cuff.sent[2] == SETTINGS_READ_FRAME);
  assert(cuff.sent[3] == CLOCK_READ_FRAME);
  assert(cuff.sent[4] == END_FRAME);

  // A session that failed may not claim the ring. After finish(false) the next
  // one reads the records again, which is what stops a half-finished transfer
  // from swallowing a measurement.
  session.finish(false);
  cuff.sent.clear();
  session.reset();
  session.begin(true);
  cuff.pump(session);
  assert(cuff.read_frames() == 2);  // the previous *successful* session still stands
}

void test_session_full_read_on_pairing_needs_both_the_option_and_the_flag() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);

  OmronSessionConfig config = captured_session_config(mw3, 3);
  session.configure(config);
  session.begin(true);
  cuff.pump(session);
  const size_t full = cuff.read_frames();
  assert(full > 2);
  session.finish(true);

  // The pairing bit alone changes nothing: without the option the cursor check
  // still decides, which is what the cuff did on hardware before this existed.
  cuff.sent.clear();
  session.reset();
  session.set_pairing_advertised(true);
  session.begin(true);
  cuff.pump(session);
  assert(cuff.read_frames() == 2);
  session.finish(true);

  // Both together read the whole ring again.
  config.full_read_on_pairing = true;
  session.configure(config);
  cuff.sent.clear();
  session.reset();
  session.set_pairing_advertised(true);
  session.begin(true);
  cuff.pump(session);
  assert(cuff.failure == nullptr);
  assert(cuff.read_frames() == full);
  session.finish(true);

  // The option on its own does not: an ordinary press stays at two frames, so
  // turning it on does not cost every session a full transfer.
  cuff.sent.clear();
  session.reset();
  session.set_pairing_advertised(false);
  session.begin(true);
  cuff.pump(session);
  assert(cuff.read_frames() == 2);
}

void test_session_registration_writes_reach_the_wire() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);
  // Both users have a measurement the cuff has not been told we collected. The
  // state that hides a mistake here: a frame rebuilt at send time from a copy
  // that lost the field logs 00 80 and puts 01 00 on the wire, and only the
  // wire is checked below.
  cuff.poke(static_cast<uint16_t>(mw3.settings_read_address + 4), {0x01, 0x00});
  cuff.poke(static_cast<uint16_t>(mw3.settings_read_address + 6), {0x01, 0x00});
  cuff.has_wall_clock = true;
  cuff.wall_clock = OmronDateTime{2026, 8, 10, 17, 23, 44};

  OmronSessionConfig config = captured_session_config(mw3, 3);
  config.register_as_user = 2;
  config.birth_dates[1] = OmronDateTime{1911, 11, 11, 0, 0, 0};

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(config);
  session.set_paired_this_session(true);

  session.begin(true);
  cuff.pump(session);

  assert(cuff.failure == nullptr);
  assert(cuff.transfer_complete);
  assert(cuff.sent.back() == END_FRAME);

  // Two runs when user 2 registers: the pointer region alone, then the user
  // block and the clock together, because user 1's block lies between them.
  assert(cuff.writes.size() == 2);
  assert(cuff.writes[0].first == 0x02A4 && cuff.writes[0].second.size() == 24);
  assert(cuff.writes[1].first == 0x02C6 && cuff.writes[1].second.size() == 26);

  // What actually reached the cuff, not what the log line said: both users'
  // unsent counters cleared, because this session read both rings.
  const std::vector<uint8_t> &pointers = cuff.writes[0].second;
  assert(pointers[4] == 0x00 && pointers[5] == 0x80);
  assert(pointers[6] == 0x00 && pointers[7] == 0x80);

  // The block run carries the date this node chose and a version counter one
  // past what it read. Nine was in the captured settings; a session that pairs
  // steps it, and a session that does not must leave it alone.
  const std::vector<uint8_t> &block = cuff.writes[1].second;
  assert(block[0] == 11 && block[1] == 11 && block[2] == 11);  // 1911-11-11, year less 1900
  assert(block[4] == 10 && block[5] == 0 && block[6] == 0 && block[7] == 0);
  // And the clock, in the same run, set to the time the write went out.
  OmronDateTime written{};
  assert(parse_device_clock(std::span<const uint8_t>(block).subspan(10, 16), mw3.clock_fields_offset, written) ==
         ClockParseError::NONE);
  assert(written.year == 2026 && written.month == 8 && written.day == 10);
  assert(written.hour == 17 && written.minute == 23 && written.second == 44);
}

void test_session_writes_a_birth_date_without_registering() {
  // A cuff holds two people and this node registers as one of them. The other's
  // date has no way in short of a second pairing, which is what the opt-in
  // exists for. Off by default: no other host sends this shape.
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);
  cuff.has_wall_clock = true;
  cuff.wall_clock = OmronDateTime{2026, 8, 11, 18, 0, 0};

  OmronSessionConfig config = captured_session_config(mw3, 3);
  config.register_as_user = 2;
  config.birth_dates[0] = OmronDateTime{1922, 2, 22, 0, 0, 0};  // user 1, the one not registering
  config.birth_dates[1] = OmronDateTime{1911, 11, 11, 0, 0, 0};
  config.write_birth_date_users = 1 << 0;

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(config);
  session.set_paired_this_session(true);
  session.begin(true);
  cuff.pump(session);
  assert(cuff.failure == nullptr && cuff.transfer_complete);

  // All four regions now touch, so the merge would ask for sixty bytes in one
  // frame - past the cuff's own maximum. Capped at the transfer block size it
  // comes out as the pointer region with both blocks, then the clock.
  assert(cuff.writes.size() == 2);
  assert(cuff.writes[0].first == 0x02A4 && cuff.writes[0].second.size() == 44);
  assert(cuff.writes[1].first == 0x02D0 && cuff.writes[1].second.size() == 16);

  // User 1's block sits at offset 24 of that first run: the date this node chose
  // and a version counter left exactly as it was read. Stepping it would claim a
  // registration that did not happen.
  const std::vector<uint8_t> &run = cuff.writes[0].second;
  assert(run[24] == 22 && run[25] == 2 && run[26] == 22);
  assert(run[28] == 1 && run[29] == 0 && run[30] == 0 && run[31] == 0);
  // And the block still adds up, which is the guard that a wrong layout trips.
  uint32_t sum = 0;
  for (size_t i = 24; i < 32; i++)
    sum += run[i];
  assert(run[32] == static_cast<uint8_t>(sum & 0xFF));

  // The cuff now holds that date, so a later session must not spend another
  // EEPROM write on it. This is what stops the option costing a write forever.
  session.finish(true);
  cuff.writes.clear();
  session.reset();
  session.set_paired_this_session(false);
  session.begin(true);
  cuff.pump(session);
  for (const auto &write : cuff.writes)
    assert(write.first != 0x02A4 || write.second.size() != 44);
}

void test_session_survives_the_reply_racing_the_write_ack() {
  // A write-with-response finishes twice: once when the stack takes the bytes,
  // once when the peer answers the command. Nothing orders those two against
  // each other, and this cuff has been seen doing both.
  //
  // FakeCuff has to be able to answer AWAITING_RESPONSE, or
  // WriteDispatch::AWAITING_RESPONSE and the whole of on_write_response go
  // unexecuted. Two async events whose order is assumed rather than handled is
  // the class of fault this group exists for.
  //
  // The assertion is not "it works". It is that the two orders are
  // indistinguishable on the wire, byte for byte, frame for frame.
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);

  std::vector<std::vector<uint8_t>> ack_first;
  std::vector<std::vector<uint8_t>> reply_first;

  for (int pass = 0; pass < 2; pass++) {
    FakeCuff cuff;
    load_captured_cuff(cuff, mw3);
    cuff.acknowledge_protocol_writes = true;

    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(captured_session_config(mw3, 3));

    session.begin(true);
    cuff.pump_acknowledged(session, pass == 1);

    assert(cuff.failure == nullptr);
    assert(cuff.transfer_complete);
    assert(cuff.settings_reported);
    assert(cuff.sent.back() == END_FRAME);
    // Nothing was dropped as arriving out of turn. A stray count here would mean
    // the session had lost track of which acknowledgement belonged to what.
    assert(diagnostics.unexpected_replies == 0);
    // Never two commands outstanding. This is what the reply-before-ack
    // handling buys, and the only place it is observable.
    assert(cuff.overlapping_writes == 0);
    (pass == 0 ? ack_first : reply_first) = cuff.sent;
  }

  assert(!ack_first.empty());
  assert(ack_first == reply_first);

  // And the same session driven the way every other test drives it - no separate
  // acknowledgement at all - still produces those very same frames.
  FakeCuff plain;
  load_captured_cuff(plain, mw3);
  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&plain);
  session.set_diagnostics(&diagnostics);
  session.configure(captured_session_config(mw3, 3));
  session.begin(true);
  plain.pump(session);
  assert(plain.failure == nullptr && plain.transfer_complete);
  assert(plain.sent == ack_first);
}

void test_session_wire_state_guards() {
  // The happy paths through the command wire state were covered; its guards were
  // not. Measured by mutation: three separate breakages of the conditions
  // below left the rest of the suite green, and every one of them is a way for
  // a command to be answered by the wrong event.
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);

  // 1. An acknowledgement with nothing in flight. A duplicate, or one arriving
  // after the session has moved on. It must be counted and dropped, never
  // consumed as if it belonged to the command now outstanding.
  {
    // No acknowledgements at all from this link, so every command goes out as
    // ON_THE_WIRE and the session is waiting for a reply, never for an ack.
    FakeCuff cuff;
    load_captured_cuff(cuff, mw3);
    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(captured_session_config(mw3, 3));
    session.begin(true);
    assert(session.wire_state() == CommandWireState::AWAITING_REPLY);

    // An acknowledgement for a write this session is not waiting on. Consuming
    // it would move the command along on the strength of an event that belongs
    // to something else.
    session.on_write_response(SessionChannel::PROTOCOL, WriteOutcome::OK, 0);
    assert(diagnostics.unexpected_replies == 1);
    assert(session.wire_state() == CommandWireState::AWAITING_REPLY);
    assert(cuff.failure == nullptr);

    // And the session still completes normally afterwards.
    cuff.pump(session);
    assert(cuff.failure == nullptr && cuff.transfer_complete);
    assert(diagnostics.unexpected_replies == 1);
  }

  // 2. The reply timeout belongs to a command nobody has answered. A write still
  // waiting for its acknowledgement has the host's own write timeout, and timing
  // it out here would retry a command the link has not finished sending.
  {
    FakeCuff cuff;
    load_captured_cuff(cuff, mw3);
    cuff.acknowledge_protocol_writes = true;
    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(captured_session_config(mw3, 3));
    session.begin(true);
    // begin() sends the token, which goes out as a write command and so is
    // already gone. One step further is a protocol write, which this link
    // acknowledges separately - that is the state under test.
    cuff.step(session);
    assert(session.wire_state() == CommandWireState::AWAITING_WRITE_ACK);

    // An hour of ticks, with the acknowledgement deliberately withheld.
    for (uint32_t t = 0; t < 3600; t++)
      session.tick(cuff.now_ms + t * 1000);
    assert(session.wire_state() == CommandWireState::AWAITING_WRITE_ACK);
    assert(diagnostics.protocol_failures == 0);
    assert(cuff.failure == nullptr);
  }

  // 3. A command must start from nothing outstanding. The reset at the top of
  // send_pending_command_ was one of three assignments before this refactor, and
  // forgetting one of the three is the shape of both bugs above.
  {
    FakeCuff cuff;
    load_captured_cuff(cuff, mw3);
    cuff.acknowledge_protocol_writes = true;
    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(captured_session_config(mw3, 3));
    session.begin(true);

    // Drive the whole transfer with the reply beating the acknowledgement every
    // time, so every command is sent from a state the previous one reached
    // through REPLY_EARLY rather than through the simple path.
    cuff.pump_acknowledged(session, true);
    assert(cuff.failure == nullptr && cuff.transfer_complete);
    assert(diagnostics.unexpected_replies == 0);
    assert(cuff.overlapping_writes == 0);
    // Nothing left outstanding once the envelope is closed.
    assert(session.wire_state() == CommandWireState::IDLE);
  }
}

void test_session_fails_when_the_link_refuses_the_write() {
  // The other half of on_write_response: the stack takes the bytes and then
  // reports it could not send them. That must end the session rather than leave
  // it waiting for a reply to a command the cuff never received.
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  FakeCuff cuff;
  load_captured_cuff(cuff, mw3);
  cuff.acknowledge_protocol_writes = true;

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(captured_session_config(mw3, 3));

  session.begin(true);
  cuff.step(session);  // token reply, which puts the first protocol write out
  assert(cuff.pending_acks == 1);
  session.on_write_response(SessionChannel::PROTOCOL, WriteOutcome::FAILED, 0x81);

  assert(session.failed());
  assert(cuff.failure != nullptr);
  assert(!cuff.transfer_complete);
}

void test_session_pairing_programs_the_key_before_it_reads() {
  // The highest-stakes write this component makes: it puts a key of this
  // node's choosing into a user slot on the cuff, works only while the display
  // blinks -P-, and a refusal latches until the device is power-cycled.
  // PairStep::PROGRAM is unreachable unless a test calls arm_pairing().
  const OmronProfile &classic = get_profile(OmronProfileId::HEM_7155T);
  assert(classic.unlock_mode == UnlockMode::CLASSIC_KEY);

  OmronBindKey key{};
  for (size_t i = 0; i < key.size(); i++)
    key[i] = static_cast<uint8_t>(0x10 + i);

  {
    FakeCuff cuff;
    load_captured_cuff(cuff, classic);
    OmronSessionConfig config = captured_session_config(classic, 3);
    config.bind_key_set = true;
    config.bind_key = key;

    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(config);
    // What the client does at discovery, from the handles it actually resolved.
    session.set_rx_channel_count(cuff.rx_channels);
    session.arm_pairing();
    assert(session.pairing_armed());

    session.begin(true);
    assert(cuff.sent.size() == 1);
    assert(cuff.sent[0][0] == 0x02);  // confirm encryption first, pairing or not

    // The cuff answers 82, and only then does the key go out. Nothing may write
    // a key before that verdict: a key offered to a cuff that has not confirmed
    // encryption is the refusal that latches until it is power-cycled.
    assert(cuff.step(session));
    assert(cuff.sent.size() == 2);
    assert(cuff.sent[1][0] == 0x00);  // program, not authenticate
    for (size_t i = 0; i < key.size(); i++)
      assert(cuff.sent[1][1 + i] == key[i]);
    // Padded, never truncated: the frame is seventeen bytes of content and the
    // variant may want it wider.
    assert(cuff.sent[1].size() >= 17);
    // Still armed: the key is on the wire and the cuff has not answered. A lost
    // reply must not cost the walk to the cuff and the held button behind it.
    assert(session.pairing_armed());

    // The cuff accepts, and the session reads on in the same connection. One
    // press of the button should leave both a usable key and a set of records.
    cuff.pump(session);
    assert(cuff.failure == nullptr);
    assert(cuff.transfer_complete);
    // And spent once the cuff answered. A second session authenticates with the
    // key rather than programming it again.
    assert(!session.pairing_armed());
    // Authenticate with the key that was just programmed, then the ordinary
    // envelope. A classic profile opens its transfer with 0x01 and the stored
    // key; the key it now holds is the one written two frames ago.
    assert(cuff.sent[2][0] == 0x01);
    for (size_t i = 0; i < key.size(); i++)
      assert(cuff.sent[2][1 + i] == key[i]);
    assert(cuff.sent[3] == START_FRAME);
    assert(cuff.sent.back() == END_FRAME);
    assert(cuff.read_frames() > 0);
  }

  // A cuff that refuses the key still gives up its records. Getting one into
  // pairing mode costs a walk and a held button; ending the session over a
  // refusal spends that and returns nothing.
  {
    FakeCuff cuff;
    load_captured_cuff(cuff, classic);
    cuff.key_program_status = 0x01;  // rejected
    OmronSessionConfig config = captured_session_config(classic, 3);
    config.bind_key_set = true;
    config.bind_key = key;

    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(config);
    // What the client does at discovery, from the handles it actually resolved.
    session.set_rx_channel_count(cuff.rx_channels);
    session.arm_pairing();
    session.begin(true);
    cuff.pump(session);

    assert(cuff.sent[1][0] == 0x00);
    assert(cuff.failure == nullptr);  // a refusal is not a failed session
    assert(cuff.transfer_complete);
    assert(cuff.read_frames() > 0);
  }

  // A cuff that confirms encryption and then says nothing to the key itself.
  // Without a clock on that write the session holds the connection until the
  // peer drops it, and nothing ever offers the key again.
  {
    FakeCuff cuff;
    load_captured_cuff(cuff, classic);
    cuff.answer_key_program = false;
    OmronSessionConfig config = captured_session_config(classic, 3);
    config.bind_key_set = true;
    config.bind_key = key;

    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(config);
    session.set_rx_channel_count(cuff.rx_channels);
    session.arm_pairing();
    session.begin(true);
    assert(cuff.step(session));  // 82 confirms, and the key goes out
    assert(cuff.sent.size() == 2 && cuff.sent[1][0] == 0x00);

    // Silence. Only the clock can move this on, and it must offer the key again
    // rather than fall through to a plain read: the intent is not spent until
    // the cuff has answered.
    cuff.now_ms += 500;
    session.tick(cuff.now_ms);
    assert(cuff.sent.size() == 2);
    cuff.now_ms += 500;
    session.tick(cuff.now_ms);
    assert(cuff.sent.size() == 3 && cuff.sent[2][0] == 0x02);
    assert(session.pairing_armed());

    // Answering now still programs the key, in the same session.
    cuff.answer_key_program = true;
    cuff.pump(session);
    assert(cuff.failure == nullptr);
    assert(cuff.transfer_complete);
    assert(!session.pairing_armed());
    bool key_written = false;
    for (const auto &frame : cuff.sent)
      key_written = key_written || (frame.size() > 16 && frame[0] == 0x00 && frame[1] == key[0]);
    assert(key_written);
  }

  // Armed with no key to program is refused before anything reaches the wire.
  // Writing sixteen zero bytes into a user slot would pair this node to a key
  // it does not know, and the cuff has no undo for that.
  {
    FakeCuff cuff;
    load_captured_cuff(cuff, classic);
    OmronSessionConfig config = captured_session_config(classic, 3);
    config.bind_key_set = false;

    OmronDiagnostics diagnostics{};
    OmronSession session;
    session.set_host(&cuff);
    session.set_diagnostics(&diagnostics);
    session.configure(config);
    // What the client does at discovery, from the handles it actually resolved.
    session.set_rx_channel_count(cuff.rx_channels);
    session.arm_pairing();
    session.begin(true);

    assert(session.failed());
    assert(cuff.failure != nullptr);
    assert(cuff.sent.empty());
  }
}

void test_session_classic_handshake_retries_on_its_own_clock() {
  // The classic families open every session by asking the cuff to confirm the
  // link encryption, and a cuff that says nothing at all is the case the retry
  // exists for: a retry hung off the reply handler never runs, because no reply
  // comes. The session keeps that timer on the clock it is already handed, and
  // this is the only path that exercises it.
  const OmronProfile &classic = get_profile(OmronProfileId::HEM_7155T);
  assert(classic.unlock_mode == UnlockMode::CLASSIC_KEY);

  FakeCuff cuff;
  cuff.answer_handshake = false;  // total silence, the shape that needs a clock
  OmronSessionConfig config = captured_session_config(classic, 3);
  config.bind_key_set = true;
  config.bind_key.fill(0xA5);

  OmronDiagnostics diagnostics{};
  OmronSession session;
  session.set_host(&cuff);
  session.set_diagnostics(&diagnostics);
  session.configure(config);

  session.begin(true);
  assert(cuff.sent.size() == 1);
  assert(cuff.sent[0][0] == 0x02);  // confirm encryption

  // Nothing comes back, so only the clock can move this on. A tick before the
  // interval must not.
  cuff.now_ms = 1500;
  session.tick(cuff.now_ms);
  assert(cuff.sent.size() == 1);

  cuff.now_ms = 2000;
  session.tick(cuff.now_ms);
  assert(cuff.sent.size() == 2 && cuff.sent[1][0] == 0x02);

  cuff.now_ms = 3000;
  session.tick(cuff.now_ms);
  assert(cuff.sent.size() == 3 && cuff.sent[2][0] == 0x02);

  // Half the attempts go out at seventeen bytes and half at twenty, so one
  // window answers both "does this variant take the command" and "does it want
  // the wider frame" instead of costing a second press of the cuff's button.
  assert(cuff.sent[0].size() == 17);
  assert(cuff.sent[1].size() == 20 && cuff.sent[2].size() == 20);

  // The budget is spent, and the session reads records anyway rather than
  // ending: getting a cuff into pairing mode costs a walk and a held button,
  // and giving nothing back for it is worse than useless. On a classic profile
  // the read opens with the stored key.
  cuff.now_ms = 4000;
  session.tick(cuff.now_ms);
  assert(cuff.sent.size() == 4);
  assert(cuff.sent[3][0] == 0x01);  // authenticate with the key we already hold
  assert(cuff.failure == nullptr);
}

// --- Which reading a person actually sees ---
//
// Of everything this component decides, these are the decisions that reach a
// human: which slot the entities get, which older records still owe Home
// Assistant an event, and which are thrown away. A wrong answer here does not
// look like an error - it looks like a blood pressure.

void test_harvest_picks_the_reading_the_entities_show() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  PollLayout layout{};
  assert(make_poll_layout(mw3, layout, 15) == ProfileAdapterError::NONE);

  // 0x0768 is user 2's slot twelve, 0x0348 is user 1's slot six.
  OmronMemoryImage memory;
  memory.add_block(0x0768, USER2_RECORDS);
  memory.add_block(0x0348, USER1_RECORDS);

  const std::vector<UserRecordPlan> plans = {plan_for(0, {8, 7, 6}), plan_for(1, {14, 13, 12})};
  const HarvestResult harvest = harvest_records(harvest_request_for(mw3, layout, memory, plans));

  // User 2's newest is record fifteen: the reading the entities showed for it.
  const HarvestedUser &user2 = harvest[1];
  assert(user2.valid);
  assert(user2.newest.slot == 14);
  assert(user2.newest.measurement.systolic_mm_hg == 130);
  assert(user2.newest.measurement.diastolic_mm_hg == 91);
  assert(user2.newest.measurement.pulse_bpm == 102);
  assert(user2.newest.measurement.timestamp.year == 2026 && user2.newest.measurement.timestamp.month == 8);
  assert(user2.newest.measurement.timestamp.day == 9 && user2.newest.measurement.timestamp.hour == 12);
  assert(user2.parsed == 3 && user2.kept == 3 && user2.dropped_before_cutoff == 0);

  // User 1's three are all stamped 2019-01-01, so the newest is decided by the
  // slot and nothing else.
  const HarvestedUser &user1 = harvest[0];
  assert(user1.valid);
  assert(user1.newest.slot == 8);
  assert(user1.newest.measurement.systolic_mm_hg == 125);
  assert(user1.newest.measurement.timestamp.year == 2019);

  // History comes back oldest first, which is what keeps the watermark moving
  // forward and Home Assistant receiving events in the order they happened.
  assert(user2.history.size() == 3);
  for (size_t index = 0; index + 1 < user2.history.size(); index++)
    assert(user2.history[index].epoch < user2.history[index + 1].epoch);
  assert(user2.watermark_advanced);
  assert(user2.watermark == epoch_of(2026, 8, 9, 12, 46, 16));
  // The fingerprint is what the duplicate check runs on, so two people's
  // records must never collide even at the same address.
  assert(user1.newest.fingerprint != user2.newest.fingerprint);
}

void test_harvest_prefers_the_cursor_over_the_clock() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  PollLayout layout{};
  assert(make_poll_layout(mw3, layout, 15) == ProfileAdapterError::NONE);

  // The same two captured records, swapped: the one stamped 2026-08-09 put in
  // the older slot, the one stamped 2026-08-04 in the newer. Nothing here is
  // synthetic - only their addresses differ from the capture.
  const std::vector<uint8_t> newer_stamp(USER2_RECORDS.begin() + 32, USER2_RECORDS.begin() + 48);
  const std::vector<uint8_t> older_stamp(USER2_RECORDS.begin(), USER2_RECORDS.begin() + 16);
  OmronMemoryImage memory;
  memory.add_block(0x0768, newer_stamp);                              // slot 12
  memory.add_block(static_cast<uint16_t>(0x0768 + 16), older_stamp);  // slot 13

  const std::vector<UserRecordPlan> plans = {plan_for(1, {13, 12})};
  const HarvestResult harvest = harvest_records(harvest_request_for(mw3, layout, memory, plans));

  // Slot thirteen wins although its stamp is five days older. The cuff stamps
  // everything the same until someone sets its time, so one wrong stamp would
  // otherwise outrank every real reading for good.
  const HarvestedUser &user2 = harvest[1];
  assert(user2.valid && user2.newest.slot == 13);
  assert(user2.newest.measurement.systolic_mm_hg == 115);
  assert(user2.newest.measurement.timestamp.day == 4);
}

void test_harvest_cutoff_watermark_and_budget() {
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  PollLayout layout{};
  assert(make_poll_layout(mw3, layout, 15) == ProfileAdapterError::NONE);
  OmronMemoryImage memory;
  memory.add_block(0x0768, USER2_RECORDS);
  memory.add_block(0x0348, USER1_RECORDS);
  const std::vector<UserRecordPlan> plans = {plan_for(0, {8, 7, 6}), plan_for(1, {14, 13, 12})};

  // The cut-off drops a record whole: no entity, no event, no watermark. On this
  // cuff it empties user 1, whose records all predate the clock being set.
  {
    HarvestRequest request = harvest_request_for(mw3, layout, memory, plans);
    request.cutoff_set = true;
    request.cutoff_epoch = epoch_of(2020, 1, 1, 0, 0, 0);
    const HarvestResult harvest = harvest_records(request);
    assert(!harvest[0].valid);
    assert(harvest[0].parsed == 3 && harvest[0].kept == 0 && harvest[0].dropped_before_cutoff == 3);
    assert(harvest[0].history.empty() && !harvest[0].watermark_advanced);
    assert(harvest[1].valid && harvest[1].kept == 3);  // user 2 is untouched by it
  }

  // A watermark at the middle record leaves exactly the one behind it.
  {
    HarvestRequest request = harvest_request_for(mw3, layout, memory, plans);
    request.watermark[1] = epoch_of(2026, 8, 9, 12, 5, 20);  // record fourteen
    const HarvestResult harvest = harvest_records(request);
    assert(harvest[1].history.size() == 1);
    assert(harvest[1].history[0].measurement.systolic_mm_hg == 130);
    assert(harvest[1].watermark == epoch_of(2026, 8, 9, 12, 46, 16));
    // The entity still gets the newest record. A watermark says what has already
    // been reported as history, not what the entities may show.
    assert(harvest[1].valid && harvest[1].newest.slot == 14);
  }

  // A node whose own clock is far behind refuses to move the watermark past
  // records it cannot vouch for. Left unguarded, one future stamp silences that
  // user for good.
  {
    HarvestRequest request = harvest_request_for(mw3, layout, memory, plans);
    request.now_known = true;
    request.now_epoch = epoch_of(2019, 1, 1, 0, 0, 0);
    const HarvestResult harvest = harvest_records(request);
    assert(harvest[1].history.empty());
    assert(harvest[1].dropped_in_future == 3);
    assert(!harvest[1].watermark_advanced);
    assert(harvest[1].valid);  // the entity value is not gated on this
  }

  // A full queue truncates rather than growing without bound, and says so.
  {
    HarvestRequest request = harvest_request_for(mw3, layout, memory, plans);
    request.history_budget = 1;
    const HarvestResult harvest = harvest_records(request);
    const size_t queued = harvest[0].history.size() + harvest[1].history.size();
    assert(queued == 1);
    assert(harvest[0].history_truncated || harvest[1].history_truncated);
  }

  // History off still publishes the newest reading; it only stops the older
  // ones leaving as events.
  {
    HarvestRequest request = harvest_request_for(mw3, layout, memory, plans);
    request.history_records = 0;
    const HarvestResult harvest = harvest_records(request);
    assert(harvest[1].valid && harvest[1].newest.slot == 14);
    assert(harvest[1].history.empty() && !harvest[1].watermark_advanced);
  }

  // Bytes arrive and decode to nothing, and the plan names slots that never come
  // back at all. Collapsed into silent continues, an empty ring, a ring
  // nothing can decode and a ring never delivered all answer "no valid
  // measurement record" and none of them says which.
  //
  // The middle one is the reason this matters. A profile aimed at the wrong
  // region reads real bytes and refuses every one of them, and every family in
  // this catalog but one is configured from a source nobody has checked against
  // hardware.
  {
    // A real date - 2026-08-09 12:00 - carrying a systolic of 25, which is below
    // the plausibility floor. Valid enough to reach the values check and fail it.
    std::array<uint8_t, 16> refused{};
    refused[0] = 0x00;  // 0 + 25 mmHg
    refused[1] = 80;
    refused[2] = 70;
    refused[3] = 26;    // year 2026
    refused[4] = 0x2C;  // hour 12, day 9, month 8, packed
    refused[5] = 0x21;
    std::vector<uint8_t> ring;
    for (int index = 0; index < 3; index++)
      ring.insert(ring.end(), refused.begin(), refused.end());

    OmronMemoryImage undecodable;
    undecodable.add_block(0x0768, ring);
    const HarvestResult harvest = harvest_records(harvest_request_for(mw3, layout, undecodable, plans));

    assert(!harvest[1].valid);
    assert(harvest[1].parsed == 0);
    assert(harvest[1].unparsed == 3);
    assert(harvest[1].first_parse_error == MeasurementParseError::INVALID_MEASUREMENT);
    assert(harvest[1].unreadable == 0);

    // User 1's slots were planned and the transfer never delivered them, which
    // is a fault on our side of the link and not a decoding question.
    assert(!harvest[0].valid);
    assert(harvest[0].unreadable == 3);
    assert(harvest[0].unparsed == 0);
    assert(harvest[0].first_parse_error == MeasurementParseError::NONE);
  }

  // An empty ring still reports itself as empty rather than as nothing at all.
  {
    std::vector<uint8_t> erased(48, 0xFF);
    OmronMemoryImage blank;
    blank.add_block(0x0768, erased);
    const HarvestResult harvest = harvest_records(harvest_request_for(mw3, layout, blank, plans));
    assert(harvest[1].unparsed == 3);
    assert(harvest[1].first_parse_error == MeasurementParseError::EMPTY_SLOT);
  }

  // A half-finished session hands over nothing rather than crashing.
  {
    HarvestRequest request;
    const HarvestResult harvest = harvest_records(request);
    assert(!harvest[0].valid && !harvest[1].valid);
  }
}
