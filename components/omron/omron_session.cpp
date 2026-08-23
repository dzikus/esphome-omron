#include "omron_session.h"

#include <algorithm>

#include "omron_log.h"

namespace esphome::omron {

static const char *const TAG = "omron.session";

static_assert(OmronSession::USER_SLOTS == std::tuple_size_v<decltype(PollLayout::users)>,
              "the cursor watermarks must cover every user the poll layout can plan for");

void OmronSession::configure(const OmronSessionConfig &config) {
  this->config_ = config;
  this->reset();
}

void OmronSession::reset() {
  this->transaction_.reset();
  this->transaction_.clear_read_ranges();
  this->frame_assembler_ =
      OmronFrameAssembler(this->config_.profile != nullptr && this->config_.profile->gatt != nullptr
                              ? this->config_.profile->gatt->rx_channel_count
                              : 1);
  this->index_memory_.clear();
  this->record_memory_.clear();
  this->record_plans_.clear();
  this->poll_phase_ = PollPhase::NONE;

  this->active_command_ = {};
  this->wire_ = CommandWireState::IDLE;
  this->reply_started_at_ = 0;
  this->token_force_response_ = false;
  this->failed_ = false;

  // Not pair_armed_: the button can arm a session before the cuff is reachable,
  // and that intent has to survive until a session actually gets far enough to
  // spend it.
  this->pair_step_ = PairStep::NONE;
  this->pair_attempt_ = 0;
  this->paired_this_session_ = false;
  this->encryption_requested_ = false;
  this->unlock_channel_available_ = false;
  this->handshake_retry_armed_ = false;
  this->handshake_retry_armed_at_ = 0;

  this->clock_write_attempted_ = false;
  this->clock_write_queued_ = false;
}

void OmronSession::set_rx_channel_count(uint8_t count) {
  this->frame_assembler_ = OmronFrameAssembler(count);
}

void OmronSession::abort() {
  this->failed_ = true;
}

void OmronSession::fail_(const char *reason, int code) {
  if (this->failed_)
    return;
  this->failed_ = true;
  if (this->host_ != nullptr)
    this->host_->session_failed(reason, code);
}

void OmronSession::note_unexpected_reply_(const char *what) {
  if (this->diagnostics_ != nullptr)
    this->diagnostics_->unexpected_replies++;
  OMRON_LOG_W(TAG, "[%s] Ignored: %s", this->host_ != nullptr ? this->host_->session_address() : "?", what);
}

void OmronSession::finish(bool success) {
  // Only a session that finished cleanly may claim it collected a ring. A
  // failed one leaves the previous cursors standing, so the next attempt reads
  // the records again instead of skipping them on the strength of a session
  // that never got through them.
  if (success) {
    for (size_t user = 0; user < USER_SLOTS; user++) {
      if (!this->has_staged_cursor_[user])
        continue;
      this->polled_cursor_[user] = this->staged_cursor_[user];
      this->has_polled_cursor_[user] = true;
    }
  }
  for (size_t user = 0; user < USER_SLOTS; user++)
    this->has_staged_cursor_[user] = false;
}

bool OmronSession::transfer_open() const {
  const TransactionState state = this->transaction_.state();
  return state == TransactionState::READ_PENDING || state == TransactionState::WRITE_PENDING ||
         state == TransactionState::END_PENDING;
}

uint16_t OmronSession::settings_read_len_() const {
  return this->config_.profile != nullptr && this->config_.profile->time_region_start != 0
             ? this->config_.profile->time_region_start
             : SETTINGS_DUMP_LEN;
}

uint16_t OmronSession::settings_region_len_() const {
  return this->config_.profile != nullptr && this->config_.profile->time_region_end != 0
             ? this->config_.profile->time_region_end
             : SETTINGS_DUMP_LEN;
}

void OmronSession::update_transaction_phase_() {
  if (this->diagnostics_ == nullptr)
    return;
  switch (this->transaction_.state()) {
    case TransactionState::KEY_PENDING:
    case TransactionState::TOKEN_PENDING:
      this->diagnostics_->phase = SessionPhase::UNLOCKING;
      break;
    case TransactionState::START_PENDING:
      this->diagnostics_->phase = SessionPhase::STARTING_TRANSFER;
      break;
    case TransactionState::READ_PENDING:
      this->diagnostics_->phase =
          this->poll_phase_ == PollPhase::INDEX ? SessionPhase::READING_INDEX : SessionPhase::READING_RECORDS;
      break;
    case TransactionState::END_PENDING:
      this->diagnostics_->phase = SessionPhase::ENDING_TRANSFER;
      break;
    default:
      break;
  }
}

// --- The handshake ---

uint8_t OmronSession::handshake_budget_() const {
  return this->pair_armed_ ? PAIR_MAX_ATTEMPTS : CONFIRM_MAX_ATTEMPTS;
}

size_t OmronSession::handshake_frame_length_() const {
  // Half the attempts each way, so one window answers both "does this variant
  // take the command at all" and "does it want the wider frame" instead of
  // costing a second press of the cuff's button.
  return this->pair_attempt_ <= this->handshake_budget_() / 2 ? OmronUnlockFrame{}.size() : PAIR_MAX_FRAME_LEN;
}

bool OmronSession::write_unlock_frame_(const OmronUnlockFrame &frame, size_t length) {
  if (this->host_ == nullptr || !this->unlock_channel_available_ || length < frame.size())
    return false;
  // Padded on request: the key commands are seventeen bytes and the token
  // handshake twenty, and a cuff expecting a fixed width ignores anything
  // shorter rather than complaining.
  std::array<uint8_t, PAIR_MAX_FRAME_LEN> buffer{};
  // The two widths are declared in different headers, so nothing but this ties
  // them together.
  static_assert(OmronUnlockFrame{}.size() <= PAIR_MAX_FRAME_LEN, "the padded buffer must hold a whole unlock frame");
  std::ranges::copy(frame, buffer.begin());
  return this->host_->session_write(SessionChannel::UNLOCK, std::span<const uint8_t>(buffer).first(length), false) !=
         WriteDispatch::FAILED;
}

void OmronSession::begin(bool unlock_channel_available) {
  this->unlock_channel_available_ = unlock_channel_available;
  this->begin_handshake_();
}

void OmronSession::begin_handshake_() {
  if (this->failed_ || this->host_ == nullptr)
    return;
  // The first thing on every session, pairing or not: the cuff answers 0x82
  // with its own verdict on link encryption, which is how a host that never
  // re-encrypts a bonded link still ends up talking over an encrypted one.
  if (!this->unlock_channel_available_ || this->config_.profile == nullptr ||
      this->config_.profile->unlock_mode != UnlockMode::CLASSIC_KEY) {
    this->begin_index_transaction_();
    return;
  }
  if (this->pair_armed_ && !this->config_.bind_key_set) {
    this->fail_("Pairing needs a bindkey in the hub configuration", 0);
    return;
  }

  const uint8_t budget = this->handshake_budget_();
  if (this->pair_attempt_ >= budget) {
    this->pair_step_ = PairStep::NONE;
    // Falls through to the ordinary read rather than ending the session. Getting
    // the cuff into pairing mode costs a deliberate press and a walk to wherever
    // it lives; spending that and giving nothing back but Err on its display is
    // worse than useless.
    OMRON_LOG_W(TAG, "[%s] Cuff never confirmed encryption; reading records anyway", this->host_->session_address());
    this->pair_armed_ = false;
    this->begin_index_transaction_();
    return;
  }
  this->pair_step_ = PairStep::CONFIRM;
  this->pair_attempt_++;
  const size_t length = this->handshake_frame_length_();
  OMRON_LOG_I(TAG, "[%s] Session: asking the cuff to confirm encryption (attempt %u/%u, %u-byte frame)",
              this->host_->session_address(), static_cast<unsigned>(this->pair_attempt_), static_cast<unsigned>(budget),
              static_cast<unsigned>(length));
  if (!this->write_unlock_frame_(make_confirm_encryption_request(), length)) {
    this->fail_("Could not write the encryption confirmation", 0);
    return;
  }
  // Driven by the clock, not by a reply. Waiting for an answer that never comes
  // is what burned the first pairing window: the cuff said nothing at all and
  // the retry, hung off the reply handler, never ran.
  this->arm_handshake_retry_();
}

void OmronSession::handle_handshake_write_(WriteOutcome outcome, int raw_status) {
  if (outcome == WriteOutcome::OK)
    return;
  // A cuff that demands an encrypted link before it will even take the command
  // is the one case where security has to be started from this side. It only
  // happens after the plaintext write has been refused, never speculatively.
  if (outcome == WriteOutcome::NEEDS_ENCRYPTION && !this->encryption_requested_) {
    OMRON_LOG_I(TAG,
                "[%s] Session: cuff wants the link encrypted before the command (status=%d); starting security once",
                this->host_->session_address(), raw_status);
    this->handshake_retry_armed_ = false;
    this->encryption_requested_ = true;
    if (!this->host_->session_request_link_encryption()) {
      this->fail_("Could not start link encryption for the handshake", raw_status);
      return;
    }
    this->arm_handshake_retry_();
    return;
  }
  OMRON_LOG_W(TAG, "[%s] Session: the cuff refused the encryption confirmation (status=%d)",
              this->host_->session_address(), raw_status);
}

void OmronSession::handle_handshake_reply_(std::span<const uint8_t> data) {
  const UnlockReply reply = classify_unlock_reply(data);
  if (this->pair_step_ == PairStep::CONFIRM) {
    if (reply != UnlockReply::ENCRYPTION_CONFIRMED) {
      OMRON_LOG_D(TAG, "[%s] Session: no encryption verdict yet (%s); the retry timer will try again",
                  this->host_->session_address(), format_hex_pretty(data.data(), data.size()).c_str());
      return;
    }
    this->handshake_retry_armed_ = false;
    // Logged, never fatal: any 0x82 is reason to carry on, whatever the status
    // byte says.
    OMRON_LOG_I(TAG, "[%s] Session: cuff reports encryption %s", this->host_->session_address(),
                encryption_status_to_string(data.size() > 1 ? data[1] : 0xFF));
    if (!this->pair_armed_) {
      this->pair_step_ = PairStep::NONE;
      this->begin_index_transaction_();
      return;
    }
    this->pair_step_ = PairStep::PROGRAM;
    OMRON_LOG_I(TAG, "[%s] Pairing: registering this node's key in a user slot", this->host_->session_address());
    if (!this->write_unlock_frame_(make_program_key_request(this->config_.bind_key), this->handshake_frame_length_())) {
      this->fail_("Could not write the pairing key", 0);
      return;
    }
    // Driven by the clock, like the confirmation: a cuff that answers this write
    // with silence offers nothing to hang a retry off, and would hold the
    // session until the peer dropped the link. The arming intent rides along, so
    // the retry is another pairing attempt rather than a plain read.
    this->arm_handshake_retry_();
    return;
  }

  this->pair_step_ = PairStep::NONE;
  this->handshake_retry_armed_ = false;
  // Spent either way, because the cuff answered. A refusal is not something
  // asking again would change, and a success has nothing left to ask for.
  this->pair_armed_ = false;
  if (reply != UnlockReply::KEY_PROGRAMMED) {
    OMRON_LOG_W(TAG, "[%s] Cuff refused the key (%s); reading records with the key it already has",
                this->host_->session_address(), unlock_status_to_string(data.size() > 1 ? data[1] : 0xFF));
    this->begin_index_transaction_();
    return;
  }
  OMRON_LOG_I(TAG, "[%s] Pairing: key registered. Later sessions authenticate with it instead of pairing mode.",
              this->host_->session_address());
  // Reads on in the same session: one press of the cuff's button should leave
  // both a usable key and a set of records behind.
  this->begin_index_transaction_();
}

// --- The transfer ---

void OmronSession::begin_index_transaction_() {
  const OmronProfile *profile = this->config_.profile;
  if (profile == nullptr) {
    this->fail_("No profile to open a transfer with", 0);
    return;
  }
  ReadRange index_range;
  if (!build_index_read(this->config_.layout, index_range)) {
    this->fail_("Could not build the profile index read", 0);
    return;
  }
  this->transaction_.reset();
  this->transaction_.clear_read_ranges();

  // The index region sits at the front of the settings region on every profile
  // that has both, so one frame fetches both. Only a profile whose index lives
  // elsewhere still needs a read of its own.
  const uint16_t settings_len = this->settings_read_len_();
  const bool index_inside_settings = profile->settings_read_address != 0 &&
                                     index_range.address == profile->settings_read_address &&
                                     index_range.length <= settings_len;
  if (!index_inside_settings &&
      !this->transaction_.add_read_range(index_range.address, index_range.length, index_range.block_size)) {
    this->fail_("Could not queue the profile index read", 0);
    return;
  }

  // Read from the read base: a dump taken at the write base comes back zeros.
  //
  // The two bases are one buffer under two aliases, each used in one direction:
  // a write at 0x02A4 shows up in the next connection's read at 0x0260, byte
  // for byte.
  if (profile->settings_read_address != 0 &&
      !this->transaction_.add_read_range(profile->settings_read_address, settings_len,
                                         this->config_.layout.transfer_block_size)) {
    OMRON_LOG_W(TAG, "[%s] Could not queue the settings dump; continuing without it", this->host_->session_address());
    if (index_inside_settings &&
        !this->transaction_.add_read_range(index_range.address, index_range.length, index_range.block_size)) {
      this->fail_("Could not queue the profile index read", 0);
      return;
    }
  }

  TransactionUnlock unlock = TransactionUnlock::NONE;
  if (profile->unlock_mode == UnlockMode::CLASSIC_KEY)
    unlock = TransactionUnlock::CUSTOM_KEY;
  else if (profile->unlock_mode == UnlockMode::TOKEN_KEY)
    unlock = TransactionUnlock::TOKEN_KEY;
  (void)this->begin_transaction_(PollPhase::INDEX, unlock);
}

bool OmronSession::begin_transaction_(PollPhase phase, TransactionUnlock unlock) {
  if (unlock == TransactionUnlock::CUSTOM_KEY && !this->config_.bind_key_set) {
    this->fail_("Classic read unlock requires a configured bindkey", 0);
    return false;
  }
  std::array<uint8_t, 4> nonce{};
  if (unlock == TransactionUnlock::TOKEN_KEY && !this->host_->session_random_nonce(nonce)) {
    this->fail_("Could not generate a token nonce", 0);
    return false;
  }
  if (!this->transaction_.begin(unlock, this->config_.bind_key, nonce)) {
    this->fail_("Could not start the memory transaction", 0);
    return false;
  }
  this->poll_phase_ = phase;
  this->active_command_ = {};
  this->wire_ = CommandWireState::IDLE;
  this->frame_assembler_.reset();
  this->update_transaction_phase_();
  this->send_pending_command_();
  return true;
}

void OmronSession::send_pending_command_() {
  if (this->failed_)
    return;
  if (!this->config_.end_session && this->transaction_.finish_without_end()) {
    OMRON_LOG_D(TAG, "[%s] Leaving the transfer open: end opcode suppressed by configuration",
                this->host_->session_address());
    this->handle_transaction_complete_();
    return;
  }
  // Immediately before the frame is taken, not when it was queued: the clock
  // run carries a wall-clock reading and the rest of the write queue sits
  // between the end of the read phase and this line. That gap is short, and
  // still long enough to write a second stale into a device whose whole point
  // here is keeping the right time.
  //
  // Braced because the body vanishes at the firmware's log level, and an if
  // with an empty body warns.
  if (this->transaction_.refresh_pending_write()) {
    OMRON_LOG_V(TAG, "[%s] Rebuilt the pending write with the clock of this moment", this->host_->session_address());
  }
  this->active_command_ = this->transaction_.pending_command();
  if (this->active_command_.kind == CommandKind::NONE || this->active_command_.bytes.empty()) {
    this->fail_("Transaction produced an empty command", 0);
    return;
  }
  // A fresh command starts from nothing outstanding. Defensive rather than
  // load-bearing: every path that reaches here already leaves IDLE behind, so
  // this guards a fourth path being added rather than anything the suite can
  // exercise today.
  this->wire_ = CommandWireState::IDLE;

  const bool unlock_command =
      this->active_command_.kind == CommandKind::KEY_AUTH || this->active_command_.kind == CommandKind::TOKEN;
  // The token goes out unacknowledged by default; everything else is the host's
  // call from the characteristic's own properties.
  const bool prefer_no_response = this->active_command_.kind == CommandKind::TOKEN && !this->token_force_response_;
  const WriteDispatch dispatch =
      this->host_->session_write(unlock_command ? SessionChannel::UNLOCK : SessionChannel::PROTOCOL,
                                 this->active_command_.bytes, prefer_no_response);
  switch (dispatch) {
    case WriteDispatch::FAILED:
      this->fail_("GATT command write failed", 0);
      return;
    case WriteDispatch::AWAITING_RESPONSE:
      this->wire_ = CommandWireState::AWAITING_WRITE_ACK;
      return;
    case WriteDispatch::ON_THE_WIRE:
      this->wire_ = CommandWireState::AWAITING_REPLY;
      this->reply_started_at_ = this->host_->session_now_ms();
      return;
  }
}

void OmronSession::on_write_response(SessionChannel channel, WriteOutcome outcome, int raw_status) {
  if (this->failed_)
    return;
  // The handshake writes outside the transaction's in-flight bookkeeping, so it
  // needs its own branch or its failures land nowhere.
  if (this->pair_step_ != PairStep::NONE && channel == SessionChannel::UNLOCK) {
    this->handle_handshake_write_(outcome, raw_status);
    return;
  }
  // Only the two states that have a write outstanding may consume an
  // acknowledgement. Expressed as a state rather than as separate booleans, so
  // "the reply already came" cannot be true here without a write to attach it
  // to - an invariant booleans leave to the order of the lines that set them.
  const CommandWireState was = this->wire_;
  if (was != CommandWireState::AWAITING_WRITE_ACK && was != CommandWireState::REPLY_EARLY) {
    this->note_unexpected_reply_("write acknowledgement with no write in flight");
    return;
  }
  if (outcome != WriteOutcome::OK) {
    this->wire_ = CommandWireState::IDLE;
    this->fail_("GATT command write response failed", raw_status);
    return;
  }
  if (was == CommandWireState::REPLY_EARLY) {
    this->wire_ = CommandWireState::IDLE;
    this->active_command_ = {};
    this->after_transaction_reply_();
    return;
  }
  this->wire_ = CommandWireState::AWAITING_REPLY;
  this->reply_started_at_ = this->host_->session_now_ms();
}

void OmronSession::on_unlock_notification(std::span<const uint8_t> data) {
  if (this->failed_)
    return;
  if (this->pair_step_ != PairStep::NONE) {
    this->handle_handshake_reply_(data);
    return;
  }
  ProtocolError error;
  if (this->transaction_.state() == TransactionState::KEY_PENDING) {
    error = this->transaction_.accept_key_response(data);
  } else if (this->transaction_.state() == TransactionState::TOKEN_PENDING) {
    error = this->transaction_.accept_token_response(data);
  } else {
    // The unlock channel spoke and no step of this session was listening.
    this->note_unexpected_reply_("unlock notification outside key or token exchange");
    return;
  }
  this->complete_reply_(error, "unlock reply this exchange is not waiting on");
}

// The tail both channels share. Do not inline it back into the two callers: a
// fix applied to one copy and not the other is a bug that hides on one channel.
void OmronSession::complete_reply_(ProtocolError error, const char *stray_what) {
  if (error == ProtocolError::STRAY_FRAME) {
    this->note_unexpected_reply_(stray_what);
    return;
  }
  const bool ack_still_outstanding = this->wire_ == CommandWireState::AWAITING_WRITE_ACK;
  if (error == ProtocolError::DEVICE_REPORTED_ERROR) {
    // Reported, not fatal: this arrives on the end opcode and refers to what
    // the cuff refused, while the records it already sent are good. Losing a
    // session's measurements because a settings write was rejected would be a
    // worse answer than saying so. Seen as 0xE5 whenever a settings block was
    // written; the cuff blinks Err alongside it.
    OMRON_LOG_W(TAG, "[%s] Cuff closed the session with result 0x%02X; readings kept", this->host_->session_address(),
                static_cast<unsigned>(this->transaction_.end_status()));
    if (this->diagnostics_ != nullptr) {
      this->diagnostics_->protocol_failures++;
      this->diagnostics_->last_protocol_error = error;
    }
  } else if (error != ProtocolError::NONE) {
    if (this->diagnostics_ != nullptr) {
      this->diagnostics_->protocol_failures++;
      this->diagnostics_->last_protocol_error = error;
    }
    this->wire_ = CommandWireState::IDLE;
    this->fail_(protocol_error_to_string(error), 0);
    return;
  }
  if (ack_still_outstanding) {
    this->wire_ = CommandWireState::REPLY_EARLY;
    return;
  }
  this->wire_ = CommandWireState::IDLE;
  this->active_command_ = {};
  this->after_transaction_reply_();
}

void OmronSession::on_protocol_notification(uint8_t channel, std::span<const uint8_t> data) {
  if (this->failed_)
    return;
  const AssembleResult result = this->frame_assembler_.add_fragment(channel, data);
  if (result == AssembleResult::INCOMPLETE)
    return;
  if (result == AssembleResult::ERROR) {
    const ProtocolError error = this->frame_assembler_.error();
    this->transaction_.fail(error);
    if (this->diagnostics_ != nullptr) {
      this->diagnostics_->protocol_failures++;
      this->diagnostics_->last_protocol_error = error;
    }
    this->fail_(protocol_error_to_string(error), 0);
    return;
  }
  const auto &frame = this->frame_assembler_.frame();
  const ProtocolError error = this->transaction_.accept_frame(frame);
  this->frame_assembler_.reset();
  // Before the wire state is touched, because this frame answered nothing: the
  // transaction dropped it and stayed where it was, and the reply actually
  // being waited for has not come yet. Falling through here re-sends the
  // command still in flight, on the strength of somebody else's echo.
  this->complete_reply_(error, "protocol frame for an address this transaction is not waiting on");
}

void OmronSession::after_transaction_reply_() {
  this->update_transaction_phase_();
  if (this->transaction_.state() == TransactionState::FAILED) {
    this->fail_(protocol_error_to_string(this->transaction_.error()), 0);
  } else if (this->transaction_.state() == TransactionState::END_PENDING && this->poll_phase_ == PollPhase::INDEX) {
    // Do not let the index transaction end here. The cuff closes the whole
    // session on the end opcode: a start sent afterwards is never answered and
    // the link drops a few seconds later. Append the record reads while the
    // envelope is still open so one end covers both.
    if (!this->build_record_reads_()) {
      this->fail_("Could not build record reads from the index cursor", 0);
    } else {
      this->send_pending_command_();
    }
  } else if (this->transaction_.state() == TransactionState::END_PENDING && this->poll_phase_ == PollPhase::RECORDS &&
             !this->clock_write_attempted_) {
    // Last chance to write before the end opcode closes the session: doing it
    // afterwards would mean a second connection.
    this->clock_write_attempted_ = true;
    // Registration builds every run it needs from one snapshot in one pass, so
    // nothing here competes for the same bytes and no later frame can carry a
    // stale version of what an earlier one changed.
    //
    // The clock write below is only the fallback for a session with no user to
    // mark it with.
    this->maybe_queue_registration_writes_();
    this->maybe_queue_clock_write_();
    this->send_pending_command_();
  } else if (this->transaction_.state() == TransactionState::COMPLETE) {
    this->handle_transaction_complete_();
  } else {
    this->send_pending_command_();
  }
}

void OmronSession::handle_transaction_complete_() {
  if (this->poll_phase_ != PollPhase::RECORDS) {
    this->fail_("Transaction completed in an invalid poll phase", 0);
    return;
  }
  this->record_memory_.clear();
  for (const auto &block : this->transaction_.received_blocks())
    this->record_memory_.add_block(block.address, block.data);
  this->host_->session_transfer_complete();
}

bool OmronSession::build_record_reads_() {
  const OmronProfile *profile = this->config_.profile;
  const PollLayout &layout = this->config_.layout;

  this->index_memory_.clear();
  for (const auto &block : this->transaction_.received_blocks())
    this->index_memory_.add_block(block.address, block.data);
  if (profile->settings_read_address != 0) {
    const std::vector<uint8_t> settings =
        this->index_memory_.read(profile->settings_read_address, this->settings_read_len_());
    if (!settings.empty()) {
      OMRON_LOG_I(TAG, "[%s] Settings block @0x%04X: %s", this->host_->session_address(),
                  static_cast<unsigned>(profile->settings_read_address),
                  format_hex_pretty(settings.data(), settings.size()).c_str());
      this->host_->session_settings_read(settings);
    }
  }

  const std::vector<uint8_t> index_data = this->index_memory_.read(layout.index_address, layout.index_size);
  if (index_data.size() != layout.index_size || !build_record_plan(layout, index_data, this->record_plans_))
    return false;

  // Remember where every ring stands before anything is dropped, so the cursor
  // of a user whose frames we skip is still the cursor we compare against next
  // time.
  for (const auto &user_plan : this->record_plans_) {
    if (user_plan.user < USER_SLOTS) {
      this->staged_cursor_[user_plan.user] = user_plan.raw_cursor;
      this->has_staged_cursor_[user_plan.user] = true;
    }
  }

  // Drop the users whose ring has not moved since the last session that
  // finished. Their entities keep the values they already hold, which is what
  // those values were: the newest record in a ring that has not changed.
  const bool read_everything = this->config_.full_read_on_pairing && this->pairing_advertised_;
  const size_t skipped = std::erase_if(this->record_plans_, [this, read_everything](const UserRecordPlan &plan) {
    return !read_everything && plan.user < USER_SLOTS && this->has_polled_cursor_[plan.user] &&
           this->polled_cursor_[plan.user] == plan.raw_cursor;
  });
  if (skipped != 0)
    OMRON_LOG_D(TAG, "[%s] %u user ring(s) unchanged since the last session; not re-reading them",
                this->host_->session_address(), static_cast<unsigned>(skipped));
  else if (read_everything)
    OMRON_LOG_D(TAG, "[%s] Pairing mode: reading every ring whether or not its cursor moved",
                this->host_->session_address());

  // The clock goes immediately behind the settings frame, before any record.
  // The cuff answers either order, so this is not correctness: the rule is that
  // a session read off the wire should look like every other host's.
  if (layout.clock_size != 0) {
    if (!this->transaction_.extend_reads(layout.clock_address, layout.clock_size, layout.transfer_block_size))
      OMRON_LOG_D(TAG, "[%s] Clock read did not fit in this transfer; skipping it", this->host_->session_address());
  }

  bool extended = false;
  for (const auto &user_plan : this->record_plans_) {
    for (const auto &read : user_plan.reads) {
      if (!this->transaction_.extend_reads(read.address, read.length, layout.transfer_block_size))
        return false;
      extended = true;
    }
  }
  // No record reads left is a failure only when there were none to begin with.
  // Every ring being unchanged is the ordinary case once this node has read the
  // cuff once, and that session still has a clock to read and writes to send.
  if (!extended && skipped == 0)
    return false;

  this->record_memory_.clear();
  this->poll_phase_ = PollPhase::RECORDS;
  OMRON_LOG_D(TAG, "[%s] Index decoded; reading records for %u user(s) in the same transfer",
              this->host_->session_address(), static_cast<unsigned>(this->record_plans_.size()));
  return true;
}

// --- The writes ---

void OmronSession::merge_clock_into_settings_() {
  if (this->config_.profile == nullptr || this->config_.layout.clock_size == 0)
    return;
  // The clock arrives in the records transfer, in its own frame at its own
  // address. Merging it into the settings image is what lets a read of the
  // whole region span both frames, which is what the write builders need.
  for (const auto &block : this->transaction_.received_blocks()) {
    if (block.address == this->config_.layout.clock_address && block.data.size() == this->config_.layout.clock_size)
      this->index_memory_.add_block(block.address, block.data);
  }
}

// Registers this host with the cuff, which is what keeps the bond alive. Skip
// any of it and the cuff discards its half, and the next connection fails with
// auth fail 97 (HCI_ERR_KEY_MISSING), which looks like a key-handling fault and
// is not.
//
// The counter moves only in the session that pairs. Every other session still
// sends the pointer region and the marked clock; stepping the counter on each
// read would grow it without end and spend a write per measurement.
bool OmronSession::maybe_queue_registration_writes_() {
  if (this->config_.profile == nullptr)
    return false;
  if (this->config_.register_as_user == 0 && this->config_.write_birth_date_users == 0)
    return false;

  this->merge_clock_into_settings_();
  const std::vector<uint8_t> settings =
      this->index_memory_.read(this->config_.profile->settings_read_address, this->settings_region_len_());

  SessionSettingsUpdate update{};
  update.user_number = this->config_.register_as_user;
  update.register_block = this->paired_this_session_;

  // Every user whose records were read, not just the registered one: this
  // component publishes both areas, so a counter left standing states something
  // untrue about records already handed over.
  //
  // Not because a standing counter keeps the radio awake - it does not. The
  // cuff falls silent about seventy seconds after a session whatever the
  // counters read.
  for (const auto &plan : this->record_plans_) {
    if (plan.user < sizeof(update.collected_users) * 8)
      update.collected_users |= static_cast<uint8_t>(1U << plan.user);
  }

  // The time travels in whichever run carries the clock window: a write of its
  // own would land on the marker this session just set and undo it.
  OmronDateTime clock{};
  if (this->clock_write_target_(clock))
    update.clock = clock;

  // The date travels in the block, which only the registering session writes.
  // Nothing writes it later; see standalone_birth_dates for the way in.
  if (update.register_block && this->config_.register_as_user >= 1 &&
      this->config_.register_as_user <= OMRON_MAX_USERS) {
    const OmronDateTime &configured = this->config_.birth_dates[this->config_.register_as_user - 1];
    if (configured.year != 0)
      update.birth_date = configured;
  }

  // And the dates yaml allows out without registering. The builder decides
  // whether each one is worth a write: it refuses a block that reads empty or
  // whose checksum does not reproduce, and skips a date already stored, so this
  // costs one EEPROM write once rather than one per session forever.
  for (uint8_t user = 1; user <= OMRON_MAX_USERS; user++) {
    if ((this->config_.write_birth_date_users & (1U << (user - 1))) == 0)
      continue;
    const OmronDateTime &configured = this->config_.birth_dates[user - 1];
    if (configured.year != 0)
      update.standalone_birth_dates[user - 1] = configured;
  }

  std::vector<SettingsWriteFrame> writes;
  const SettingsWriteError error = build_session_settings_writes(*this->config_.profile, update, settings, writes);
  if (error != SettingsWriteError::NONE) {
    OMRON_LOG_W(TAG, "[%s] Registration writes for user %u not built: %s", this->host_->session_address(),
                static_cast<unsigned>(this->config_.register_as_user), settings_write_error_to_string(error));
    return false;
  }

  // A run carrying the clock is rebuilt again when it reaches the wire, so the
  // time on it is the time then rather than the time the read phase ended. The
  // whole set is rebuilt rather than the clock bytes patched, because the block
  // checksum covers them: everything but the clock comes out identical, since
  // it is derived from this session's settings buffer, which does not move.
  //
  // The update is captured whole, by value. Copied field by field, the next
  // field added here goes missing without a word: it reaches the frame and the
  // log and vanishes before the wire, which on the collected-users mask leaves
  // the cuff still being told a measurement is waiting.
  const auto rebuild_run = [this, settings, update](uint16_t address) -> std::vector<uint8_t> {
    SessionSettingsUpdate fresh = update;
    OmronDateTime now{};
    fresh.clock.reset();
    if (this->clock_write_target_(now))
      fresh.clock = now;
    std::vector<SettingsWriteFrame> rebuilt;
    if (build_session_settings_writes(*this->config_.profile, fresh, settings, rebuilt) != SettingsWriteError::NONE)
      return {};
    for (const auto &candidate : rebuilt) {
      if (candidate.address == address)
        return candidate.frame;
    }
    return {};
  };

  for (const auto &write : writes) {
    // Only when a clock is actually going out. Nothing else in these runs ages
    // between queueing and sending, and a plain frame cannot fail to rebuild.
    const uint16_t address = write.address;
    const bool queued =
        update.clock.has_value()
            ? this->transaction_.queue_write(
                  address, [rebuild_run, address]() -> std::vector<uint8_t> { return rebuild_run(address); })
            : this->transaction_.queue_write(address, write.frame);
    if (!queued) {
      OMRON_LOG_W(TAG, "[%s] Registration write at 0x%04X could not be queued in this session",
                  this->host_->session_address(), write.address);
      // True if an earlier run of this set did get queued: a partial
      // registration is still a session with a write in it.
      return this->transaction_.write_queued();
    }
    OMRON_LOG_I(TAG, "[%s] Settings run at 0x%04X: %s", this->host_->session_address(), write.address,
                format_hex_pretty(write.frame.data(), write.frame.size()).c_str());
  }
  this->clock_write_queued_ = true;
  if (update.birth_date.has_value())
    OMRON_LOG_I(TAG, "[%s] Registration carries user %u birth date %04u-%02u-%02u", this->host_->session_address(),
                static_cast<unsigned>(this->config_.register_as_user), static_cast<unsigned>(update.birth_date->year),
                static_cast<unsigned>(update.birth_date->month), static_cast<unsigned>(update.birth_date->day));
  if (update.clock.has_value())
    OMRON_LOG_I(TAG, "[%s] Setting the cuff clock to %04u-%02u-%02u %02u:%02u:%02u in the same run",
                this->host_->session_address(), static_cast<unsigned>(clock.year), static_cast<unsigned>(clock.month),
                static_cast<unsigned>(clock.day), static_cast<unsigned>(clock.hour),
                static_cast<unsigned>(clock.minute), static_cast<unsigned>(clock.second));
  return true;
}

const std::vector<uint8_t> *OmronSession::clock_window_() const {
  for (const auto &block : this->transaction_.received_blocks()) {
    if (block.address == this->config_.layout.clock_address && block.data.size() == this->config_.layout.clock_size)
      return &block.data;
  }
  return nullptr;
}

bool OmronSession::clock_write_target_(OmronDateTime &target) const {
  if (this->config_.profile == nullptr || this->config_.layout.clock_size == 0)
    return false;
  // Nothing to copy from without a time source, and writing the cuff's own
  // clock back to itself would be pointless at best.
  OmronDateTime now{};
  if (!this->host_->session_wall_clock(now))
    return false;

  const std::vector<uint8_t> *window = this->clock_window_();
  if (window == nullptr)
    return false;

  OmronDateTime current{};
  const ClockParseError parse_error = parse_device_clock(*window, this->config_.profile->clock_fields_offset, current);
  // A window whose checksum does not add up is not a window we should be
  // writing back: the sixteen bytes carry settings we would be copying blind.
  if (parse_error != ClockParseError::NONE && parse_error != ClockParseError::INVALID_DATE)
    return false;

  target = now;
  if (parse_error == ClockParseError::NONE) {
    const int64_t drift = civil_seconds(current) - civil_seconds(target);
    // The profile's own figure unless yaml overrode it. Zero refreshes every
    // session, which is what the model measured here wants; six families state
    // 600 s and get that rather than one cuff's behaviour imposed on them.
    const int64_t threshold = this->config_.clock_sync_threshold_set ? this->config_.clock_sync_threshold_s
                                                                     : this->config_.profile->clock_sync_threshold_s;
    if (threshold > 0 && drift <= threshold && drift >= -threshold)
      return false;
  }
  return true;
}

// The clock on its own, for a session with nobody armed to mark it with. When a
// user is armed the marker frame carries the time instead, and this is skipped.
bool OmronSession::maybe_queue_clock_write_() {
  if (this->clock_write_queued_ || this->config_.profile == nullptr)
    return false;

  OmronDateTime target{};
  if (!this->clock_write_target_(target))
    return false;

  const std::vector<uint8_t> *window = this->clock_window_();
  if (window == nullptr)
    return false;

  std::vector<uint8_t> frame;
  uint16_t address = 0;
  const ClockWriteError build_error =
      build_clock_write_request(*this->config_.profile, *window, target, frame, address);
  if (build_error != ClockWriteError::NONE) {
    OMRON_LOG_W(TAG, "[%s] Clock write refused: %s", this->host_->session_address(),
                clock_write_error_to_string(build_error));
    return false;
  }
  if (!this->transaction_.queue_write(address, frame)) {
    OMRON_LOG_W(TAG, "[%s] Clock write could not be queued in this session", this->host_->session_address());
    return false;
  }
  OMRON_LOG_I(TAG, "[%s] Setting the cuff clock to %04u-%02u-%02u %02u:%02u:%02u at 0x%04X",
              this->host_->session_address(), static_cast<unsigned>(target.year), static_cast<unsigned>(target.month),
              static_cast<unsigned>(target.day), static_cast<unsigned>(target.hour),
              static_cast<unsigned>(target.minute), static_cast<unsigned>(target.second), address);
  return true;
}

// --- Timeouts ---

void OmronSession::arm_handshake_retry_() {
  this->handshake_retry_armed_ = true;
  // The moment it was armed, not the deadline: differences of unsigned
  // milliseconds survive the wrap that absolute comparisons do not, and every
  // other timer in this component is written the same way.
  this->handshake_retry_armed_at_ = this->host_->session_now_ms();
}

void OmronSession::tick(uint32_t now) {
  if (this->failed_)
    return;
  // Ahead of the reply timeout, because the handshake runs before there is a
  // transaction to time out and the two never overlap.
  if (this->handshake_retry_armed_ && now - this->handshake_retry_armed_at_ >= PAIR_RETRY_INTERVAL_MS) {
    this->handshake_retry_armed_ = false;
    this->begin_handshake_();
    return;
  }
  // Only a command nobody has answered can time out. A write still waiting for
  // its acknowledgement has the host's own write timeout, and a reply already
  // held for a pending ack is not late - it is early.
  if (this->wire_ != CommandWireState::AWAITING_REPLY || now - this->reply_started_at_ < PROTOCOL_REPLY_TIMEOUT_MS)
    return;
  this->wire_ = CommandWireState::IDLE;
  this->frame_assembler_.reset();
  if (!this->transaction_.retry_pending()) {
    if (this->diagnostics_ != nullptr) {
      this->diagnostics_->protocol_failures++;
      this->diagnostics_->last_protocol_error = this->transaction_.error();
    }
    this->fail_("Memory-protocol reply timed out after all retries", 0);
    return;
  }
  OMRON_LOG_W(TAG, "[%s] Memory-protocol reply timeout; retry %u/%u", this->host_->session_address(),
              static_cast<unsigned>(this->transaction_.attempt() + 1),
              static_cast<unsigned>(OmronTransaction::MAX_ATTEMPTS));
  // The token unlock goes out unacknowledged by default, so a dropped one is
  // indistinguishable from a cuff that ignored it. Retry it as a write request
  // rather than burning all five attempts on the same silent transport.
  if (this->active_command_.kind == CommandKind::TOKEN)
    this->token_force_response_ = true;
  this->active_command_ = {};
  this->send_pending_command_();
}

}  // namespace esphome::omron
