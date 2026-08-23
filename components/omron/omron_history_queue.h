#pragma once

// Older readings on their way out of the node, one at a time. A session can
// lift a whole ring at once and each record leaves as its own Home Assistant
// event, which would bury the API connection in a single loop pass.
//
// The invariant: the watermark lives in flash so a reboot does not resend
// everything, and it may only be saved once the queue is empty. Saved with
// events still waiting it records them as sent, and a reboot in that window
// drops them with no symptom at all.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "omron_measurement.h"

namespace esphome::omron {

struct HistoryEvent {
  OmronMeasurement measurement{};
  uint8_t user_index{0};
  uint16_t slot{0};
};

class OmronHistoryQueueHost {
 public:
  virtual ~OmronHistoryQueueHost() = default;
  virtual uint32_t history_now_ms() = 0;
  virtual void history_emit(const HistoryEvent &event) = 0;
  // Called only with an empty queue. See the invariant above.
  virtual void history_save_watermark(uint8_t user_index, int64_t epoch) = 0;
};

class OmronHistoryQueue {
 public:
  void set_host(OmronHistoryQueueHost *host) { this->host_ = host; }

  // A session asks before it decodes, so it can stop reading rather than build
  // events it would have to drop.
  size_t room() const { return this->events_.size() >= CAPACITY ? 0 : CAPACITY - this->events_.size(); }
  bool push(const HistoryEvent &event);

  // Held, not saved: it becomes durable only once everything queued before it
  // has gone out.
  void note_watermark(uint8_t user_index, int64_t epoch);
  // Restore a mark that is already durable. Separate from note_watermark
  // because it must NOT be marked dirty: it is already in flash, and treating
  // it as new would spend a write on every boot.
  void seed_watermark(uint8_t user_index, int64_t epoch) {
    if (user_index < USER_SLOTS)
      this->watermark_[user_index] = epoch;
  }

  // Drains at most one event per interval. True while there is still work, so
  // the caller keeps its loop awake.
  bool tick(uint32_t now);

  size_t size() const { return this->events_.size(); }
  bool watermark_pending() const;
  // What a session compares records against to decide which are new.
  int64_t watermark(uint8_t user_index) const { return user_index < USER_SLOTS ? this->watermark_[user_index] : 0; }

  // Spread enough that a full ring is not one burst, short enough that sixty
  // events take seven seconds rather than a minute.
  static constexpr uint32_t EMIT_INTERVAL_MS = 120;
  // Two full rings plus headroom, so a queue nobody drains cannot grow without
  // bound while Home Assistant is away.
  static constexpr size_t CAPACITY = 128;
  static constexpr uint8_t USER_SLOTS = OMRON_MAX_USERS;

 private:
  void flush_watermarks_();

  OmronHistoryQueueHost *host_{nullptr};
  std::vector<HistoryEvent> events_{};
  std::array<int64_t, USER_SLOTS> watermark_{};
  std::array<bool, USER_SLOTS> watermark_dirty_{};
  uint32_t last_emit_ms_{0};
};

}  // namespace esphome::omron
