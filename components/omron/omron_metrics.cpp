#include "omron_metrics.h"

namespace esphome::omron {

bool plausible_vitals(uint16_t systolic, uint16_t diastolic, uint16_t pulse) {
  return systolic >= OMRON_MIN_SYSTOLIC && systolic <= OMRON_MAX_SYSTOLIC && diastolic >= OMRON_MIN_DIASTOLIC &&
         diastolic <= OMRON_MAX_DIASTOLIC && pulse >= OMRON_MIN_PULSE && pulse <= OMRON_MAX_PULSE &&
         systolic > diastolic;
}

DerivedMetrics calculate_derived_metrics(uint16_t systolic, uint16_t diastolic, uint16_t pulse) {
  DerivedMetrics metrics;
  if (!plausible_vitals(systolic, diastolic, pulse))
    return metrics;

  metrics.pulse_pressure = static_cast<float>(systolic - diastolic);
  metrics.estimated_mean_arterial_pressure =
      (static_cast<float>(systolic) + 2.0f * static_cast<float>(diastolic)) / 3.0f;
  metrics.shock_index = static_cast<float>(pulse) / static_cast<float>(systolic);
  metrics.rate_pressure_product = static_cast<float>(systolic) * static_cast<float>(pulse);

  // ACC/AHA 2017 classification. Where systolic and diastolic disagree, the
  // more severe category wins.
  if (systolic > 180 || diastolic > 120) {
    metrics.category = BloodPressureCategory::HYPERTENSIVE_CRISIS;
  } else if (systolic >= 140 || diastolic >= 90) {
    metrics.category = BloodPressureCategory::HYPERTENSION_STAGE_2;
  } else if (systolic >= 130 || diastolic >= 80) {
    metrics.category = BloodPressureCategory::HYPERTENSION_STAGE_1;
  } else if (systolic >= 120) {
    metrics.category = BloodPressureCategory::ELEVATED;
  } else {
    metrics.category = BloodPressureCategory::NORMAL;
  }
  return metrics;
}

const char *blood_pressure_category_to_string(BloodPressureCategory category) {
  switch (category) {
    case BloodPressureCategory::NORMAL:
      return "Normal";
    case BloodPressureCategory::ELEVATED:
      return "Elevated";
    case BloodPressureCategory::HYPERTENSION_STAGE_1:
      return "Hypertension Stage 1";
    case BloodPressureCategory::HYPERTENSION_STAGE_2:
      return "Hypertension Stage 2";
    case BloodPressureCategory::HYPERTENSIVE_CRISIS:
      return "Hypertensive Crisis";
    case BloodPressureCategory::UNKNOWN:
      return "Unknown";
  }
  return "Unknown";
}

}  // namespace esphome::omron
