#include "omron_bond_cleanup.h"

#include "omron_log.h"

namespace esphome::omron {

static const char *const TAG = "omron.bond";

void OmronBondCleanup::begin(const char *reason) {
  if (this->host_ == nullptr)
    return;
  // Already running: keep the clock that is already ticking. A teardown can ask
  // twice - a failed OPEN and then the disconnect behind it - and restarting
  // here would hand the second request a fresh timeout it did not earn.
  if (this->pending())
    return;

  OMRON_LOG_I(TAG, "[%s] Starting selective per-session bond cleanup (%s)", this->host_->bond_address(), reason);
  this->phase_ = Phase::ARMED;
  this->error_logged_ = false;
  if (this->diagnostics_ != nullptr)
    this->diagnostics_->phase = SessionPhase::REMOVING_BOND;
}

void OmronBondCleanup::tick(uint32_t now) {
  if (this->host_ == nullptr || !this->pending())
    return;

  if (this->phase_ == Phase::ARMED) {
    this->phase_ = Phase::LOOKING_UP;
    this->started_at_ = now;
    // Backdated so the first tick asks immediately rather than idling out the
    // interval before the first question.
    this->last_poll_at_ = now - POLL_INTERVAL_MS;
  }

  if (now - this->started_at_ >= this->timeout_ms_) {
    OMRON_LOG_E(TAG, "[%s] Selective bond cleanup timed out; continuing with a possibly stale bond",
                this->host_->bond_address());
    this->phase_ = Phase::FAILED;
    if (this->diagnostics_ != nullptr) {
      this->diagnostics_->cleanup_failures++;
      this->diagnostics_->phase = SessionPhase::IDLE;
    }
    return;
  }
  if (now - this->last_poll_at_ < POLL_INTERVAL_MS)
    return;
  this->last_poll_at_ = now;

  int error = 0;
  const BondLookupResult result = this->host_->bond_lookup(error);
  if (result == BondLookupResult::ERROR) {
    if (!this->error_logged_) {
      OMRON_LOG_W(TAG, "[%s] Could not inspect bond list, retrying (code=%d)", this->host_->bond_address(), error);
      this->error_logged_ = true;
    }
    return;
  }

  if (result == BondLookupResult::ABSENT) {
    this->phase_ = Phase::IDLE;
    if (this->diagnostics_ != nullptr)
      this->diagnostics_->phase = SessionPhase::IDLE;
    // Only once the record is confirmed gone. Doing it while the removal is
    // still in flight would throw away a cache that still has a bond behind it.
    this->host_->bond_forget_attribute_cache();
    OMRON_LOG_I(TAG, "[%s] Selective bond cleanup complete", this->host_->bond_address());
    return;
  }

  // Still there. Ask once and then keep looking: the stack answers the request,
  // not the outcome, so the disappearance of the address is the only proof.
  if (this->phase_ != Phase::REMOVE_REQUESTED) {
    if (this->host_->bond_remove(error)) {
      this->phase_ = Phase::REMOVE_REQUESTED;
      OMRON_LOG_I(TAG, "[%s] Bond removal accepted; waiting until the exact address disappears",
                  this->host_->bond_address());
    } else if (!this->error_logged_) {
      OMRON_LOG_W(TAG, "[%s] Selective bond removal was rejected, retrying (code=%d)", this->host_->bond_address(),
                  error);
      this->error_logged_ = true;
    }
  }
}

}  // namespace esphome::omron
