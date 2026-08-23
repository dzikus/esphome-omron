#include "omron_record_store.h"

namespace esphome::omron {

bool OmronRecordStore::is_new(uint8_t user, uint32_t fingerprint) const {
  if (user >= MAX_USERS)
    return true;
  return !this->valid_[user] || this->fingerprints_[user] != fingerprint;
}

bool OmronRecordStore::accept(uint8_t user, uint32_t fingerprint) {
  if (user >= MAX_USERS)
    return false;
  const bool changed = this->is_new(user, fingerprint);
  this->fingerprints_[user] = fingerprint;
  this->valid_[user] = true;
  return changed;
}

}  // namespace esphome::omron
