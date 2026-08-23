#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "omron_diagnostics.h"
#include "omron_measurement.h"
#include "omron_memory.h"
#include "omron_poll_plan.h"
#include "omron_profiles.h"
#include "omron_protocol.h"
#include "omron_settings_write.h"
#include "omron_transaction.h"
#include "omron_unlock.h"

namespace esphome::omron {

// Everything between "the notification channels are up" and "the end opcode has
// been answered": the handshake, the transfer envelope, what is read and in
// which order, what is written before it closes, and which replies mean what.
//
// Deliberately free of ESP-IDF. The shape of a session is not the behaviour of
// any one function in it, so unit tests over the parts miss a frame sent in the
// wrong order. Keep transport on the far side of the seam: handles, channel
// splitting, write types, bonding, discovery.
class OmronSessionHost;

// Which characteristic a frame belongs on. The session knows this because it
// knows what it is sending; the handle behind it is the host's business.
enum class SessionChannel : uint8_t {
  PROTOCOL = 0,
  UNLOCK,
};

// A write's result reduced to what the session branches on. The raw status
// still travels alongside for the log, but keeping esp_gatt_status_t out of
// here is what lets this file compile without ESP-IDF.
enum class WriteOutcome : uint8_t {
  OK = 0,
  // The cuff refused the frame in the clear and wants the link encrypted first.
  // Exactly one family does this, and only ever after the plaintext attempt.
  NEEDS_ENCRYPTION,
  FAILED,
};

// What became of a command handed to the host. An unacknowledged write is
// already gone by the time the call returns, an acknowledged one reports later;
// saying which avoids the host having to call back into a session that is still
// inside the call that started the write.
enum class WriteDispatch : uint8_t {
  FAILED = 0,
  ON_THE_WIRE,
  AWAITING_RESPONSE,
};

// Where the one command this session has outstanding currently is.
//
// One field rather than three booleans, because the invariant that matters -
// reply-early only ever happens while a write is in flight - is then held up by
// the order of the lines that set them and by nothing a compiler can check.
enum class CommandWireState : uint8_t {
  // No command outstanding.
  IDLE = 0,
  // Written with a response requested; the ATT acknowledgement has not arrived.
  // The reply cannot be acted on yet, because the write it answers is not
  // finished as far as the link is concerned.
  AWAITING_WRITE_ACK,
  // On the wire and answered by nobody yet. The only state the reply timeout
  // applies to.
  AWAITING_REPLY,
  // The cuff answered while the write acknowledgement was still outstanding.
  // Not hypothetical: the notification and the acknowledgement race, and the
  // notification wins often enough to matter. The reply is held here until the
  // acknowledgement lands.
  REPLY_EARLY,
};

class OmronSessionHost {
 public:
  virtual ~OmronSessionHost() = default;

  // Hands a whole command to the link; channel splitting, write type and
  // response matching are the host's problem. The return says whether to expect
  // on_write_response or treat the bytes as already gone.
  virtual WriteDispatch session_write(SessionChannel channel, std::span<const uint8_t> data,
                                      bool prefer_no_response) = 0;

  // Milliseconds since boot. Only ever differenced.
  virtual uint32_t session_now_ms() = 0;

  // Local wall clock, for the cuff's own clock field. False without a time
  // source, and then the clock is read and published but never set.
  virtual bool session_wall_clock(OmronDateTime &now) = 0;

  // Separate from the rest so a test can make a session's first frame
  // deterministic.
  virtual bool session_random_nonce(std::span<uint8_t> data) = 0;

  // For the one case where the cuff refuses the handshake in the clear. Never
  // called speculatively.
  virtual bool session_request_link_encryption() = 0;

  // The settings image, once, as soon as it has been read.
  virtual void session_settings_read(const std::vector<uint8_t> &settings) = 0;

  // Records are in and the memory image is complete. Decoding and publishing
  // belong to the host, which owns the entities.
  virtual void session_transfer_complete() = 0;

  virtual void session_failed(const char *reason, int code) = 0;
  virtual const char *session_address() = 0;
};

// Settled at setup or by yaml, and passed by value so a session cannot reach
// back into the client for something not declared here.
struct OmronSessionConfig {
  const OmronProfile *profile{nullptr};
  PollLayout layout{};
  OmronBindKey bind_key{};
  bool bind_key_set{false};
  // Whether to close the transfer with the end opcode.
  bool end_session{true};
  // One-based, zero for off. Registering this node with the cuff is what stops
  // the cuff discarding the bond.
  uint8_t register_as_user{0};
  // Unset means "whatever the profile says".
  bool clock_sync_threshold_set{false};
  int64_t clock_sync_threshold_s{0};
  // Year zero means no date configured for that user.
  std::array<OmronDateTime, OMRON_MAX_USERS> birth_dates{};
  // Bit 0 is user 1. Whose date may go out without registering as them: a cuff
  // holds two people and a node registers as one, so the other's date otherwise
  // needs a second pairing to get in.
  uint8_t write_birth_date_users{0};
  // Keyed on the advertisement's pairing bit, not on the session registering.
  bool full_read_on_pairing{false};
};

class OmronSession {
 public:
  // As many people as PollLayout can plan for, which is more than any profile in
  // the catalog stores. Sized from the layout rather than from the entity block
  // so the two can never disagree about which user a cursor belongs to.
  static constexpr size_t USER_SLOTS = 4;

  OmronSession() = default;
  // A copy taken by accident would answer for frames it never sent. The config
  // stays a value type on purpose; that one is meant to be copied.
  OmronSession(const OmronSession &) = delete;
  OmronSession &operator=(const OmronSession &) = delete;

  void set_host(OmronSessionHost *host) { this->host_ = host; }
  void set_diagnostics(OmronDiagnostics *diagnostics) { this->diagnostics_ = diagnostics; }
  void configure(const OmronSessionConfig &config);

  // Clears every per-connection latch. Deliberately not the pairing arm or the
  // cursor watermarks: the first is an intent that has to outlive a cuff being
  // out of range, the second is what a later session compares against.
  void reset();

  // What discovery found, which need not be what the profile declares. Frames
  // are reassembled across exactly these, so it is set at discovery rather than
  // at begin(): a notification can arrive during the settle window between.
  void set_rx_channel_count(uint8_t count);

  // The notification channels are up, which is where a session begins. The flag
  // says whether the cuff exposed an unlock characteristic at all; without one
  // there is no handshake and the transfer starts straight away.
  void begin(bool unlock_channel_available);

  // Reply timeouts, and the handshake retry. Called from the host's loop.
  //
  // The handshake retries on a clock rather than on a reply, because the cuff
  // can answer the first attempt with silence and a retry hung off the reply
  // handler then never runs.
  void tick(uint32_t now);

  // The host failed the session for a reason of its own. Idempotent, and it
  // stops the session touching the wire again.
  void abort();

  void on_protocol_notification(uint8_t channel, std::span<const uint8_t> data);
  void on_unlock_notification(std::span<const uint8_t> data);
  void on_write_response(SessionChannel channel, WriteOutcome outcome, int raw_status);

  // Pairing intent survives reset(): a button can arm a session before the cuff
  // is reachable, and that has to keep until a session can spend it.
  void arm_pairing() { this->pair_armed_ = true; }
  bool pairing_armed() const { return this->pair_armed_; }
  // Set by the host when this connection was the one that bonded. The version
  // counter is stepped only in that session.
  void set_paired_this_session(bool paired) { this->paired_this_session_ = paired; }

  // Has to be set before the transfer plans records.
  void set_pairing_advertised(bool advertised) { this->pairing_advertised_ = advertised; }

  // Commits the per-user cursor watermarks on success, drops them otherwise:
  // only a session that finished cleanly may claim it collected a ring.
  void finish(bool success);

  const OmronMemoryImage &record_memory() const { return this->record_memory_; }
  const std::vector<UserRecordPlan> &record_plans() const { return this->record_plans_; }

  bool failed() const { return this->failed_; }
  bool waiting_for_reply() const { return this->wire_ == CommandWireState::AWAITING_REPLY; }
  CommandWireState wire_state() const { return this->wire_; }
  // Whether the cuff is still holding a memory session open, which is what
  // decides if the teardown owes it an end opcode.
  bool transfer_open() const;

 private:
  enum class PollPhase : uint8_t { NONE = 0, INDEX, RECORDS };
  enum class PairStep : uint8_t { NONE = 0, CONFIRM, PROGRAM };

  void begin_handshake_();
  void arm_handshake_retry_();
  void handle_handshake_reply_(std::span<const uint8_t> data);
  void handle_handshake_write_(WriteOutcome outcome, int raw_status);
  uint8_t handshake_budget_() const;
  size_t handshake_frame_length_() const;
  bool write_unlock_frame_(const OmronUnlockFrame &frame, size_t length);

  void begin_index_transaction_();
  bool begin_transaction_(PollPhase phase, TransactionUnlock unlock);
  void send_pending_command_();
  void after_transaction_reply_();
  void handle_transaction_complete_();
  bool build_record_reads_();
  void update_transaction_phase_();
  void fail_(const char *reason, int code = 0);
  // A reply the session had no state to receive. Not fatal, since failing over
  // a late answer would throw away readings already in hand, but never silent:
  // a dropped frame nobody counts costs a debugging session.
  void note_unexpected_reply_(const char *what);
  // What both notification handlers do with a classified reply: drop a stray
  // one, keep the readings when the cuff refuses a write, fail on anything
  // else, and otherwise hand over to the next step, holding the reply if the
  // write acknowledgement is still outstanding. `stray_what` names the channel
  // that could not place the frame and is the only difference between callers.
  void complete_reply_(ProtocolError error, const char *stray_what);

  void merge_clock_into_settings_();
  bool maybe_queue_registration_writes_();
  bool maybe_queue_clock_write_();
  bool clock_write_target_(OmronDateTime &target) const;
  const std::vector<uint8_t> *clock_window_() const;

  // Everything up to the clock block, which is read separately at its own
  // address: 44 bytes then 16 on the cuff here.
  uint16_t settings_read_len_() const;
  // The whole region including the clock, assembled from those two reads rather
  // than fetched a third time. A registration run can span block and clock.
  uint16_t settings_region_len_() const;

  // For a profile whose settings geometry is unknown; anything with a time
  // region uses the two helpers above.
  static constexpr uint16_t SETTINGS_DUMP_LEN = 0x40;
  // Ten tries a second apart, which is how long the BLE pairing underneath can
  // take to finish before the cuff will answer at all.
  static constexpr uint8_t PAIR_MAX_ATTEMPTS = 10;
  // Far fewer for an ordinary poll: nobody is standing at the cuff holding a
  // button, so a silent one should fall through to the read quickly.
  static constexpr uint8_t CONFIRM_MAX_ATTEMPTS = 3;
  static constexpr uint32_t PAIR_RETRY_INTERVAL_MS = 1000;
  // The token handshake is twenty bytes wide, the key commands seventeen. Half
  // the attempts go out at each width.
  static constexpr size_t PAIR_MAX_FRAME_LEN = 20;
  static constexpr uint32_t PROTOCOL_REPLY_TIMEOUT_MS = 3500;

  OmronSessionHost *host_{nullptr};
  OmronDiagnostics *diagnostics_{nullptr};
  OmronSessionConfig config_{};

  OmronTransaction transaction_{};
  OmronFrameAssembler frame_assembler_{};
  OmronMemoryImage index_memory_{};
  OmronMemoryImage record_memory_{};
  std::vector<UserRecordPlan> record_plans_{};
  PollPhase poll_phase_{PollPhase::NONE};

  PendingCommand active_command_{};
  CommandWireState wire_{CommandWireState::IDLE};
  uint32_t reply_started_at_{0};
  bool token_force_response_{false};
  bool failed_{false};

  PairStep pair_step_{PairStep::NONE};
  uint8_t pair_attempt_{0};
  bool pair_armed_{false};
  bool paired_this_session_{false};
  bool pairing_advertised_{false};
  bool encryption_requested_{false};
  bool unlock_channel_available_{false};
  // The handshake retry, on the same clock tick() is given.
  bool handshake_retry_armed_{false};
  uint32_t handshake_retry_armed_at_{0};

  bool clock_write_attempted_{false};
  // Set once the registration runs have gone out, which include the clock. The
  // bare clock write is only a fallback for a session with no user to mark, and
  // must not follow them: it lands on the same bytes and undoes the marker.
  bool clock_write_queued_{false};

  // Where each user's write cursor stood at the last successful session. A
  // cursor that has not moved means that ring holds nothing new, so its record
  // frames are not worth sending. Committed only when a session ends cleanly.
  //
  // Deliberately not persisted, however much it looks like it should be: the
  // one moment it would be read back from NVS is the first session after a
  // boot, and that is the one session that must not skip. Entities come up
  // empty and this read is what fills them. Persisting it would also mean an
  // NVS write per measurement, to save the second and a half a full ring costs.
  std::array<uint32_t, USER_SLOTS> polled_cursor_{};
  std::array<bool, USER_SLOTS> has_polled_cursor_{};
  std::array<uint32_t, USER_SLOTS> staged_cursor_{};
  std::array<bool, USER_SLOTS> has_staged_cursor_{};
};

}  // namespace esphome::omron
