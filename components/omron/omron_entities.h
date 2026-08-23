#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

// The values themselves, in a header with no ESPHome in it. What fills them
// lives in omron_publish.*, which is compiled and tested on the host.
#include "omron_entity_data.h"

namespace esphome::omron {

// The transport owner is deliberately incomplete here. OmronBLEClient includes
// this header and holds an OmronEntities; this file never includes the owner,
// BLE, or model-profile headers.
class OmronBLEClient;

// Manual trigger for the ordinary session. The owner decides whether to connect
// immediately or wait for the next matching advertisement; nothing leaves for
// the cuff on the press itself.
class OmronPollNowButton : public button::Button, public Parented<OmronBLEClient> {
 protected:
  void press_action() override;
};

// The one button that changes the cuff: it programs the configured bind key so
// later sessions authenticate with it instead of needing the pairing button
// held down every time.
class OmronPairButton : public button::Button, public Parented<OmronBLEClient> {
 protected:
  void press_action() override;
};

// Drops this node's bond record so the next session pairs from scratch. Nothing
// leaves for the cuff on the press itself; what it buys is a session that
// registers, which is the only kind the cuff takes a user block from.
class OmronForgetBondButton : public button::Button, public Parented<OmronBLEClient> {
 protected:
  void press_action() override;
};

// One person's entities. A cuff that stores two users keeps two of these, and a
// record reaches only the set belonging to the user it was stored under. The
// dividing line is the record itself: every field decoded out of a measurement
// belongs to whoever took it, everything about the radio link belongs to the
// cuff and lives on OmronEntities instead.
struct OmronUserEntities {
  sensor::Sensor *systolic{nullptr};
  sensor::Sensor *diastolic{nullptr};
  sensor::Sensor *pulse{nullptr};
  sensor::Sensor *pulse_pressure{nullptr};
  sensor::Sensor *estimated_map{nullptr};
  sensor::Sensor *shock_index{nullptr};
  sensor::Sensor *rate_pressure_product{nullptr};
  sensor::Sensor *measurement_user{nullptr};
  sensor::Sensor *measurement_sequence{nullptr};
  sensor::Sensor *consecutive_measurement{nullptr};
  sensor::Sensor *artifact_detection{nullptr};
  sensor::Sensor *ihb_detection{nullptr};
  sensor::Sensor *settings_version{nullptr};

  text_sensor::TextSensor *measurement_timestamp{nullptr};
  text_sensor::TextSensor *blood_pressure_category{nullptr};
  text_sensor::TextSensor *birth_date{nullptr};

  binary_sensor::BinarySensor *cuff_fit{nullptr};
  binary_sensor::BinarySensor *body_movement{nullptr};
  binary_sensor::BinarySensor *irregular_pulse{nullptr};
  binary_sensor::BinarySensor *improper_position{nullptr};

  // Set by bind_, the single funnel every per-user setter goes through, rather
  // than recomputed from the members above. Enumerating them by hand leaves a
  // member added later and forgotten here reading as unwired, so a block that
  // configures only that entity receives nothing and says nothing.
  bool any_bound{false};

  bool bound() const { return this->any_bound; }
  void publish(const OmronEntityData &data, OmronEntityCapabilities capabilities) const;
  void publish_settings(const OmronUserSettingsData &data, OmronEntityCapabilities capabilities) const;
};

// Mixin owned by OmronBLEClient. Codegen only calls the public setters. The
// owner selects a profile capability mask and invokes the protected publishing
// helpers after a complete record or lifecycle operation has been validated.
class OmronEntities {
 public:
  void set_entity_capabilities(OmronEntityCapabilities capabilities) { this->entity_capabilities_ = capabilities; }

  // Zero-based, matching the poll layout. Codegen converts from the one-based
  // `user` written in yaml.
  void set_systolic_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::systolic, entity);
  }
  void set_diastolic_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::diastolic, entity);
  }
  void set_pulse_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::pulse, entity);
  }
  void set_pulse_pressure_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::pulse_pressure, entity);
  }
  void set_estimated_map_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::estimated_map, entity);
  }
  void set_shock_index_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::shock_index, entity);
  }
  void set_rate_pressure_product_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::rate_pressure_product, entity);
  }
  void set_measurement_user_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::measurement_user, entity);
  }
  void set_measurement_sequence_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::measurement_sequence, entity);
  }
  void set_consecutive_measurement_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::consecutive_measurement, entity);
  }
  void set_artifact_detection_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::artifact_detection, entity);
  }
  void set_ihb_detection_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::ihb_detection, entity);
  }
  void set_measurement_timestamp_text_sensor(uint8_t user_index, text_sensor::TextSensor *entity) {
    this->bind_(user_index, &OmronUserEntities::measurement_timestamp, entity);
  }
  void set_blood_pressure_category_text_sensor(uint8_t user_index, text_sensor::TextSensor *entity) {
    this->bind_(user_index, &OmronUserEntities::blood_pressure_category, entity);
  }
  void set_birth_date_text_sensor(uint8_t user_index, text_sensor::TextSensor *entity) {
    this->bind_(user_index, &OmronUserEntities::birth_date, entity);
  }
  void set_settings_version_sensor(uint8_t user_index, sensor::Sensor *entity) {
    this->bind_(user_index, &OmronUserEntities::settings_version, entity);
  }
  void set_cuff_fit_binary_sensor(uint8_t user_index, binary_sensor::BinarySensor *entity) {
    this->bind_(user_index, &OmronUserEntities::cuff_fit, entity);
  }
  void set_body_movement_binary_sensor(uint8_t user_index, binary_sensor::BinarySensor *entity) {
    this->bind_(user_index, &OmronUserEntities::body_movement, entity);
  }
  void set_irregular_pulse_binary_sensor(uint8_t user_index, binary_sensor::BinarySensor *entity) {
    this->bind_(user_index, &OmronUserEntities::irregular_pulse, entity);
  }
  void set_improper_position_binary_sensor(uint8_t user_index, binary_sensor::BinarySensor *entity) {
    this->bind_(user_index, &OmronUserEntities::improper_position, entity);
  }
  void set_battery_binary_sensor(binary_sensor::BinarySensor *entity) { this->battery_binary_sensor_ = entity; }

  void set_rssi_sensor(sensor::Sensor *entity) { this->rssi_sensor_ = entity; }
  void set_poll_duration_sensor(sensor::Sensor *entity) { this->poll_duration_sensor_ = entity; }
  void set_clock_drift_sensor(sensor::Sensor *entity) { this->clock_drift_sensor_ = entity; }
  void set_device_clock_text_sensor(text_sensor::TextSensor *entity) { this->device_clock_text_sensor_ = entity; }
  // Read once from the standard device information service. The model number is
  // what identification works from; the firmware revision is diagnostic only,
  // because the string this cuff answers with names no variant in any catalog
  // and only says what changed after an update.
  void set_model_number_text_sensor(text_sensor::TextSensor *entity) { this->model_number_text_sensor_ = entity; }
  void set_firmware_revision_text_sensor(text_sensor::TextSensor *entity) {
    this->firmware_revision_text_sensor_ = entity;
  }
  void set_serial_number_text_sensor(text_sensor::TextSensor *entity) { this->serial_number_text_sensor_ = entity; }
  void set_profile_text_sensor(text_sensor::TextSensor *entity) { this->profile_text_sensor_ = entity; }
  void set_status_text_sensor(text_sensor::TextSensor *entity) { this->status_text_sensor_ = entity; }
  void set_connected_binary_sensor(binary_sensor::BinarySensor *entity) { this->connected_binary_sensor_ = entity; }

 protected:
  // The owner drives every publish below. Friendship rather than a public API,
  // so the only caller is the class that holds this one.
  friend class OmronBLEClient;

  void publish_user_measurement_(uint8_t user_index, const OmronEntityData &data);
  // Separate from the measurement path because it runs on a different trigger:
  // every session that reads the settings region, whether or not a record came
  // with it.
  void publish_user_settings_(uint8_t user_index, const OmronUserSettingsData &data);
  void publish_profile_entity_(const std::string &profile);
  void publish_model_number_entity_(const std::string &value);
  void publish_firmware_revision_entity_(const std::string &value);
  void publish_serial_number_entity_(const std::string &value);
  void publish_status_entity_(const std::string &status);
  void publish_rssi_entity_(float rssi);
  void publish_poll_duration_entity_(float seconds);
  void publish_connection_entity_(bool connected);
  void publish_clock_entities_(const std::string &device_clock, float drift_seconds);

  bool user_entities_bound_(uint8_t user_index) const {
    return user_index < OMRON_ENTITY_USER_SLOTS && this->users_[user_index].bound();
  }

  bool entity_capable_(OmronEntityCapability capability) const {
    return (this->entity_capabilities_ & entity_capability(capability)) != 0;
  }

  OmronEntityCapabilities entity_capabilities_{0};

  std::array<OmronUserEntities, OMRON_ENTITY_USER_SLOTS> users_{};

  sensor::Sensor *rssi_sensor_{nullptr};
  sensor::Sensor *poll_duration_sensor_{nullptr};
  sensor::Sensor *clock_drift_sensor_{nullptr};
  text_sensor::TextSensor *device_clock_text_sensor_{nullptr};
  text_sensor::TextSensor *model_number_text_sensor_{nullptr};
  text_sensor::TextSensor *firmware_revision_text_sensor_{nullptr};
  text_sensor::TextSensor *serial_number_text_sensor_{nullptr};
  text_sensor::TextSensor *profile_text_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  binary_sensor::BinarySensor *battery_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_binary_sensor_{nullptr};

 private:
  template <typename T>
  void bind_(uint8_t user_index, T OmronUserEntities::*member, T entity) {
    if (user_index >= OMRON_ENTITY_USER_SLOTS)
      return;
    this->users_[user_index].*member = entity;
    this->users_[user_index].any_bound = this->users_[user_index].any_bound || entity != nullptr;
  }
};

}  // namespace esphome::omron
