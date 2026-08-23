#include "omron_subscriptions.h"

#include "omron_log.h"

namespace esphome::omron {

static const char *const TAG = "omron.subs";

void OmronSubscriptions::clear() {
  this->targets_.clear();
  this->index_ = 0;
  this->phase_ = Phase::IDLE;
  this->target_at_ = 0;
  this->attempt_ = 0;
}

void OmronSubscriptions::add(uint16_t characteristic_handle, uint16_t descriptor_handle, bool required) {
  this->targets_.push_back({characteristic_handle, descriptor_handle, required});
}

void OmronSubscriptions::begin(uint32_t now) {
  if (this->host_ == nullptr) {
    // Loud, because silence here costs a session. A machine wired to nothing
    // does nothing: no registration, no retry, and the only symptom is the
    // phase timeout ten seconds later saying the notifications were never
    // configured, which reads as a peer problem and is not one.
    OMRON_LOG_E(TAG, "Subscription queue has no host; nothing will be subscribed. This is a wiring bug.");
    this->phase_ = Phase::FAILED;
    return;
  }
  this->index_ = 0;
  this->attempt_ = 0;
  this->subscribe_current_(now);
}

void OmronSubscriptions::subscribe_current_(uint32_t now) {
  // Walked rather than recursed: an optional target that the stack refuses on
  // the spot moves straight to the next one, and a list of them would otherwise
  // nest one call per target.
  while (true) {
    if (this->index_ >= this->targets_.size()) {
      this->phase_ = Phase::READY;
      this->host_->subscription_ready();
      return;
    }

    this->phase_ = Phase::SUBSCRIBING;
    this->target_at_ = now;
    this->attempt_++;

    const Target &target = this->targets_[this->index_];
    int error = 0;
    if (this->host_->subscription_register(target.characteristic_handle, error))
      return;

    // The request itself was refused, which is not the hang this class exists
    // for - there will be no event either way, so decide now.
    if (target.required) {
      this->phase_ = Phase::FAILED;
      this->host_->subscription_failed("Could not register a required GATT notification", error);
      return;
    }
    this->host_->subscription_dropped_optional(target.characteristic_handle, "registration could not be requested",
                                               error);
    this->index_++;
    this->attempt_ = 0;
  }
}

void OmronSubscriptions::advance_(uint32_t now) {
  this->index_++;
  this->attempt_ = 0;
  this->subscribe_current_(now);
}

void OmronSubscriptions::drop_optional_or_fail_(const char *why, int error, uint32_t now) {
  const Target &target = this->targets_[this->index_];
  if (target.required) {
    this->phase_ = Phase::FAILED;
    this->host_->subscription_failed(why, error);
    return;
  }
  this->host_->subscription_dropped_optional(target.characteristic_handle, why, error);
  this->advance_(now);
}

void OmronSubscriptions::on_register_result(uint16_t characteristic_handle, bool ok, int status) {
  if (this->host_ == nullptr || this->index_ >= this->targets_.size())
    return;
  if (this->targets_[this->index_].characteristic_handle != characteristic_handle)
    return;
  // Accepted is not done. The CCCD write is a separate event and may never
  // arrive at all, which is what the retry clock in tick() is for - so this
  // returns and waits rather than advancing.
  if (ok)
    return;
  this->drop_optional_or_fail_("Required GATT notification registration failed", status,
                               this->host_->subscription_now_ms());
}

void OmronSubscriptions::on_descriptor_written(uint16_t descriptor_handle, bool ok, int status) {
  if (this->host_ == nullptr || this->index_ >= this->targets_.size())
    return;
  if (this->targets_[this->index_].descriptor_handle != descriptor_handle)
    return;

  const uint32_t now = this->host_->subscription_now_ms();
  if (ok) {
    this->advance_(now);
    return;
  }

  // The first CCCD write comes back insufficient
  // authentication on a link the cuff was happy to open, let through discovery
  // and negotiate an MTU on. That is the cuff asking to be paired at the moment
  // it matters, which is what an OS stack waits for instead of encrypting
  // speculatively at connect. Subscribing is what makes this device demand
  // security, so it is answered here rather than treated as a failure.
  if (this->host_->subscription_needs_encryption(status)) {
    this->phase_ = Phase::AWAITING_ENCRYPTION;
    return;
  }

  this->drop_optional_or_fail_("Required GATT CCCD write failed", status, now);
}

void OmronSubscriptions::resume(uint32_t now) {
  if (this->host_ == nullptr || this->phase_ != Phase::AWAITING_ENCRYPTION)
    return;
  // The same target again, from attempt one: the encryption it was waiting for
  // is the thing that changed, so the attempts it spent before are not evidence
  // about the attempt after.
  this->attempt_ = 0;
  this->subscribe_current_(now);
}

void OmronSubscriptions::tick(uint32_t now) {
  if (this->host_ == nullptr || !this->pending() || this->index_ >= this->targets_.size())
    return;
  if (now - this->target_at_ < RETRY_INTERVAL_MS)
    return;

  const Target &stuck = this->targets_[this->index_];
  if (this->attempt_ < MAX_ATTEMPTS) {
    this->host_->subscription_retrying(stuck.characteristic_handle, static_cast<uint8_t>(this->attempt_ + 1),
                                       MAX_ATTEMPTS);
    // Not advance_: the same target is asked again, and the attempt counter has
    // to survive so this terminates.
    this->subscribe_current_(now);
    return;
  }
  this->drop_optional_or_fail_("The stack never wrote the CCCD for a required notification", 0, now);
}

}  // namespace esphome::omron
