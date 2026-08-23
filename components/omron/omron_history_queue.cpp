#include "omron_history_queue.h"

#include <algorithm>
#include <functional>

namespace esphome::omron {

bool OmronHistoryQueue::push(const HistoryEvent &event) {
  if (this->events_.size() >= CAPACITY)
    return false;
  this->events_.push_back(event);
  return true;
}

void OmronHistoryQueue::note_watermark(uint8_t user_index, int64_t epoch) {
  if (user_index >= USER_SLOTS)
    return;
  // Only ever forwards. A session that read less deeply than an earlier one must
  // not walk the mark back and resend what has already gone.
  if (epoch <= this->watermark_[user_index])
    return;
  this->watermark_[user_index] = epoch;
  this->watermark_dirty_[user_index] = true;
}

bool OmronHistoryQueue::watermark_pending() const {
  return std::ranges::any_of(this->watermark_dirty_, std::identity{});
}

void OmronHistoryQueue::flush_watermarks_() {
  for (uint8_t user = 0; user < USER_SLOTS; user++) {
    if (!this->watermark_dirty_[user])
      continue;
    this->watermark_dirty_[user] = false;
    this->host_->history_save_watermark(user, this->watermark_[user]);
  }
}

bool OmronHistoryQueue::tick(uint32_t now) {
  if (this->host_ == nullptr)
    return false;

  if (this->events_.empty()) {
    // The whole point of this class. Everything queued has gone out, so the mark
    // may become durable; doing it any earlier would record events as sent while
    // they were still waiting, and a reboot in that window loses them with no
    // symptom at all.
    this->flush_watermarks_();
    return false;
  }

  if (this->last_emit_ms_ != 0 && now - this->last_emit_ms_ < EMIT_INTERVAL_MS)
    return true;
  this->last_emit_ms_ = now;

  // Front, not back: the ring is read oldest first and the events carry their
  // own timestamps, but a consumer appending them in arrival order deserves them
  // in the order they happened.
  const HistoryEvent event = this->events_.front();
  this->events_.erase(this->events_.begin());
  this->host_->history_emit(event);
  return !this->events_.empty() || this->watermark_pending();
}

}  // namespace esphome::omron
