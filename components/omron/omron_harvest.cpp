#include "omron_harvest.h"

#include <algorithm>

namespace esphome::omron {

namespace {

// Everything that survived the cut-off for one user, in the order the ring gave
// them up - newest first, because that is the order the planner names the slots
// and the order the newest-record rule depends on.
struct KeptRecords {
  std::vector<HarvestedRecord> records{};
  uint16_t parsed{0};
  uint16_t dropped_before_cutoff{0};
  uint16_t unreadable{0};
  uint16_t unparsed{0};
  MeasurementParseError first_parse_error{MeasurementParseError::NONE};
};

KeptRecords read_user_ring(const HarvestRequest &request, const UserRecordPlan &plan) {
  KeptRecords kept;
  const UserPollLayout &user_layout = request.layout->users[plan.user];
  for (uint16_t slot : plan.slots) {
    uint16_t address = 0;
    if (!record_address(user_layout.ring, slot, address)) {
      kept.unreadable++;
      continue;
    }
    std::vector<uint8_t> raw = request.memory->read(address, user_layout.ring.record_size);
    if (raw.size() != user_layout.ring.record_size) {
      kept.unreadable++;
      continue;
    }
    OmronMeasurement measurement;
    const MeasurementParseError error = parse_measurement_record(raw, *request.profile, measurement);
    if (error != MeasurementParseError::NONE) {
      kept.unparsed++;
      if (kept.first_parse_error == MeasurementParseError::NONE)
        kept.first_parse_error = error;
      continue;
    }

    kept.parsed++;
    const int64_t epoch = civil_seconds(measurement.timestamp);
    // Before anything else sees it. The record has two destinations from here,
    // the entities and the history queue, and a cut-off that reached only one
    // of them would leave a reading half published.
    if (request.cutoff_set && epoch < request.cutoff_epoch) {
      kept.dropped_before_cutoff++;
      continue;
    }

    HarvestedRecord record;
    record.measurement = measurement;
    record.slot = slot;
    record.address = address;
    record.epoch = epoch;
    record.fingerprint = measurement_fingerprint(request.profile->model, plan.user, address, raw);
    record.raw = std::move(raw);
    kept.records.push_back(std::move(record));
  }
  return kept;
}

}  // namespace

HarvestResult harvest_records(const HarvestRequest &request) {
  HarvestResult result{};
  if (request.profile == nullptr || request.layout == nullptr || request.memory == nullptr || request.plans == nullptr)
    return result;

  size_t budget = request.history_budget;

  for (const UserRecordPlan &plan : *request.plans) {
    if (plan.user >= request.layout->users.size() || plan.user >= OMRON_MAX_USERS)
      continue;
    HarvestedUser &user = result[plan.user];

    KeptRecords kept = read_user_ring(request, plan);
    user.parsed = kept.parsed;
    user.dropped_before_cutoff = kept.dropped_before_cutoff;
    user.unreadable = kept.unreadable;
    user.unparsed = kept.unparsed;
    user.first_parse_error = kept.first_parse_error;
    user.kept = static_cast<uint16_t>(kept.records.size());
    user.watermark = plan.user < request.watermark.size() ? request.watermark[plan.user] : 0;

    // The write cursor, not the clock, says which record is newest: the cuff
    // stamps everything the same until someone sets its time, and one wrong
    // stamp would then outrank every real reading. Slots come newest-first, so
    // the first one that decoded is the one the entities get.
    if (!kept.records.empty()) {
      user.valid = true;
      user.newest = kept.records.front();
    }

    if (request.history_records == 0 || kept.records.empty())
      continue;

    // Oldest first from here, so Home Assistant receives them in the order they
    // happened and the watermark only ever moves forward.
    std::sort(kept.records.begin(), kept.records.end(),
              [](const HarvestedRecord &lhs, const HarvestedRecord &rhs) { return lhs.epoch < rhs.epoch; });

    int64_t newest_reported = user.watermark;
    for (HarvestedRecord &record : kept.records) {
      if (record.epoch <= user.watermark)
        continue;
      if (request.now_known && record.epoch > request.now_epoch + request.future_tolerance_s) {
        user.dropped_in_future++;
        continue;
      }
      if (budget == 0) {
        user.history_truncated = true;
        break;
      }
      budget--;
      if (record.epoch > newest_reported)
        newest_reported = record.epoch;
      user.history.push_back(std::move(record));
    }
    if (newest_reported != user.watermark) {
      user.watermark = newest_reported;
      user.watermark_advanced = true;
    }
  }

  return result;
}

}  // namespace esphome::omron
