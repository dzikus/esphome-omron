// What goes into an entity.
//
// Every mistake this layer can make is silent: a field taken from the wrong
// place still publishes a number in the right range, and an inverted flag still
// publishes a flag. The assertions below pin the four that are easiest to get
// wrong - the sequence number, the position bit, the cuff-fit contract, and the
// separation between a status update and a reading.

#include <cassert>
#include <cstdint>
#include <string>

#include "omron_publish.h"
#include "test_support.h"

using namespace esphome::omron;

namespace {

StandardBloodPressureMeasurement good_measurement() {
  StandardBloodPressureMeasurement measurement{};
  measurement.has_systolic = true;
  measurement.has_diastolic = true;
  measurement.systolic = 126.0f;
  measurement.diastolic = 85.0f;
  return measurement;
}

}  // namespace

void test_publish_capabilities_follow_the_catalog() {
  // omron_entities.cpp refuses to compile when a capability in a publishing
  // group has no entity wired to it. That leaves one way through, and it is this
  // one: granting a capability that belongs to no group at all, which no static
  // assertion over the tables can see. Nothing may hand out a bit unplaced.
  // And the other way a capability goes unpublished: granted to a profile with
  // no source behind it. IMPROPER_POSITION comes only from the standard
  // characteristic, which a session with a profile does not subscribe, and no
  // stored record carries the field on any model - so no profile may claim it.
  // A live-only session may, because there the subscription is all it has.
  for (size_t i = 0; i < profile_count(); i++) {
    const OmronProfile *entry = profile_at(i);
    assert(entry != nullptr);
    assert((capabilities_for_profile(*entry) & entity_capability(OmronEntityCapability::IMPROPER_POSITION)) == 0);
  }
  assert((capabilities_for_live_only(nullptr) & entity_capability(OmronEntityCapability::IMPROPER_POSITION)) != 0);

  for (size_t i = 0; i < profile_count(); i++) {
    const OmronProfile *profile = profile_at(i);
    assert(profile != nullptr);
    assert((capabilities_for_profile(*profile) & ~OMRON_CAPABILITIES_ALL) == 0);
  }
  assert((capabilities_for_live_only(nullptr) & ~OMRON_CAPABILITIES_ALL) == 0);

  // This profile declares cuff, movement, irregular and consecutive detections,
  // and a sequence number at offset 10.
  const OmronEntityCapabilities measured = capabilities_for_profile(get_profile(OmronProfileId::HEM_7155T_MW3));
  assert((measured & entity_capability(OmronEntityCapability::SYSTOLIC)) != 0);
  assert((measured & entity_capability(OmronEntityCapability::CUFF_FIT)) != 0);
  assert((measured & entity_capability(OmronEntityCapability::SEQUENCE)) != 0);
  // Settings entities need a block to read them out of, and this profile has one.
  assert((measured & entity_capability(OmronEntityCapability::BIRTH_DATE)) != 0);
  assert((measured & entity_capability(OmronEntityCapability::SETTINGS_VERSION)) != 0);

  // A family whose records carry no sequence number must not offer it. It would
  // publish a permanent zero, which is indistinguishable from a real reading of
  // zero.
  const OmronProfile &no_sequence = get_profile(OmronProfileId::HEM_7322T);
  assert(no_sequence.record_sequence_offset == NO_RECORD_SEQUENCE);
  assert((capabilities_for_profile(no_sequence) & entity_capability(OmronEntityCapability::SEQUENCE)) == 0);

  // Six-byte blocks list a birth date and no version counter, but the block
  // exists, so both settings entities are offered and the write path is what
  // refuses the registration.
  assert((capabilities_for_profile(no_sequence) & entity_capability(OmronEntityCapability::BIRTH_DATE)) != 0);

  // Live-only with nothing read: claim all four detections rather than none. An
  // unknown cuff that does detect movement should not lose the entity over a
  // 0x2A49 read that failed.
  const OmronEntityCapabilities unknown = capabilities_for_live_only(nullptr);
  assert((unknown & entity_capability(OmronEntityCapability::BODY_MOVEMENT)) != 0);
  assert((unknown & entity_capability(OmronEntityCapability::IMPROPER_POSITION)) != 0);
  // And no settings entities at all, because there is no block layout to read.
  assert((unknown & entity_capability(OmronEntityCapability::BIRTH_DATE)) == 0);

  // A features word of 0x0007 states movement, cuff and irregular, and no
  // position. The position entity has to disappear with it.
  StandardBpFeatures features{};
  const std::array<uint8_t, 2> stated_features{0x07, 0x00};
  assert(parse_standard_bp_features(stated_features, features));
  const OmronEntityCapabilities stated = capabilities_for_live_only(&features);
  assert((stated & entity_capability(OmronEntityCapability::BODY_MOVEMENT)) != 0);
  assert((stated & entity_capability(OmronEntityCapability::CUFF_FIT)) != 0);
  assert((stated & entity_capability(OmronEntityCapability::IRREGULAR_PULSE)) != 0);
  assert((stated & entity_capability(OmronEntityCapability::IMPROPER_POSITION)) == 0);
}

void test_publish_record_marks_only_what_a_record_carries() {
  OmronMeasurement measurement{};
  measurement.systolic_mm_hg = 126;
  measurement.diastolic_mm_hg = 85;
  measurement.pulse_bpm = 78;
  measurement.cuff_flag = true;
  measurement.consecutive_measurement = 2;
  measurement.has_record_id = true;
  measurement.record_id = 21;

  const OmronEntityData data = entity_from_record(measurement, 1, "2026-08-13T16:06:18+02:00");
  assert(data.systolic == 126 && data.diastolic == 85 && data.pulse == 78);
  // Planner counts users from zero, the entity from one. Getting this backwards
  // files one person's reading under the other.
  assert(data.user == 2);
  assert(data.timestamp == "2026-08-13T16:06:18+02:00");
  assert(data.has(OmronEntityCapability::SEQUENCE) && data.sequence == 21);
  assert(data.consecutive_measurement == 2);

  // The one that matters. No stored record carries a position field on any
  // model, and the bit nearest to where one would sit is the
  // consecutive-measurement index. Marking it available publishes that number
  // as a body-position flag.
  assert(!data.has(OmronEntityCapability::IMPROPER_POSITION));
  assert(data.has(OmronEntityCapability::CONSECUTIVE_MEASUREMENT));

  // A record with no sequence number must not claim one, or the entity reads a
  // stale value forever.
  OmronMeasurement without = measurement;
  without.has_record_id = false;
  assert(!entity_from_record(without, 0, "").has(OmronEntityCapability::SEQUENCE));
}

void test_publish_standard_notification_must_name_its_owner() {
  const OmronProfile &two_users = get_profile(OmronProfileId::HEM_7155T_MW3);
  assert(two_users.user_count == 2);
  uint8_t user_index = 99;
  OmronEntityData entity;

  // No user id on a two-person cuff: dropped. Publishing it would attribute a
  // reading to whichever of the two happens to be slot one.
  assert(standard_measurement_entity(good_measurement(), &two_users, "", user_index, entity) ==
         StandardPublishDecision::NO_USER_ID_MULTI_USER);

  // No user id and no profile at all - the live-only path, which exists for
  // cuffs nothing here can identify, so there is not even a user count to lean
  // on.
  assert(standard_measurement_entity(good_measurement(), nullptr, "", user_index, entity) ==
         StandardPublishDecision::NO_USER_ID_NO_PROFILE);

  // Named, and within the slots this node has.
  StandardBloodPressureMeasurement named = good_measurement();
  named.has_user_id = true;
  named.user_id = 2;
  assert(standard_measurement_entity(named, &two_users, "", user_index, entity) == StandardPublishDecision::PUBLISH);
  assert(user_index == 1 && entity.user == 2 && entity.has(OmronEntityCapability::USER));

  // 0xFF is the SIG's "unknown user", not a user. It has to fall through to the
  // same attribution rules rather than being taken as slot 255.
  StandardBloodPressureMeasurement unknown = good_measurement();
  unknown.has_user_id = true;
  unknown.user_id = 0xFF;
  assert(standard_measurement_entity(unknown, &two_users, "", user_index, entity) ==
         StandardPublishDecision::NO_USER_ID_MULTI_USER);

  // A user this node has no entity slot for.
  StandardBloodPressureMeasurement stranger = good_measurement();
  stranger.has_user_id = true;
  stranger.user_id = 7;
  assert(standard_measurement_entity(stranger, &two_users, "", user_index, entity) ==
         StandardPublishDecision::UNKNOWN_USER);

  // A single-user profile is the one safe fallback: there is nobody else it
  // could belong to.
  const OmronProfile &one_user = get_profile(OmronProfileId::HEM_9601T);
  assert(one_user.user_count == 1);
  assert(standard_measurement_entity(good_measurement(), &one_user, "", user_index, entity) ==
         StandardPublishDecision::PUBLISH);
  assert(user_index == 0);
  // Nobody named it, so the user entity stays unset rather than claiming 1.
  assert(!entity.has(OmronEntityCapability::USER));

  // Named as somebody that cuff does not store. Both ceilings apply, not just
  // this node's slot count: a second person on a one-person cuff has no entity
  // set to reach, so saying so beats writing into one nothing could configure.
  StandardBloodPressureMeasurement second = good_measurement();
  second.has_user_id = true;
  second.user_id = 2;
  assert(standard_measurement_entity(second, &one_user, "", user_index, entity) ==
         StandardPublishDecision::UNKNOWN_USER);
  // And the same id is fine on a cuff that does store two.
  assert(standard_measurement_entity(second, &two_users, "", user_index, entity) == StandardPublishDecision::PUBLISH);
}

void test_publish_standard_notification_ranges_and_status() {
  const OmronProfile &one_user = get_profile(OmronProfileId::HEM_9601T);
  uint8_t user_index = 0;
  OmronEntityData entity;

  StandardBloodPressureMeasurement half = good_measurement();
  half.has_diastolic = false;
  assert(standard_measurement_entity(half, &one_user, "", user_index, entity) == StandardPublishDecision::INCOMPLETE);

  // Diastolic above systolic is not a reading, whatever the bytes say.
  StandardBloodPressureMeasurement inverted = good_measurement();
  inverted.systolic = 80.0f;
  inverted.diastolic = 120.0f;
  assert(standard_measurement_entity(inverted, &one_user, "", user_index, entity) ==
         StandardPublishDecision::OUT_OF_RANGE);

  // kPa converts rather than being rejected: 16.8 kPa is 126 mmHg.
  StandardBloodPressureMeasurement kpa = good_measurement();
  kpa.units_kpa = true;
  kpa.systolic = 16.8f;
  kpa.diastolic = 11.3f;
  assert(standard_measurement_entity(kpa, &one_user, "", user_index, entity) == StandardPublishDecision::PUBLISH);
  assert(entity.systolic == 126 && entity.diastolic == 85);

  // A pulse outside the plausible range drops the field, not the reading.
  StandardBloodPressureMeasurement fast = good_measurement();
  fast.has_pulse_rate = true;
  fast.pulse_rate = 900.0f;
  assert(standard_measurement_entity(fast, &one_user, "", user_index, entity) == StandardPublishDecision::PUBLISH);
  assert(!entity.has(OmronEntityCapability::PULSE));
  assert(entity.has(OmronEntityCapability::SYSTOLIC));

  // Unlike a record, a standard notification does carry a position bit - bit 5
  // of the SIG measurement status - so this is the one path that may mark it.
  StandardBloodPressureMeasurement with_status = good_measurement();
  with_status.has_measurement_status = true;
  with_status.measurement_status = 0xFFFF;
  assert(standard_measurement_entity(with_status, &one_user, "", user_index, entity) ==
         StandardPublishDecision::PUBLISH);
  assert(entity.has(OmronEntityCapability::IMPROPER_POSITION));
  assert(entity.body_movement && entity.irregular_pulse && entity.improper_position);
  assert(!entity.cuff_fit);

  StandardBloodPressureMeasurement clean_status = good_measurement();
  clean_status.has_measurement_status = true;
  clean_status.measurement_status = 0x0000;
  assert(standard_measurement_entity(clean_status, &one_user, "", user_index, entity) ==
         StandardPublishDecision::PUBLISH);
  assert(entity.cuff_fit);
  assert(!entity.body_movement && !entity.irregular_pulse && !entity.improper_position);

  // A timestamp is claimed only when the notification carried one, so an entity
  // never shows a formatted string the cuff did not send.
  StandardBloodPressureMeasurement stamped = good_measurement();
  stamped.has_timestamp = true;
  assert(standard_measurement_entity(stamped, &one_user, "2026-08-13T16:06:18", user_index, entity) ==
         StandardPublishDecision::PUBLISH);
  assert(entity.has(OmronEntityCapability::TIMESTAMP) && entity.timestamp == "2026-08-13T16:06:18");
  // And the mirror: a formatted string offered for a notification that carried
  // no timestamp must not be claimed, or the entity shows a time the cuff never
  // sent.
  assert(standard_measurement_entity(good_measurement(), &one_user, "2026-08-13T16:06:18", user_index, entity) ==
         StandardPublishDecision::PUBLISH);
  assert(!entity.has(OmronEntityCapability::TIMESTAMP));
}

void test_publish_poll_outcome_names_the_right_cause() {
  HarvestResult harvest{};

  // The quiet case, and the common one: no ring was planned because no cursor
  // moved. This must not claim the cuff has no records - this session never
  // asked.
  assert(std::string(poll_outcome_status(harvest, 0, 0, false)) == "nothing new since the last session");

  // A ring was read and gave up nothing at all. That is an empty ring.
  assert(std::string(poll_outcome_status(harvest, 0, 0, true)) == "read complete; no measurement record stored");

  // Bytes came back and none of them decoded. This is what a profile aimed at
  // the wrong region looks like, and it has to say so rather than share a
  // sentence with an empty ring.
  harvest[0].unparsed = 3;
  assert(std::string(poll_outcome_status(harvest, 0, 0, true)) ==
         "read complete; no slot decoded - check the configured profile");

  // Everything decoded and the cut-off threw it away: the yaml names the wrong
  // date, not the wrong model.
  harvest = HarvestResult{};
  harvest[1].dropped_before_cutoff = 9;
  assert(std::string(poll_outcome_status(harvest, 0, 0, true)) ==
         "read complete; every record older than the configured cut-off");

  // Undecodable bytes outrank a cut-off: a wrong memory map is the more serious
  // of the two and the one somebody has to act on.
  harvest[0].unparsed = 1;
  assert(std::string(poll_outcome_status(harvest, 0, 0, true)) ==
         "read complete; no slot decoded - check the configured profile");

  // Decoded but nothing new to publish, and decoded with a fresh reading. The
  // counts win over everything above them: a session that published a record
  // must never report a cut-off or a decode failure that also happened.
  harvest = HarvestResult{};
  harvest[0].unparsed = 5;
  assert(std::string(poll_outcome_status(harvest, 1, 0, true)) == "read complete; latest measurement unchanged");
  assert(std::string(poll_outcome_status(harvest, 2, 1, true)) == "ok");
}

void test_publish_settings_entities() {
  const OmronProfile &profile = get_profile(OmronProfileId::HEM_7155T_MW3);
  // A user block in the shape this cuff returns one, at offset 34: a birth date
  // that belongs to nobody, and version counter 23. The date is made up on
  // purpose - the checksum is recomputed for it, so it still has to decode.
  std::vector<uint8_t> settings(60, 0x00);
  const uint8_t block[10] = {0x37, 0x0B, 0x05, 0x00, 0x17, 0x00, 0x00, 0x00, 0x5E, 0x00};
  for (size_t i = 0; i < sizeof(block); i++)
    settings[34 + i] = block[i];

  OmronUserSettingsData data{};
  assert(settings_entity_for_user(profile, 2, settings, data));
  assert(data.has(OmronEntityCapability::BIRTH_DATE) && data.birth_date == "1955-11-05");
  assert(data.has(OmronEntityCapability::SETTINGS_VERSION) && data.version == 23);

  // A user the profile does not have gets nothing rather than bytes from
  // wherever the arithmetic landed.
  assert(!settings_entity_for_user(profile, 3, settings, data));
  assert(!settings_entity_for_user(profile, 0, settings, data));
  assert(!settings_entity_for_user(profile, 1, {}, data));
}

// A model whose user block holds a birth date and nothing else.
//
// Two different answers have to come out of one block size. The date is there
// and publishes; the version counter is not, and an entity fed from it would
// read zero for the life of the node - the same zero an unregistered user shows
// on a model that does keep one. So the capability has to be withheld rather
// than published as a plausible number.
void test_publish_settings_where_the_block_keeps_no_counter() {
  const OmronProfile &profile = get_profile(OmronProfileId::HEM_7320T);
  assert(profile.user_block_size == 6);
  assert(!user_block_carries_version(profile));

  OmronSettingsBlock block{};
  assert(user_settings_block(profile, 1, block));
  std::vector<uint8_t> settings(profile.time_region_start, 0x00);
  settings[block.offset] = 0x37;
  settings[block.offset + 1] = 0x0B;
  settings[block.offset + 2] = 0x05;

  OmronUserSettingsData data{};
  assert(settings_entity_for_user(profile, 1, settings, data));
  assert(data.has(OmronEntityCapability::BIRTH_DATE) && data.birth_date == "1955-11-05");
  assert(!data.has(OmronEntityCapability::SETTINGS_VERSION));

  // And the profile grants exactly the one the block can fill. Both halves
  // matter: granting the counter leaves a permanently unknown entity, and
  // withholding the date takes away the only stored field these models have.
  const OmronEntityCapabilities lean = capabilities_for_profile(profile);
  assert((lean & entity_capability(OmronEntityCapability::BIRTH_DATE)) != 0);
  assert((lean & entity_capability(OmronEntityCapability::SETTINGS_VERSION)) == 0);

  // A counter-carrying layout still gets both, so this is a split rather than a
  // removal.
  const OmronEntityCapabilities full = capabilities_for_profile(get_profile(OmronProfileId::HEM_7155T_MW3));
  assert((full & entity_capability(OmronEntityCapability::BIRTH_DATE)) != 0);
  assert((full & entity_capability(OmronEntityCapability::SETTINGS_VERSION)) != 0);

  // Across the catalog, one way only: a profile may keep a counter and still be
  // refused the entity, because the block has to be locatable as well - but
  // nothing may be granted the entity without a counter behind it.
  for (size_t i = 0; i < profile_count(); i++) {
    const OmronProfile *entry = profile_at(i);
    assert(entry != nullptr);
    const OmronEntityCapabilities granted = capabilities_for_profile(*entry);
    if ((granted & entity_capability(OmronEntityCapability::SETTINGS_VERSION)) != 0)
      assert(user_block_carries_version(*entry));
  }
}
