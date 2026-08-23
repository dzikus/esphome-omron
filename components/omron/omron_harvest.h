#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "omron_measurement.h"
#include "omron_memory.h"
#include "omron_poll_plan.h"
#include "omron_profiles.h"

namespace esphome::omron {

// What a finished transfer yields: which reading each person's entities get,
// and which older ones still owe Home Assistant an event.
//
// The failure mode is silent - pick the wrong slot and the node publishes a
// reading from three days ago as if it were taken this morning - so this is a
// pure layer the host suite can drive. It reads a memory image and answers what
// is in it, touching no entities, preferences or link.

struct HarvestedRecord {
  OmronMeasurement measurement{};
  uint16_t slot{0};
  uint16_t address{0};
  int64_t epoch{0};
  std::vector<uint8_t> raw{};
  // Mixed from the profile, the user, the address and the bytes.
  uint32_t fingerprint{0};
};

struct HarvestedUser {
  // False says nothing on its own about why; the counters below separate the
  // causes.
  bool valid{false};
  HarvestedRecord newest{};
  // Everything that also beat the watermark, oldest first, so Home Assistant
  // receives them in the order they happened.
  std::vector<HarvestedRecord> history{};

  // Records that parsed, cut-off or not.
  uint16_t parsed{0};
  // Of those, the ones past the cut-off.
  uint16_t kept{0};
  uint16_t dropped_before_cutoff{0};
  // Slots the plan named that the memory image could not give up: the address
  // fell outside the ring, or the transfer came back short. This is a fault in
  // the plan or in the read, not in the data.
  uint16_t unreadable{0};
  // Slots whose bytes arrived and the decoder refused. Its own counter because
  // this is what a profile aimed at the wrong region looks like: real bytes,
  // none of them decodable. Folded into any other count it is indistinguishable
  // from an empty ring, and almost every profile here is unproven on hardware.
  uint16_t unparsed{0};
  // One reason is enough: the rest of the ring usually fails the same way.
  MeasurementParseError first_parse_error{MeasurementParseError::NONE};
  // Stamped later than now. A record from the future would raise the watermark
  // past every real measurement and silence this user for good.
  uint16_t dropped_in_future{0};
  bool history_truncated{false};

  // Where the watermark should stand once this history has actually gone out.
  // Not advanced by the caller until then: advancing it here would be a promise
  // the node has not kept.
  int64_t watermark{0};
  bool watermark_advanced{false};
};

struct HarvestRequest {
  const OmronProfile *profile{nullptr};
  const PollLayout *layout{nullptr};
  const OmronMemoryImage *memory{nullptr};
  const std::vector<UserRecordPlan> *plans{nullptr};

  // Records stamped before this are dropped whole: no entity, no event, no
  // watermark. Unset keeps everything.
  bool cutoff_set{false};
  int64_t cutoff_epoch{0};

  // Zero means entities only - the ring is still read and the newest still
  // published, but nothing older leaves as an event.
  uint8_t history_records{0};

  // The newest epoch already reported, per user.
  std::array<int64_t, OMRON_MAX_USERS> watermark{};

  // Wall clock, for the future-stamp check. Without a time source nothing is
  // rejected on those grounds.
  bool now_known{false};
  int64_t now_epoch{0};
  // A day of slack, so a cuff a few minutes ahead is not treated as broken.
  int64_t future_tolerance_s{86400};

  // How many events the caller can still take. Zero drops the rest of the ring
  // rather than growing the queue without bound.
  size_t history_budget{0};
};

using HarvestResult = std::array<HarvestedUser, OMRON_MAX_USERS>;

// A request missing any of its four pointers yields an empty result rather than
// a crash: this runs at the end of a session that may have failed half way.
HarvestResult harvest_records(const HarvestRequest &request);

}  // namespace esphome::omron
