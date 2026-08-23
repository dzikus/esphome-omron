from typing import NamedTuple

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_BEATS_PER_MINUTE,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_EMPTY,
    UNIT_SECOND,
)

from . import (
    CONF_OMRON_ID,
    OMRON_ENTITY_PLATFORM_SCHEMA,
    SCOPE_HUB,
    SCOPE_USER,
    inject_entity_defaults,
    omron_user_index,
    resolve_entity_block,
)

DEPENDENCIES = ["omron"]
CODEOWNERS = ["@dzikus"]

# Deliberately published without DEVICE_CLASS_PRESSURE. Home Assistant registers
# that device class with its unit converters, so a display-unit or unit-system
# preference of hPa would rescale 120 mmHg to about 160 and store long-term
# statistics in the converted unit.
UNIT_MILLIMETER_OF_MERCURY = "mmHg"
UNIT_SHOCK_INDEX = "ratio"
UNIT_RATE_PRESSURE_PRODUCT = "mmHg*bpm"


class Row(NamedTuple):
    """One sensor's whole definition.

    Named rather than a plain tuple because this table is unpacked in three
    places: a column added to a bare tuple would be read as the wrong field
    there instead of failing.
    """

    key: str
    setter: str
    scope: str
    unit: str
    decimals: int
    device_class: str | None
    state_class: str | None
    icon: str | None
    entity_category: str | None
    default_name: str


# Every entity is created for every block. Runtime profile capabilities and the
# per-record availability mask decide whether a state is published. This keeps
# model knowledge out of the entity layer and leaves unsupported entities in
# the unknown state instead of publishing invented zero/false values.
SENSORS = [
    Row(
        "systolic",
        "set_systolic_sensor",
        SCOPE_USER,
        UNIT_MILLIMETER_OF_MERCURY,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:arrow-up-bold",
        None,
        "Systolic blood pressure",
    ),
    Row(
        "diastolic",
        "set_diastolic_sensor",
        SCOPE_USER,
        UNIT_MILLIMETER_OF_MERCURY,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:arrow-down-bold",
        None,
        "Diastolic blood pressure",
    ),
    Row(
        "pulse",
        "set_pulse_sensor",
        SCOPE_USER,
        UNIT_BEATS_PER_MINUTE,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:heart-pulse",
        None,
        "Pulse",
    ),
    Row(
        "pulse_pressure",
        "set_pulse_pressure_sensor",
        SCOPE_USER,
        UNIT_MILLIMETER_OF_MERCURY,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:heart-plus",
        None,
        "Pulse pressure",
    ),
    Row(
        "estimated_mean_arterial_pressure",
        "set_estimated_map_sensor",
        SCOPE_USER,
        UNIT_MILLIMETER_OF_MERCURY,
        1,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:gauge",
        None,
        "Estimated MAP",
    ),
    Row(
        "shock_index",
        "set_shock_index_sensor",
        SCOPE_USER,
        UNIT_SHOCK_INDEX,
        2,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:heart-flash",
        None,
        "Shock index",
    ),
    Row(
        "rate_pressure_product",
        "set_rate_pressure_product_sensor",
        SCOPE_USER,
        UNIT_RATE_PRESSURE_PRODUCT,
        1,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:heart-cog",
        None,
        "Rate pressure product",
    ),
    Row(
        "measurement_user",
        "set_measurement_user_sensor",
        SCOPE_USER,
        UNIT_EMPTY,
        0,
        None,
        None,
        "mdi:account",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Measurement user",
    ),
    Row(
        "measurement_sequence",
        "set_measurement_sequence_sensor",
        SCOPE_USER,
        UNIT_EMPTY,
        0,
        None,
        None,
        "mdi:counter",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Measurement sequence",
    ),
    # Consecutive measurement, artifact detection, IHB detection. The first is a
    # count, not a flag: it is the index within a TruRead series, and publishing
    # it as a binary "improper position" reports a different field entirely.
    Row(
        "consecutive_measurement",
        "set_consecutive_measurement_sensor",
        SCOPE_USER,
        UNIT_EMPTY,
        0,
        None,
        None,
        "mdi:numeric-3-box-multiple",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Consecutive measurement",
    ),
    Row(
        "artifact_detection",
        "set_artifact_detection_sensor",
        SCOPE_USER,
        UNIT_EMPTY,
        0,
        None,
        None,
        "mdi:waveform",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Artifact detection",
    ),
    Row(
        "ihb_detection",
        "set_ihb_detection_sensor",
        SCOPE_USER,
        UNIT_EMPTY,
        0,
        None,
        None,
        "mdi:heart-pulse",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "IHB detection",
    ),
    # Per user. The cuff compares it before it will take a block, so it is the
    # number that says whether a registration write landed. It
    # should move once per pairing and stay still otherwise: a value climbing
    # on every poll means the registration gate broke and each read is spending
    # an EEPROM write.
    Row(
        "settings_version",
        "set_settings_version_sensor",
        SCOPE_USER,
        UNIT_EMPTY,
        0,
        None,
        None,
        "mdi:counter",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Settings version in cuff",
    ),
    Row(
        "rssi",
        "set_rssi_sensor",
        SCOPE_HUB,
        UNIT_DECIBEL_MILLIWATT,
        0,
        DEVICE_CLASS_SIGNAL_STRENGTH,
        STATE_CLASS_MEASUREMENT,
        "mdi:signal",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Signal strength",
    ),
    # Signed seconds, so no duration device class: Home Assistant forbids
    # negatives there, and a cuff running behind real time is the normal case.
    Row(
        "clock_drift",
        "set_clock_drift_sensor",
        SCOPE_HUB,
        UNIT_SECOND,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:clock-alert-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Clock drift",
    ),
    Row(
        "poll_duration",
        "set_poll_duration_sensor",
        SCOPE_HUB,
        UNIT_SECOND,
        1,
        DEVICE_CLASS_DURATION,
        STATE_CLASS_MEASUREMENT,
        "mdi:timer-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Last poll duration",
    ),
]


def _sensor_schema(unit, decimals, device_class, state_class, icon, entity_category):
    kwargs = {
        "unit_of_measurement": unit,
        "accuracy_decimals": decimals,
    }
    if device_class is not None:
        kwargs["device_class"] = device_class
    if state_class is not None:
        kwargs["state_class"] = state_class
    if icon is not None:
        kwargs["icon"] = icon
    if entity_category is not None:
        kwargs["entity_category"] = entity_category
    return sensor.sensor_schema(**kwargs)


_DEFAULT_ROWS = [(row.key, row.default_name, row.scope) for row in SENSORS]


def _inject_defaults(config):
    return inject_entity_defaults(config, _DEFAULT_ROWS)


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OMRON_ENTITY_PLATFORM_SCHEMA.extend(
        {
            cv.Optional(row.key): _sensor_schema(
                row.unit,
                row.decimals,
                row.device_class,
                row.state_class,
                row.icon,
                row.entity_category,
            )
            for row in SENSORS
        }
    ),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OMRON_ID])
    config = resolve_entity_block(config, _DEFAULT_ROWS)
    user_index = omron_user_index(config)
    # The client holds its entities rather than being them, so binding goes
    # through the accessor.
    entities = parent.entities()

    for row in SENSORS:
        sub = config.get(row.key)
        if sub is None:
            continue
        entity = await sensor.new_sensor(sub)
        if row.scope == SCOPE_USER:
            cg.add(getattr(entities, row.setter)(user_index, entity))
        else:
            cg.add(getattr(entities, row.setter)(entity))
