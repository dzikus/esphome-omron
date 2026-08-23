#include "omron_entities.h"

#include <cmath>

#include "omron_ble_client.h"
#include "omron_metrics.h"
#include "omron_profiles.h"
#include "omron_switch.h"

namespace esphome::omron {

// The entity layer keeps its own slot count so it can stay clear of profile
// headers. Drift between the two would silently drop a user's entity set.
static_assert(OMRON_ENTITY_USER_SLOTS == OMRON_MAX_USERS, "entity user slots must match the profile catalog ceiling");

namespace {

// One overload set, so a table row can be published without naming the kind of
// entity it lands on.
void publish_entity(sensor::Sensor *entity, float value) {
  if (entity != nullptr)
    entity->publish_state(value);
}

void publish_entity(binary_sensor::BinarySensor *entity, bool value) {
  if (entity != nullptr)
    entity->publish_state(value);
}

void publish_entity(text_sensor::TextSensor *entity, const std::string &value) {
  if (entity != nullptr && !value.empty())
    entity->publish_state(value);
}

// Both gates, in one place, for either kind of payload: the profile has to be
// able to produce the field at all, and this particular read has to have
// actually decoded it. A template rather than an overload because the two data
// structs share nothing but that contract.
template <typename Data>
bool publishable(const Data &data, OmronEntityCapabilities capabilities, OmronEntityCapability capability) {
  return (capabilities & entity_capability(capability)) != 0 && data.has(capability);
}

// One capability's wiring: the entity of a person's set it fills, and how its
// value is reached. An accessor rather than a second pointer to member, so a
// two-bit field can feed a float sensor without a conversion table of its own.
template <typename Data, typename Entity, typename Value>
struct PublishRow {
  OmronEntityCapability capability;
  Entity *OmronUserEntities::*entity;
  Value (*value)(const Data &);
};

template <typename Row, size_t N>
constexpr OmronEntityCapabilities covered(const Row (&rows)[N]) {
  OmronEntityCapabilities mask = 0;
  for (const Row &row : rows)
    mask |= entity_capability(row.capability);
  return mask;
}

template <typename Row, size_t N, typename Data>
void publish_rows(const OmronUserEntities &user, const Row (&rows)[N], const Data &data,
                  OmronEntityCapabilities capabilities) {
  for (const Row &row : rows) {
    if (publishable(data, capabilities, row.capability))
      publish_entity(user.*row.entity, row.value(data));
  }
}

using RecordNumber = PublishRow<OmronEntityData, sensor::Sensor, float>;
using RecordFlag = PublishRow<OmronEntityData, binary_sensor::BinarySensor, bool>;
using RecordText = PublishRow<OmronEntityData, text_sensor::TextSensor, const std::string &>;

constexpr RecordNumber RECORD_NUMBERS[] = {
    {OmronEntityCapability::SYSTOLIC, &OmronUserEntities::systolic,
     [](const OmronEntityData &data) { return static_cast<float>(data.systolic); }},
    {OmronEntityCapability::DIASTOLIC, &OmronUserEntities::diastolic,
     [](const OmronEntityData &data) { return static_cast<float>(data.diastolic); }},
    {OmronEntityCapability::PULSE, &OmronUserEntities::pulse,
     [](const OmronEntityData &data) { return static_cast<float>(data.pulse); }},
    {OmronEntityCapability::USER, &OmronUserEntities::measurement_user,
     [](const OmronEntityData &data) { return static_cast<float>(data.user); }},
    {OmronEntityCapability::SEQUENCE, &OmronUserEntities::measurement_sequence,
     [](const OmronEntityData &data) { return static_cast<float>(data.sequence); }},
    {OmronEntityCapability::CONSECUTIVE_MEASUREMENT, &OmronUserEntities::consecutive_measurement,
     [](const OmronEntityData &data) { return static_cast<float>(data.consecutive_measurement); }},
    {OmronEntityCapability::ARTIFACT_DETECTION, &OmronUserEntities::artifact_detection,
     [](const OmronEntityData &data) { return static_cast<float>(data.artifact_detection); }},
    {OmronEntityCapability::IHB_DETECTION, &OmronUserEntities::ihb_detection,
     [](const OmronEntityData &data) { return static_cast<float>(data.ihb_detection); }},
};

constexpr RecordFlag RECORD_FLAGS[] = {
    {OmronEntityCapability::CUFF_FIT, &OmronUserEntities::cuff_fit,
     [](const OmronEntityData &data) { return data.cuff_fit; }},
    {OmronEntityCapability::BODY_MOVEMENT, &OmronUserEntities::body_movement,
     [](const OmronEntityData &data) { return data.body_movement; }},
    {OmronEntityCapability::IRREGULAR_PULSE, &OmronUserEntities::irregular_pulse,
     [](const OmronEntityData &data) { return data.irregular_pulse; }},
    {OmronEntityCapability::IMPROPER_POSITION, &OmronUserEntities::improper_position,
     [](const OmronEntityData &data) { return data.improper_position; }},
};

constexpr RecordText RECORD_TEXTS[] = {
    {OmronEntityCapability::TIMESTAMP, &OmronUserEntities::measurement_timestamp,
     [](const OmronEntityData &data) -> const std::string & { return data.timestamp; }},
};

using SettingsNumber = PublishRow<OmronUserSettingsData, sensor::Sensor, float>;
using SettingsText = PublishRow<OmronUserSettingsData, text_sensor::TextSensor, const std::string &>;

constexpr SettingsNumber SETTINGS_NUMBERS[] = {
    {OmronEntityCapability::SETTINGS_VERSION, &OmronUserEntities::settings_version,
     [](const OmronUserSettingsData &data) { return static_cast<float>(data.version); }},
};

constexpr SettingsText SETTINGS_TEXTS[] = {
    {OmronEntityCapability::BIRTH_DATE, &OmronUserEntities::birth_date,
     [](const OmronUserSettingsData &data) -> const std::string & { return data.birth_date; }},
};

// The point of the tables. A capability granted by a profile but wired to no
// entity publishes nothing, says nothing, and is invisible to the host suite
// because this file is not part of it.
static_assert((covered(RECORD_NUMBERS) | covered(RECORD_FLAGS) | covered(RECORD_TEXTS)) ==
                  OMRON_CAPABILITIES_USER_RECORD,
              "every capability a record carries needs a row above, or it never reaches an entity");
static_assert((covered(SETTINGS_NUMBERS) | covered(SETTINGS_TEXTS)) == OMRON_CAPABILITIES_USER_SETTINGS,
              "every stored setting needs a row above, or it never reaches an entity");

}  // namespace

void OmronBleSwitch::setup() {
  // On when nothing was stored, which is what the configured restore mode asks
  // for: a reboot must not leave the cuff quietly unreachable. A deliberate off
  // still survives, because that is a stored value rather than an absent one.
  // Written through write_state so the entity and the client agree from the
  // first second, instead of the client holding its default in silence.
  const optional<bool> restored = this->get_initial_state_with_restore_mode();
  this->write_state(restored.value_or(true));
}

void OmronBleSwitch::write_state(bool state) {
  // Published before the parent acts, so the entity reflects the request even
  // if tearing a session down takes a moment.
  this->publish_state(state);
  if (this->parent_ != nullptr)
    this->parent_->set_ble_user_enabled(state);
}

void OmronPollNowButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->request_poll();
}

void OmronPairButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->request_pairing();
}

void OmronForgetBondButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->forget_bond();
}

void OmronUserEntities::publish(const OmronEntityData &data, OmronEntityCapabilities capabilities) const {
  publish_rows(*this, RECORD_NUMBERS, data, capabilities);
  publish_rows(*this, RECORD_FLAGS, data, capabilities);
  publish_rows(*this, RECORD_TEXTS, data, capabilities);

  // One independent implementation owns all derived formulas and ACC/AHA
  // classification. These carry no capability of their own because they are not
  // fields of a record: only one that actually gave up all three primaries may
  // touch them.
  if (publishable(data, capabilities, OmronEntityCapability::SYSTOLIC) &&
      publishable(data, capabilities, OmronEntityCapability::DIASTOLIC) &&
      publishable(data, capabilities, OmronEntityCapability::PULSE)) {
    const DerivedMetrics metrics = calculate_derived_metrics(data.systolic, data.diastolic, data.pulse);
    // A measurement whose derived values cannot be computed still clears them,
    // rather than leaving the previous reading's numbers beside fresh primaries.
    const bool derived_valid = metrics.category != BloodPressureCategory::UNKNOWN;
    publish_entity(this->pulse_pressure, derived_valid ? metrics.pulse_pressure : NAN);
    publish_entity(this->estimated_map, derived_valid ? metrics.estimated_mean_arterial_pressure : NAN);
    publish_entity(this->shock_index, derived_valid ? metrics.shock_index : NAN);
    publish_entity(this->rate_pressure_product, derived_valid ? metrics.rate_pressure_product : NAN);
    if (this->blood_pressure_category != nullptr)
      this->blood_pressure_category->publish_state(blood_pressure_category_to_string(metrics.category));
  }
}

void OmronUserEntities::publish_settings(const OmronUserSettingsData &data,
                                         OmronEntityCapabilities capabilities) const {
  publish_rows(*this, SETTINGS_NUMBERS, data, capabilities);
  publish_rows(*this, SETTINGS_TEXTS, data, capabilities);
}

void OmronEntities::publish_user_settings_(uint8_t user_index, const OmronUserSettingsData &data) {
  if (user_index >= OMRON_ENTITY_USER_SLOTS)
    return;
  this->users_[user_index].publish_settings(data, this->entity_capabilities_);
}

void OmronEntities::publish_user_measurement_(uint8_t user_index, const OmronEntityData &data) {
  if (user_index >= OMRON_ENTITY_USER_SLOTS)
    return;
  this->users_[user_index].publish(data, this->entity_capabilities_);
  // Rides in on a record but describes the cuff, so it is published once here
  // rather than duplicated into every person's set.
  if (publishable(data, this->entity_capabilities_, OmronEntityCapability::BATTERY))
    publish_entity(this->battery_binary_sensor_, data.battery_low);
}

// No capability gate on these three: they come from the standard device
// information service, which is a property of the peer rather than of the
// profile we picked for it, and a profile that guessed wrong is exactly when
// reading them matters.
void OmronEntities::publish_model_number_entity_(const std::string &value) {
  publish_entity(this->model_number_text_sensor_, value);
}

void OmronEntities::publish_firmware_revision_entity_(const std::string &value) {
  publish_entity(this->firmware_revision_text_sensor_, value);
}

void OmronEntities::publish_serial_number_entity_(const std::string &value) {
  publish_entity(this->serial_number_text_sensor_, value);
}

void OmronEntities::publish_profile_entity_(const std::string &profile) {
  if (this->entity_capable_(OmronEntityCapability::PROFILE))
    publish_entity(this->profile_text_sensor_, profile);
}

void OmronEntities::publish_status_entity_(const std::string &status) {
  if (this->entity_capable_(OmronEntityCapability::STATUS))
    publish_entity(this->status_text_sensor_, status);
}

void OmronEntities::publish_rssi_entity_(float rssi) {
  if (this->entity_capable_(OmronEntityCapability::RSSI))
    publish_entity(this->rssi_sensor_, rssi);
}

void OmronEntities::publish_poll_duration_entity_(float seconds) {
  if (this->entity_capable_(OmronEntityCapability::POLL_DURATION))
    publish_entity(this->poll_duration_sensor_, seconds);
}

void OmronEntities::publish_clock_entities_(const std::string &device_clock, float drift_seconds) {
  publish_entity(this->device_clock_text_sensor_, device_clock);
  publish_entity(this->clock_drift_sensor_, drift_seconds);
}

void OmronEntities::publish_connection_entity_(bool connected) {
  if (this->entity_capable_(OmronEntityCapability::CONNECTION))
    publish_entity(this->connected_binary_sensor_, connected);
}

}  // namespace esphome::omron
