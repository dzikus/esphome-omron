#include "omron_publish.h"

#include <cmath>

#include "omron_metrics.h"

namespace esphome::omron {

OmronEntityCapabilities capabilities_for_profile(const OmronProfile &profile) {
  OmronEntityCapabilities capabilities = entity_capability(OmronEntityCapability::PROFILE) |
                                         OmronEntityCapability::STATUS | OmronEntityCapability::RSSI |
                                         OmronEntityCapability::POLL_DURATION | OmronEntityCapability::CONNECTION;
  if (profile.record_format != RecordFormat::UNSUPPORTED) {
    capabilities |= entity_capability(OmronEntityCapability::SYSTOLIC);
    capabilities |= entity_capability(OmronEntityCapability::DIASTOLIC);
    capabilities |= entity_capability(OmronEntityCapability::PULSE);
    capabilities |= entity_capability(OmronEntityCapability::TIMESTAMP);
    // Per field, per model. An absent field reads as a permanent OFF, which on
    // a cuff-fit entity looks exactly like a correctly fitted cuff.
    const MeasurementFields fields = profile.measurement_fields;
    if ((fields & MEASUREMENT_FIELD_CUFF) != 0)
      capabilities |= entity_capability(OmronEntityCapability::CUFF_FIT);
    if ((fields & MEASUREMENT_FIELD_MOVEMENT) != 0)
      capabilities |= entity_capability(OmronEntityCapability::BODY_MOVEMENT);
    if ((fields & MEASUREMENT_FIELD_IRREGULAR) != 0)
      capabilities |= entity_capability(OmronEntityCapability::IRREGULAR_PULSE);
    if ((fields & MEASUREMENT_FIELD_CONSECUTIVE) != 0)
      capabilities |= entity_capability(OmronEntityCapability::CONSECUTIVE_MEASUREMENT);
    if ((fields & MEASUREMENT_FIELD_ARTIFACT) != 0)
      capabilities |= entity_capability(OmronEntityCapability::ARTIFACT_DETECTION);
    if ((fields & MEASUREMENT_FIELD_IHB) != 0)
      capabilities |= entity_capability(OmronEntityCapability::IHB_DETECTION);
    // IMPROPER_POSITION is deliberately absent. The standard blood-pressure
    // characteristic is its only source, no stored record carries the field on
    // any model here, and a session with a profile does not subscribe that
    // characteristic at all - so granting it would be an entity nothing could
    // ever fill. It is granted on the live-only path, where that subscription
    // is the whole of the session.
    //
    // Not gated, and knowingly so: no model here is known to describe a battery
    // field and the bit is unsourced. The entity is the only way anyone will
    // find out whether it means anything.
    capabilities |= entity_capability(OmronEntityCapability::BATTERY);
    // Gated on the offset rather than on the record format: the two agree on
    // every profile except the families that carry no sequence number at all.
    if (profile.record_sequence_offset != NO_RECORD_SEQUENCE)
      capabilities |= entity_capability(OmronEntityCapability::SEQUENCE);
  }
  if (profile.user_count != 0)
    capabilities |= entity_capability(OmronEntityCapability::USER);
  // Settings entities exist only where the block layout is known. Asking
  // user_settings_block rather than naming a model keeps the answer in the one
  // place that owns the geometry.
  //
  // The two are not granted together. A six-byte block carries the birth date
  // and nothing else, so a version entity on those models would sit at zero
  // for ever - and zero is what an unread counter and a never-registered one
  // both look like.
  OmronSettingsBlock first_user_block{};
  if (user_settings_block(profile, 1, first_user_block)) {
    capabilities |= entity_capability(OmronEntityCapability::BIRTH_DATE);
    if (user_block_carries_version(profile))
      capabilities |= entity_capability(OmronEntityCapability::SETTINGS_VERSION);
  }
  return capabilities;
}

OmronEntityCapabilities capabilities_for_live_only(const StandardBpFeatures *features) {
  OmronEntityCapabilities capabilities = entity_capability(OmronEntityCapability::PROFILE) |
                                         OmronEntityCapability::STATUS | OmronEntityCapability::RSSI |
                                         OmronEntityCapability::POLL_DURATION | OmronEntityCapability::CONNECTION;
  capabilities |= entity_capability(OmronEntityCapability::SYSTOLIC);
  capabilities |= entity_capability(OmronEntityCapability::DIASTOLIC);
  capabilities |= entity_capability(OmronEntityCapability::PULSE);
  capabilities |= entity_capability(OmronEntityCapability::TIMESTAMP);
  capabilities |= entity_capability(OmronEntityCapability::USER);
  if (features == nullptr || features->body_movement)
    capabilities |= entity_capability(OmronEntityCapability::BODY_MOVEMENT);
  if (features == nullptr || features->cuff_fit)
    capabilities |= entity_capability(OmronEntityCapability::CUFF_FIT);
  if (features == nullptr || features->irregular_pulse)
    capabilities |= entity_capability(OmronEntityCapability::IRREGULAR_PULSE);
  if (features == nullptr || features->measurement_position)
    capabilities |= entity_capability(OmronEntityCapability::IMPROPER_POSITION);
  return capabilities;
}

OmronEntityData entity_from_record(const OmronMeasurement &measurement, uint8_t user_index,
                                   const std::string &timestamp) {
  OmronEntityData data;
  data.systolic = measurement.systolic_mm_hg;
  data.diastolic = measurement.diastolic_mm_hg;
  data.pulse = measurement.pulse_bpm;
  data.timestamp = timestamp;
  data.user = static_cast<uint8_t>(user_index + 1);  // planner is zero-based; entity is one-based.
  data.cuff_fit = measurement.cuff_flag;
  data.body_movement = measurement.movement_detected;
  data.irregular_pulse = measurement.irregular_heartbeat;
  data.consecutive_measurement = measurement.consecutive_measurement;
  data.artifact_detection = measurement.artifact_detection;
  data.ihb_detection = measurement.ihb_detection;
  data.battery_low = measurement.battery_flag;
  data.mark_available(OmronEntityCapability::SYSTOLIC);
  data.mark_available(OmronEntityCapability::DIASTOLIC);
  data.mark_available(OmronEntityCapability::PULSE);
  data.mark_available(OmronEntityCapability::TIMESTAMP);
  data.mark_available(OmronEntityCapability::USER);
  data.mark_available(OmronEntityCapability::CUFF_FIT);
  data.mark_available(OmronEntityCapability::BODY_MOVEMENT);
  data.mark_available(OmronEntityCapability::IRREGULAR_PULSE);
  // IMPROPER_POSITION is deliberately not marked: an EEPROM record has no field
  // for it. Only the standard blood-pressure characteristic does.
  data.mark_available(OmronEntityCapability::CONSECUTIVE_MEASUREMENT);
  data.mark_available(OmronEntityCapability::ARTIFACT_DETECTION);
  data.mark_available(OmronEntityCapability::IHB_DETECTION);
  data.mark_available(OmronEntityCapability::BATTERY);
  if (measurement.has_record_id) {
    data.sequence = measurement.record_id;
    data.mark_available(OmronEntityCapability::SEQUENCE);
  }
  return data;
}

const char *standard_publish_decision_to_string(StandardPublishDecision decision) {
  switch (decision) {
    case StandardPublishDecision::PUBLISH:
      return "published";
    case StandardPublishDecision::INCOMPLETE:
      return "the notification carried no systolic/diastolic pair";
    case StandardPublishDecision::OUT_OF_RANGE:
      return "the decoded pressures are outside a plausible range";
    case StandardPublishDecision::UNKNOWN_USER:
      return "the notification names a user this node has no slot for";
    case StandardPublishDecision::NO_USER_ID_NO_PROFILE:
      return "no user id, and no profile to say how many users exist";
    case StandardPublishDecision::NO_USER_ID_MULTI_USER:
      return "no user id on a profile that stores more than one user";
  }
  return "unknown";
}

StandardPublishDecision standard_measurement_entity(const StandardBloodPressureMeasurement &measurement,
                                                    const OmronProfile *profile, const std::string &timestamp,
                                                    uint8_t &user_index, OmronEntityData &out) {
  user_index = 0;
  if (!measurement.has_systolic || !measurement.has_diastolic)
    return StandardPublishDecision::INCOMPLETE;

  const float scale = measurement.units_kpa ? 7.50062f : 1.0f;
  const float systolic = measurement.systolic * scale;
  const float diastolic = measurement.diastolic * scale;
  if (!std::isfinite(systolic) || !std::isfinite(diastolic) || systolic < OMRON_MIN_SYSTOLIC ||
      systolic > OMRON_MAX_SYSTOLIC || diastolic < OMRON_MIN_DIASTOLIC || diastolic > OMRON_MAX_DIASTOLIC ||
      systolic <= diastolic)
    return StandardPublishDecision::OUT_OF_RANGE;

  // A live notification has to name its owner before it can be published: the
  // entity sets belong to people, and guessing would file one person's reading
  // under the other. The one safe fallback is a cuff that only has one user.
  const bool user_named = measurement.has_user_id && measurement.user_id != 0xFF;
  if (user_named) {
    // Both ceilings: a cuff storing one person naming a second one has no slot
    // to publish into.
    if (measurement.user_id == 0 || measurement.user_id > OMRON_ENTITY_USER_SLOTS ||
        (profile != nullptr && measurement.user_id > profile->user_count))
      return StandardPublishDecision::UNKNOWN_USER;
    user_index = static_cast<uint8_t>(measurement.user_id - 1);
  } else if (profile == nullptr) {
    return StandardPublishDecision::NO_USER_ID_NO_PROFILE;
  } else if (profile->user_count > 1) {
    return StandardPublishDecision::NO_USER_ID_MULTI_USER;
  }

  OmronEntityData entity;
  entity.systolic = static_cast<uint16_t>(std::lround(systolic));
  entity.diastolic = static_cast<uint16_t>(std::lround(diastolic));
  entity.mark_available(OmronEntityCapability::SYSTOLIC);
  entity.mark_available(OmronEntityCapability::DIASTOLIC);
  if (measurement.has_pulse_rate && std::isfinite(measurement.pulse_rate) &&
      measurement.pulse_rate >= OMRON_MIN_PULSE && measurement.pulse_rate <= OMRON_MAX_PULSE) {
    entity.pulse = static_cast<uint16_t>(std::lround(measurement.pulse_rate));
    entity.mark_available(OmronEntityCapability::PULSE);
  }
  if (measurement.has_timestamp) {
    entity.timestamp = timestamp;
    entity.mark_available(OmronEntityCapability::TIMESTAMP);
  }
  if (user_named) {
    entity.user = measurement.user_id;
    entity.mark_available(OmronEntityCapability::USER);
  }
  if (measurement.has_measurement_status) {
    entity.cuff_fit = !measurement.cuff_too_loose();
    entity.body_movement = measurement.body_movement_detected();
    entity.irregular_pulse = measurement.irregular_pulse_detected();
    entity.improper_position = measurement.improper_position_detected();
    entity.mark_available(OmronEntityCapability::CUFF_FIT);
    entity.mark_available(OmronEntityCapability::BODY_MOVEMENT);
    entity.mark_available(OmronEntityCapability::IRREGULAR_PULSE);
    entity.mark_available(OmronEntityCapability::IMPROPER_POSITION);
  }
  out = entity;
  return StandardPublishDecision::PUBLISH;
}

const char *poll_outcome_status(const HarvestResult &harvest, uint8_t users_decoded, uint8_t users_published,
                                bool read_anything) {
  if (users_decoded != 0)
    return users_published == 0 ? "read complete; latest measurement unchanged" : "ok";

  uint16_t unparsed = 0;
  uint16_t dropped = 0;
  for (const HarvestedUser &user : harvest) {
    unparsed = static_cast<uint16_t>(unparsed + user.unparsed);
    dropped = static_cast<uint16_t>(dropped + user.dropped_before_cutoff);
  }
  // Order matters: a ring that gave up bytes which decoded to nothing is the
  // diagnosis worth surfacing first, because it is the one that means the
  // configured memory map is wrong.
  if (unparsed != 0)
    return "read complete; no slot decoded - check the configured profile";
  if (dropped != 0)
    return "read complete; every record older than the configured cut-off";
  if (!read_anything) {
    // Nothing was read because nothing had moved: every ring's write cursor
    // stood where the last successful session left it. Saying "no record
    // stored" here is a claim about the cuff that this session never checked,
    // and it is the common case - a button press with no new measurement behind
    // it ends exactly this way.
    return "nothing new since the last session";
  }
  return "read complete; no measurement record stored";
}

bool settings_entity_for_user(const OmronProfile &profile, uint8_t user_number, std::span<const uint8_t> settings,
                              OmronUserSettingsData &out) {
  OmronSettingsBlock block{};
  if (settings.empty() || !user_settings_block(profile, user_number, block))
    return false;

  OmronUserSettingsData published{};
  OmronDateTime born{};
  if (user_birth_date(profile, user_number, settings, born)) {
    published.birth_date = format_date(born);
    published.mark_available(OmronEntityCapability::BIRTH_DATE);
  }
  // Little-endian. Stepping once per pairing is correct; a counter climbing on
  // every poll means the registration gate broke and every read is spending a
  // write to the device.
  //
  // Marked only where the block has one, so the entity of a model that keeps no
  // counter stays unknown rather than publishing the zero this would read.
  if (user_block_carries_version(profile)) {
    published.version = user_settings_version(profile, user_number, settings);
    published.mark_available(OmronEntityCapability::SETTINGS_VERSION);
  }
  out = published;
  return true;
}

}  // namespace esphome::omron
