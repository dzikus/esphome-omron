#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "omron_protocol.h"
#include "omron_unlock.h"

namespace esphome::omron {

// Transport-independent transaction engine: the reads a session plans, the
// writes it queues behind them, and the one command outstanding at any moment.
// It composes no frame of its own, so a write can only go where the caller
// built it to go.

// Produces a write frame at the moment it is needed. Returning an empty vector
// means "nothing to send now", and leaves whatever was queued in place.
using WriteFrameBuilder = std::function<std::vector<uint8_t>()>;

enum class TransactionState : uint8_t {
  IDLE = 0,
  KEY_PENDING,
  TOKEN_PENDING,
  START_PENDING,
  READ_PENDING,
  WRITE_PENDING,
  END_PENDING,
  COMPLETE,
  FAILED,
};

enum class CommandKind : uint8_t {
  NONE = 0,
  KEY_AUTH,
  TOKEN,
  START,
  READ,
  WRITE,
  END,
};

struct ReadRange {
  uint16_t address{0};
  uint16_t length{0};
  uint8_t block_size{0};
};

struct ReadBlock {
  uint16_t address{0};
  uint8_t length{0};
};

struct ReceivedBlock {
  uint16_t address{0};
  std::vector<uint8_t> data{};
};

struct PendingCommand {
  CommandKind kind{CommandKind::NONE};
  std::vector<uint8_t> bytes{};
  uint16_t address{0};
  uint8_t expected_length{0};
  uint8_t attempt{0};
};

enum class TransactionUnlock : uint8_t {
  NONE = 0,
  CUSTOM_KEY,
  TOKEN_KEY,
};

class OmronTransaction {
 public:
  // [[nodiscard]] on everything that answers whether a command was accepted.
  // A dropped answer here is a read or a write the caller believes it queued
  // and the transaction never took, which on the write side means a frame that
  // silently never reaches the cuff.
  [[nodiscard]] bool add_read_range(uint16_t address, uint16_t length, uint8_t block_size);
  // Appends reads once the planned blocks are done but the end command has not
  // gone out. The end opcode closes the whole session: after it a fresh start
  // is never answered and the link drops a few seconds later, so index and
  // record reads have to share one envelope.
  [[nodiscard]] bool extend_reads(uint16_t address, uint16_t length, uint8_t block_size);
  // Queues a write for after the reads and before the end command, for the same
  // reason. Several fit because registering a user takes two runs. The frame
  // must already be built: this engine composes nothing, so it cannot be talked
  // into writing to an address of someone else's choosing.
  [[nodiscard]] bool queue_write(uint16_t address, const std::vector<uint8_t> &frame);
  // The same for a frame that goes stale between queueing and sending, the wall
  // clock in practice. The builder runs again just before the write leaves. A
  // builder returning a frame for a different address is refused.
  [[nodiscard]] bool queue_write(uint16_t address, WriteFrameBuilder builder);
  // Rebuilds the frame at the head of the queue. False when there is nothing to
  // rebuild or the rebuild produced something unusable, and then the queued
  // frame stands: a stale write beats no write.
  [[nodiscard]] bool refresh_pending_write();
  bool write_queued() const { return !this->writes_.empty(); }
  void clear_read_ranges();

  [[nodiscard]] bool begin(TransactionUnlock unlock, const OmronBindKey &bind_key,
                           const std::array<uint8_t, 4> &token_nonce);
  PendingCommand pending_command() const;
  ProtocolError accept_key_response(std::span<const uint8_t> data);
  ProtocolError accept_token_response(std::span<const uint8_t> data);
  ProtocolError accept_frame(std::span<const uint8_t> frame);

  // Finishes without the end opcode. A diagnostic lever: 0x0F closes the memory
  // session and nothing beyond it, so skipping it buys nothing but a cuff left
  // blinking Err. Returns false unless the transaction is actually waiting to
  // send it, so a caller cannot cut a transfer short.
  [[nodiscard]] bool finish_without_end();
  [[nodiscard]] bool retry_pending();
  void fail(ProtocolError error);
  void reset();

  // Attempts one command gets before the transaction gives up. Public because
  // the caller announces the retry it is about to make, and a second copy of
  // the number in a log line goes stale without anything noticing.
  static constexpr uint8_t MAX_ATTEMPTS = 5;

  TransactionState state() const { return this->state_; }
  ProtocolError error() const { return this->error_; }
  bool finished() const {
    return this->state_ == TransactionState::COMPLETE || this->state_ == TransactionState::FAILED;
  }
  uint8_t attempt() const { return this->attempt_; }
  // Non-zero when the cuff refused something at the end opcode. The reads are
  // still good, so this is reported rather than allowed to discard them.
  uint8_t end_status() const { return this->end_status_; }
  const std::vector<ReadBlock> &plan() const { return this->plan_; }
  const std::vector<ReceivedBlock> &received_blocks() const { return this->received_blocks_; }

 private:
  void append_blocks_(uint16_t address, uint16_t length, uint8_t block_size);
  bool build_plan_();
  void advance_after_start_();
  void advance_after_read_();

  // A frame that does not parse, or a reply for an address no longer waited on,
  // is stray rather than a protocol violation. The cap stops a peer that only
  // emits garbage from spinning until the session watchdog fires. Counted
  // against the command in flight: anything the transaction accepts as progress
  // clears it, the same way the attempt counter is cleared.
  static constexpr uint8_t MAX_STRAY_FRAMES = 8;

  std::vector<ReadRange> ranges_{};
  std::vector<ReadBlock> plan_{};
  std::vector<ReceivedBlock> received_blocks_{};
  struct QueuedWrite {
    uint16_t address;
    std::vector<uint8_t> frame;
    // Empty for an already final frame, which is every write but the clock.
    WriteFrameBuilder builder{};
  };
  // Registering a user needs two. The cap stops a caller growing this without
  // bound inside one session.
  static constexpr size_t MAX_QUEUED_WRITES = 4;
  std::vector<QueuedWrite> writes_{};
  std::array<uint8_t, 4> token_nonce_{};
  OmronBindKey bind_key_{};
  size_t read_index_{0};
  uint8_t attempt_{0};
  uint8_t stray_frames_{0};
  uint8_t end_status_{0};
  TransactionState state_{TransactionState::IDLE};
  ProtocolError error_{ProtocolError::NONE};
};

}  // namespace esphome::omron
