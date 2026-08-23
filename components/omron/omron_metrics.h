#pragma once

#include <cstdint>

namespace esphome::omron {

// Derived values. They never alter the raw measurement and do not depend on the
// selected profile.

// Corruption bounds, not clinical thresholds. Every layer that decides whether
// a reading may be published shares these, so a value that clears the decoder
// cannot later be rejected by the derived-metric gate.
inline constexpr uint16_t OMRON_MIN_SYSTOLIC = 60;
inline constexpr uint16_t OMRON_MAX_SYSTOLIC = 280;
inline constexpr uint16_t OMRON_MIN_DIASTOLIC = 30;
inline constexpr uint16_t OMRON_MAX_DIASTOLIC = 180;
inline constexpr uint16_t OMRON_MIN_PULSE = 30;
inline constexpr uint16_t OMRON_MAX_PULSE = 240;

enum class BloodPressureCategory : uint8_t {
  UNKNOWN = 0,
  NORMAL,
  ELEVATED,
  HYPERTENSION_STAGE_1,
  HYPERTENSION_STAGE_2,
  HYPERTENSIVE_CRISIS,
};

struct DerivedMetrics {
  float pulse_pressure{0.0f};
  float estimated_mean_arterial_pressure{0.0f};
  float shock_index{0.0f};
  float rate_pressure_product{0.0f};
  BloodPressureCategory category{BloodPressureCategory::UNKNOWN};
};

bool plausible_vitals(uint16_t systolic, uint16_t diastolic, uint16_t pulse);
DerivedMetrics calculate_derived_metrics(uint16_t systolic, uint16_t diastolic, uint16_t pulse);
const char *blood_pressure_category_to_string(BloodPressureCategory category);

}  // namespace esphome::omron
