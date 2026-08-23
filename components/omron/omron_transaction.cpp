#include "omron_transaction.h"

#include <utility>

namespace esphome::omron {

namespace {

// A write frame carries the address it lands on, and the caller names that
// address separately. Both have to agree before the frame is queued or kept: a
// frame that disagrees would write where the caller never asked, and then fail
// to match its own acknowledgement.
bool frame_targets(const std::vector<uint8_t> &frame, uint16_t address) {
  if (frame.size() < FRAME_ADDRESS_OFFSET + 2)
    return false;
  const uint16_t framed = static_cast<uint16_t>(static_cast<uint16_t>(frame[FRAME_ADDRESS_OFFSET]) << 8) |
                          static_cast<uint16_t>(frame[FRAME_ADDRESS_OFFSET + 1]);
  return framed == address;
}

}  // namespace

bool OmronTransaction::add_read_range(uint16_t address, uint16_t length, uint8_t block_size) {
  if ((this->state_ != TransactionState::IDLE && !this->finished()) || length == 0 || block_size == 0)
    return false;
  const uint32_t end = static_cast<uint32_t>(address) + length;
  if (end > 0x10000UL)
    return false;
  this->ranges_.push_back({address, length, block_size});
  return true;
}

bool OmronTransaction::extend_reads(uint16_t address, uint16_t length, uint8_t block_size) {
  // Callers append one range at a time, so only the first call resumes reading;
  // the rest just grow the plan behind the current read index.
  const bool resuming = this->state_ == TransactionState::END_PENDING;
  if ((!resuming && this->state_ != TransactionState::READ_PENDING) || length == 0 || block_size == 0)
    return false;
  const uint32_t end = static_cast<uint32_t>(address) + length;
  if (end > 0x10000UL)
    return false;

  const size_t resume_index = this->plan_.size();
  this->append_blocks_(address, length, block_size);
  if (resuming) {
    this->read_index_ = resume_index;
    this->attempt_ = 0;
    this->stray_frames_ = 0;
    this->end_status_ = 0;
    this->state_ = TransactionState::READ_PENDING;
  }
  return true;
}

bool OmronTransaction::queue_write(uint16_t address, const std::vector<uint8_t> &frame) {
  // Same window as extend_reads: mid-session, before the end command goes out.
  const bool resuming = this->state_ == TransactionState::END_PENDING;
  // Queueing the first write out of END_PENDING moves the state to
  // WRITE_PENDING, so a second one arriving right behind it lands here, in a
  // state neither of the two below covers. Without this branch the user block
  // is dropped and only the region ahead of it goes out.
  const bool appending = this->state_ == TransactionState::WRITE_PENDING;
  if ((!resuming && !appending && this->state_ != TransactionState::READ_PENDING) || frame.empty() ||
      this->writes_.size() >= MAX_QUEUED_WRITES)
    return false;

  this->writes_.push_back(QueuedWrite{address, frame, {}});
  if (resuming) {
    this->attempt_ = 0;
    this->stray_frames_ = 0;
    this->end_status_ = 0;
    this->state_ = TransactionState::WRITE_PENDING;
  }
  return true;
}

bool OmronTransaction::queue_write(uint16_t address, WriteFrameBuilder builder) {
  if (!builder)
    return false;
  // Built once here as well, so the queue is never holding a promise it cannot
  // keep: a builder that fails at send time leaves this frame standing, and a
  // builder that fails now means there was never anything to queue.
  std::vector<uint8_t> frame = builder();
  if (!frame_targets(frame, address))
    return false;
  if (!this->queue_write(address, frame))
    return false;
  this->writes_.back().builder = std::move(builder);
  return true;
}

bool OmronTransaction::refresh_pending_write() {
  if (this->state_ != TransactionState::WRITE_PENDING || this->writes_.empty())
    return false;
  QueuedWrite &pending = this->writes_.front();
  if (!pending.builder)
    return false;

  std::vector<uint8_t> rebuilt = pending.builder();
  // The frame already queued stands when the rebuild cannot be trusted; a stale
  // write beats one aimed somewhere else.
  if (!frame_targets(rebuilt, pending.address))
    return false;

  pending.frame = std::move(rebuilt);
  return true;
}

void OmronTransaction::clear_read_ranges() {
  if (this->state_ == TransactionState::IDLE || this->finished())
    this->ranges_.clear();
}

void OmronTransaction::append_blocks_(uint16_t address, uint16_t length, uint8_t block_size) {
  uint16_t remaining = length;
  uint16_t cursor = address;
  while (remaining != 0) {
    const uint8_t chunk = remaining < block_size ? static_cast<uint8_t>(remaining) : block_size;
    this->plan_.push_back({cursor, chunk});
    cursor = static_cast<uint16_t>(cursor + chunk);
    remaining = static_cast<uint16_t>(remaining - chunk);
  }
}

bool OmronTransaction::build_plan_() {
  this->plan_.clear();
  for (const auto &range : this->ranges_)
    this->append_blocks_(range.address, range.length, range.block_size);
  return !this->plan_.empty();
}

bool OmronTransaction::begin(TransactionUnlock unlock, const OmronBindKey &bind_key,
                             const std::array<uint8_t, 4> &token_nonce) {
  if (this->state_ != TransactionState::IDLE && !this->finished())
    return false;
  if (!this->build_plan_())
    return false;

  this->token_nonce_ = token_nonce;
  this->bind_key_ = bind_key;
  this->writes_.clear();
  this->received_blocks_.clear();
  this->read_index_ = 0;
  this->attempt_ = 0;
  this->stray_frames_ = 0;
  this->end_status_ = 0;
  this->error_ = ProtocolError::NONE;
  if (unlock == TransactionUnlock::CUSTOM_KEY)
    this->state_ = TransactionState::KEY_PENDING;
  else if (unlock == TransactionUnlock::TOKEN_KEY)
    this->state_ = TransactionState::TOKEN_PENDING;
  else
    this->state_ = TransactionState::START_PENDING;
  return true;
}

PendingCommand OmronTransaction::pending_command() const {
  PendingCommand command;
  command.attempt = this->attempt_;
  switch (this->state_) {
    case TransactionState::KEY_PENDING: {
      command.kind = CommandKind::KEY_AUTH;
      const auto bytes = make_key_auth_request(this->bind_key_);
      command.bytes.assign(bytes.begin(), bytes.end());
      break;
    }
    case TransactionState::TOKEN_PENDING: {
      command.kind = CommandKind::TOKEN;
      const auto bytes = make_token_request(this->token_nonce_);
      command.bytes.assign(bytes.begin(), bytes.end());
      break;
    }
    case TransactionState::START_PENDING: {
      command.kind = CommandKind::START;
      const auto bytes = make_start_request();
      command.bytes.assign(bytes.begin(), bytes.end());
      break;
    }
    case TransactionState::READ_PENDING: {
      if (this->read_index_ >= this->plan_.size())
        break;
      const ReadBlock &block = this->plan_[this->read_index_];
      command.kind = CommandKind::READ;
      command.address = block.address;
      command.expected_length = block.length;
      const auto bytes = make_read_request(block.address, block.length);
      command.bytes.assign(bytes.begin(), bytes.end());
      break;
    }
    case TransactionState::WRITE_PENDING: {
      if (this->writes_.empty())
        break;
      command.kind = CommandKind::WRITE;
      command.address = this->writes_.front().address;
      command.bytes = this->writes_.front().frame;
      break;
    }
    case TransactionState::END_PENDING: {
      command.kind = CommandKind::END;
      const auto bytes = make_end_request();
      command.bytes.assign(bytes.begin(), bytes.end());
      break;
    }
    default:
      break;
  }
  return command;
}

ProtocolError OmronTransaction::accept_key_response(std::span<const uint8_t> data) {
  if (this->state_ != TransactionState::KEY_PENDING)
    return ProtocolError::UNEXPECTED_COMMAND;
  const UnlockReply reply = classify_unlock_reply(data);
  if (reply != UnlockReply::KEY_ACCEPTED) {
    this->fail(reply == UnlockReply::KEY_REJECTED ? ProtocolError::UNLOCK_KEY_REJECTED
                                                  : ProtocolError::INVALID_UNLOCK_RESPONSE);
    return this->error_;
  }
  this->attempt_ = 0;
  this->state_ = TransactionState::START_PENDING;
  return ProtocolError::NONE;
}

ProtocolError OmronTransaction::accept_token_response(std::span<const uint8_t> data) {
  if (this->state_ != TransactionState::TOKEN_PENDING)
    return ProtocolError::UNEXPECTED_COMMAND;
  const ProtocolError error = validate_token_response(data, this->token_nonce_);
  if (error != ProtocolError::NONE) {
    this->fail(error);
    return error;
  }
  this->attempt_ = 0;
  this->state_ = TransactionState::START_PENDING;
  return ProtocolError::NONE;
}

ProtocolError OmronTransaction::accept_frame(std::span<const uint8_t> frame) {
  ResponseFrame response;
  const ProtocolError parse_error = parse_response(frame, response);
  if (parse_error != ProtocolError::NONE) {
    // STRAY_FRAME, for the same reason the address mismatch below answers that
    // way: nothing was consumed, so a caller told NONE would retire the command
    // still in flight and skip the block it was waiting for.
    if (++this->stray_frames_ <= MAX_STRAY_FRAMES)
      return ProtocolError::STRAY_FRAME;
    this->fail(parse_error);
    return parse_error;
  }

  switch (this->state_) {
    case TransactionState::START_PENDING:
      if (response.type != PacketType::START_RESPONSE) {
        this->fail(ProtocolError::UNEXPECTED_COMMAND);
        return this->error_;
      }
      this->advance_after_start_();
      return ProtocolError::NONE;

    case TransactionState::READ_PENDING: {
      if (response.type != PacketType::READ_RESPONSE || this->read_index_ >= this->plan_.size()) {
        this->fail(ProtocolError::UNEXPECTED_COMMAND);
        return this->error_;
      }
      const ReadBlock &expected = this->plan_[this->read_index_];
      if (response.address != expected.address) {
        // Late or duplicated reply for a block we already consumed. Drop it and
        // stay in this state instead of failing the whole poll. Answered as
        // STRAY_FRAME rather than NONE so the caller cannot read "stayed put"
        // as "carry on" - it did, and re-sent the command still in flight.
        if (++this->stray_frames_ <= MAX_STRAY_FRAMES)
          return ProtocolError::STRAY_FRAME;
        this->fail(ProtocolError::UNEXPECTED_COMMAND);
        return this->error_;
      }
      if (response.data.size() != expected.length) {
        this->fail(ProtocolError::PAYLOAD_LENGTH_MISMATCH);
        return this->error_;
      }
      this->received_blocks_.push_back({response.address, std::move(response.data)});
      this->advance_after_read_();
      return ProtocolError::NONE;
    }

    case TransactionState::WRITE_PENDING: {
      // The acknowledgement carries the address the write landed on. Accepting
      // one for a different address would report success for a write that went
      // somewhere else entirely.
      if (response.type != PacketType::WRITE_RESPONSE || this->writes_.empty() ||
          response.address != this->writes_.front().address) {
        if (++this->stray_frames_ <= MAX_STRAY_FRAMES)
          return ProtocolError::STRAY_FRAME;
        this->fail(ProtocolError::UNEXPECTED_COMMAND);
        return this->error_;
      }
      this->writes_.erase(this->writes_.begin());
      this->attempt_ = 0;
      this->stray_frames_ = 0;
      // Anything still queued goes out on this same connection: the end command
      // closes the session for everyone behind it.
      this->state_ = this->writes_.empty() ? TransactionState::END_PENDING : TransactionState::WRITE_PENDING;
      return ProtocolError::NONE;
    }

    case TransactionState::END_PENDING:
      if (response.type != PacketType::END_RESPONSE) {
        this->fail(ProtocolError::UNEXPECTED_COMMAND);
        return this->error_;
      }
      // A result code here is about what the cuff refused, not about the
      // transfer: every read was acknowledged individually and the records are
      // already in hand. Failing the session would throw away good
      // measurements because a settings write was rejected.
      this->end_status_ = response.status;
      this->attempt_ = 0;
      this->state_ = TransactionState::COMPLETE;
      return response.status == 0 ? ProtocolError::NONE : ProtocolError::DEVICE_REPORTED_ERROR;

    default:
      return ProtocolError::UNEXPECTED_COMMAND;
  }
}

void OmronTransaction::advance_after_start_() {
  this->attempt_ = 0;
  this->stray_frames_ = 0;
  this->read_index_ = 0;
  if (!this->plan_.empty())
    this->state_ = TransactionState::READ_PENDING;
  else
    this->state_ = !this->writes_.empty() ? TransactionState::WRITE_PENDING : TransactionState::END_PENDING;
}

void OmronTransaction::advance_after_read_() {
  this->attempt_ = 0;
  this->stray_frames_ = 0;
  this->read_index_++;
  if (this->read_index_ < this->plan_.size())
    this->state_ = TransactionState::READ_PENDING;
  else
    this->state_ = !this->writes_.empty() ? TransactionState::WRITE_PENDING : TransactionState::END_PENDING;
}

bool OmronTransaction::finish_without_end() {
  if (this->state_ != TransactionState::END_PENDING)
    return false;
  this->state_ = TransactionState::COMPLETE;
  return true;
}

bool OmronTransaction::retry_pending() {
  if (this->state_ == TransactionState::IDLE || this->finished())
    return false;
  this->attempt_++;
  if (this->attempt_ >= MAX_ATTEMPTS) {
    this->fail(ProtocolError::RETRY_EXHAUSTED);
    return false;
  }
  return true;
}

void OmronTransaction::fail(ProtocolError error) {
  this->error_ = error;
  this->state_ = TransactionState::FAILED;
}

void OmronTransaction::reset() {
  this->plan_.clear();
  this->received_blocks_.clear();
  this->writes_.clear();
  this->read_index_ = 0;
  this->attempt_ = 0;
  this->stray_frames_ = 0;
  this->end_status_ = 0;
  this->state_ = TransactionState::IDLE;
  this->error_ = ProtocolError::NONE;
}

}  // namespace esphome::omron
