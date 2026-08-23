#pragma once

// What goes into an entity, decided without a radio.

#include <cstdint>
#include <span>
#include <string>

#include "omron_entity_data.h"
#include "omron_harvest.h"
#include "omron_measurement.h"
#include "omron_profiles.h"
#include "omron_settings_write.h"
#include "omron_standard_bp.h"

namespace esphome::omron {

// Which entities a profile can ever fill. Taken from the catalog and never from
// a model-name conditional, so a model that reports no cuff fit gets no cuff
// entity: an absent field publishes as a permanent zero, and a permanent zero
// on a cuff-fit entity looks exactly like a correctly fitted cuff.
OmronEntityCapabilities capabilities_for_profile(const OmronProfile &profile);

// The same for a cuff whose model could not be resolved. A detection flag is
// claimed only where 0x2A49 says the cuff performs it. Pass nullptr when that
// read never answered, which claims all four rather than none.
OmronEntityCapabilities capabilities_for_live_only(const StandardBpFeatures *features);

// One decoded record as entity values. The timestamp arrives formatted, since a
// zone offset needs the node's time source. IMPROPER_POSITION is deliberately
// never marked: no stored record carries that field on any model.
OmronEntityData entity_from_record(const OmronMeasurement &measurement, uint8_t user_index,
                                   const std::string &timestamp);

enum class StandardPublishDecision : uint8_t {
  PUBLISH = 0,
  // The notification did not carry both pressures.
  INCOMPLETE,
  // Decoded, but outside what a blood pressure reading can be.
  OUT_OF_RANGE,
  // A user id this node has no entity slot for.
  UNKNOWN_USER,
  // No user id and no profile to say how many users exist. Live-only: filing it
  // under slot one would attribute a reading to whoever happens to be first.
  NO_USER_ID_NO_PROFILE,
  // No user id on a cuff that stores more than one person.
  NO_USER_ID_MULTI_USER,
};

const char *standard_publish_decision_to_string(StandardPublishDecision decision);

// `profile` may be null, which is the live-only case. `timestamp` is used only
// when the notification carried one.
StandardPublishDecision standard_measurement_entity(const StandardBloodPressureMeasurement &measurement,
                                                    const OmronProfile *profile, const std::string &timestamp,
                                                    uint8_t &user_index, OmronEntityData &out);

// The two fields the cuff stores about a person. False when the profile has no
// block for that user.
[[nodiscard]] bool settings_entity_for_user(const OmronProfile &profile, uint8_t user_number,
                                            std::span<const uint8_t> settings, OmronUserSettingsData &out);

// What the status entity says a completed transfer amounted to. Four situations
// look alike and mean different things: an empty ring and an unmoved cursor are
// normal, a full ring that decodes to nothing means the wrong model is
// configured, and readings dropped whole mean the wrong cut-off date.
//
// `read_anything` is whether the session planned any ring at all; false is the
// quiet case, a button press with no new measurement behind it.
const char *poll_outcome_status(const HarvestResult &harvest, uint8_t users_decoded, uint8_t users_published,
                                bool read_anything);

}  // namespace esphome::omron
