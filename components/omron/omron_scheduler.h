#pragma once

#include <cstdint>

namespace esphome::omron {

struct PollPolicy {
  uint32_t normal_interval_ms{300000};
  uint32_t minimum_gap_ms{15000};
  uint32_t advertisement_freshness_ms{10000};
  uint32_t initial_retry_ms{5000};
  uint32_t maximum_retry_ms{300000};
  // Breathing room before re-establishing a link the peer dropped mid-bonding.
  // Reconnecting the instant the teardown appears starts the next pairing while
  // the controller is still freeing the previous one.
  uint32_t retry_gap_ms{500};
  // The cuff advertises for a few seconds after a session ends. Those frames
  // are the tail of the burst just served, and counting them keeps the silence
  // clock alive, so the next button press finds no gap to open an invitation
  // with and the burst goes unanswered.
  uint32_t poll_tail_settle_ms{5000};
};

class OmronPollScheduler {
 public:
  explicit OmronPollScheduler(PollPolicy policy = {}) : policy_(policy) {}

  // `demands_session` is the pairing, forced-transfer or invalid-clock bit, and
  // it overrides the silence window: without it a cuff already awake and
  // blinking -P- is refused for as long as it keeps advertising.
  //
  // The per-user sequence numbers are deliberately not an input. A frame after a
  // short press is bit for bit the cuff's idle chatter, so gating on them makes
  // the press do nothing, which is most of the times somebody presses it.
  void note_advertisement(uint32_t now_ms, bool demands_session = false);
  void note_poll_started(uint32_t now_ms);
  void note_poll_finished(uint32_t now_ms, bool success);
  void request_poll();
  // A continuation of the poll that just dropped, so it is exempt from the
  // minimum gap: waiting that out lands the retry after the cuff has gone back
  // to sleep.
  void request_retry(uint32_t now_ms);
  void clear_request();

  bool advertisement_is_fresh(uint32_t now_ms) const;
  bool should_poll(uint32_t now_ms, bool busy, bool cleanup_pending) const;
  uint32_t retry_delay_ms() const { return this->retry_delay_ms_; }

 private:
  static bool elapsed_(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms);

  PollPolicy policy_{};
  uint32_t last_advertisement_ms_{0};
  uint32_t last_poll_started_ms_{0};
  uint32_t last_poll_finished_ms_{0};
  uint32_t scheduled_delay_ms_{0};
  uint32_t retry_delay_ms_{0};
  bool seen_advertisement_{false};
  bool poll_requested_{false};
  bool retry_requested_{false};
  uint32_t retry_ready_ms_{0};
  bool has_started_poll_{false};
  bool has_completed_poll_{false};
  bool invitation_pending_{false};
};

}  // namespace esphome::omron
