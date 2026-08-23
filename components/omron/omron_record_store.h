#pragma once

#include <array>
#include <cstdint>

#include "omron_profiles.h"

namespace esphome::omron {

class OmronRecordStore {
 public:
  // One slot per user the catalog can address. Sized from the profile limit so
  // an indexing bug cannot quietly land inside a spare slot.
  static constexpr uint8_t MAX_USERS = OMRON_MAX_USERS;

  // An out-of-range user is not "already seen". Callers must range-check first;
  // this returns true so a bad index surfaces downstream instead of being
  // silently swallowed as a duplicate.
  bool is_new(uint8_t user, uint32_t fingerprint) const;
  bool accept(uint8_t user, uint32_t fingerprint);

 private:
  std::array<uint32_t, MAX_USERS> fingerprints_{};
  std::array<bool, MAX_USERS> valid_{};
};

}  // namespace esphome::omron
