#include "omron_scheduler.h"

#include <algorithm>

namespace esphome::omron {

bool OmronPollScheduler::elapsed_(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms) {
  return static_cast<uint32_t>(now_ms - since_ms) >= duration_ms;
}

void OmronPollScheduler::note_advertisement(uint32_t now_ms, bool demands_session) {
  // Frames still arriving in the seconds after a poll are that poll's own tail.
  // Letting them move the silence clock leaves a press a few seconds later with
  // no gap to open an invitation with: the tail runs on for tens of
  // milliseconds, a press follows seconds later, and the whole burst it starts
  // goes unanswered.
  //
  // Ignoring the tail costs nothing, because a cuff that has been served stops
  // advertising at once - either no further frames at all, or a couple inside a
  // tenth of a second.
  if (this->has_completed_poll_ && !elapsed_(now_ms, this->last_poll_finished_ms_, this->policy_.poll_tail_settle_ms))
    return;

  const bool after_silence_before_this_frame =
      !this->seen_advertisement_ ||
      elapsed_(now_ms, this->last_advertisement_ms_, this->policy_.advertisement_freshness_ms);
  this->last_advertisement_ms_ = now_ms;
  this->seen_advertisement_ = true;

  // A burst that starts after silence is the cuff asking for something: it
  // keeps its radio off until a button is pressed or a measurement finishes. So
  // every such burst earns a session, whatever the sequence numbers say.
  //
  // Do not gate this on the sequence numbers, however tempting. With nothing
  // newly measured, the frame after a short press is bit for bit the cuff's
  // idle chatter, so the press would do nothing at all - which is most of the
  // times somebody presses it. The silence clock above is what stops that
  // costing a session per burst.
  //
  // A frame setting one of its own request bits - pairing, forced transfer,
  // invalid clock - outranks the silence window as well. Without that, a cuff
  // held into pairing mode while already awake offers no silence to open an
  // invitation with, and a node ignores it blinking -P- indefinitely.
  if (demands_session || after_silence_before_this_frame)
    this->invitation_pending_ = true;
}

void OmronPollScheduler::note_poll_started(uint32_t now_ms) {
  this->last_poll_started_ms_ = now_ms;
  this->has_started_poll_ = true;
  this->poll_requested_ = false;
  this->retry_requested_ = false;
  // Spent on this poll. A burst lasts far longer than one session, so leaving it
  // set would reconnect over and over for as long as the cuff keeps talking.
  this->invitation_pending_ = false;
}

void OmronPollScheduler::note_poll_finished(uint32_t now_ms, bool success) {
  this->last_poll_finished_ms_ = now_ms;
  this->has_completed_poll_ = true;
  // Also cleared here, not only when a poll starts: a poll that ran and failed
  // has answered this invitation, and letting it stand would drive straight
  // through the backoff for as long as the cuff kept advertising. The next press
  // sets it again.
  this->invitation_pending_ = false;
  if (success) {
    this->retry_delay_ms_ = 0;
    this->scheduled_delay_ms_ = this->policy_.normal_interval_ms;
  } else {
    if (this->retry_delay_ms_ == 0) {
      this->retry_delay_ms_ = std::min(this->policy_.initial_retry_ms, this->policy_.maximum_retry_ms);
    } else if (this->retry_delay_ms_ >= this->policy_.maximum_retry_ms ||
               this->retry_delay_ms_ > this->policy_.maximum_retry_ms / 2U) {
      this->retry_delay_ms_ = this->policy_.maximum_retry_ms;
    } else {
      this->retry_delay_ms_ *= 2U;
    }
    this->scheduled_delay_ms_ = this->retry_delay_ms_;
  }
}

void OmronPollScheduler::request_poll() {
  this->poll_requested_ = true;
}

void OmronPollScheduler::request_retry(uint32_t now_ms) {
  this->retry_requested_ = true;
  this->retry_ready_ms_ = now_ms + this->policy_.retry_gap_ms;
}

void OmronPollScheduler::clear_request() {
  this->poll_requested_ = false;
  this->retry_requested_ = false;
}

bool OmronPollScheduler::advertisement_is_fresh(uint32_t now_ms) const {
  return this->seen_advertisement_ &&
         !elapsed_(now_ms, this->last_advertisement_ms_, this->policy_.advertisement_freshness_ms);
}

bool OmronPollScheduler::should_poll(uint32_t now_ms, bool busy, bool cleanup_pending) const {
  if (busy || cleanup_pending || !this->advertisement_is_fresh(now_ms))
    return false;
  // Signed difference, so the gap survives the millis() wrap the same way the
  // rest of this scheduler does.
  if (this->retry_requested_)
    return static_cast<int32_t>(now_ms - this->retry_ready_ms_) >= 0;
  if (this->has_started_poll_ && !elapsed_(now_ms, this->last_poll_started_ms_, this->policy_.minimum_gap_ms))
    return false;
  if (this->poll_requested_ || this->invitation_pending_)
    return true;
  if (!this->has_completed_poll_)
    return true;
  return elapsed_(now_ms, this->last_poll_finished_ms_, this->scheduled_delay_ms_);
}

}  // namespace esphome::omron
