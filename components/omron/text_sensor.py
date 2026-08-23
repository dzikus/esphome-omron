from typing import NamedTuple

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import (
    CONF_DEVICE_CLASS,
    CONF_ID,
    CONF_TIME_ID,
    DEVICE_CLASS_TIMESTAMP,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE

from . import (
    CONF_OMRON_ID,
    OMRON_ENTITY_PLATFORM_SCHEMA,
    SCOPE_HUB,
    SCOPE_USER,
    inject_entity_defaults,
    omron_user_index,
    resolve_entity_block,
)


def _hub_has_time(hub_id):
    # The measured-at value can only carry a UTC offset when there is a clock to
    # ask for one, and Home Assistant refuses a timestamp without it. So the
    # device class follows the hub's time source rather than being declared and
    # hoped for.
    target = str(hub_id)
    for hub_conf in CORE.config.get("omron", []):
        if str(hub_conf.get(CONF_ID)) == target:
            return CONF_TIME_ID in hub_conf
    return False


DEPENDENCIES = ["omron"]
CODEOWNERS = ["@dzikus"]


class Row(NamedTuple):
    """One text sensor's whole definition.

    Named rather than a plain tuple because this table is unpacked in three
    places: a column added to a bare tuple would be read as the wrong field
    there instead of failing.
    """

    key: str
    setter: str
    scope: str
    icon: str | None
    entity_category: str | None
    default_name: str


TEXT_SENSORS = [
    # The timestamp device class is attached in to_code, not here, and only when
    # the hub has a time source. The cuff stores local wall time with no zone;
    # with a clock present the component appends the offset that applied on the
    # record's own date, and Home Assistant then treats the value as a real
    # timestamp. Without one the value stays naive and must stay classless,
    # because that class refuses anything zoneless and renders unknown.
    Row(
        "measurement_timestamp",
        "set_measurement_timestamp_text_sensor",
        SCOPE_USER,
        "mdi:clock-outline",
        None,
        "Measured at",
    ),
    Row(
        "blood_pressure_category",
        "set_blood_pressure_category_text_sensor",
        SCOPE_USER,
        "mdi:heart-box-outline",
        None,
        "BP category (ACC-AHA)",
    ),
    # Not a measurement: it is what the cuff stores about this person, and the
    # one value in the device this component writes, so reading it back is how
    # a registration is confirmed without watching the log. An unregistered
    # slot reads 1900-01-01, which is the erased block saying so.
    Row(
        "birth_date",
        "set_birth_date_text_sensor",
        SCOPE_USER,
        "mdi:cake-variant",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Birth date in cuff",
    ),
    # Read once per boot from the standard device information service, which
    # this cuff exposes at 0x180A. Identification works from the model number;
    # the firmware revision is diagnostic only, since the string these cuffs
    # answer with names no variant in any catalog and only says what an update
    # changed.
    Row(
        "model_number",
        "set_model_number_text_sensor",
        SCOPE_HUB,
        "mdi:tag-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Model number",
    ),
    Row(
        "firmware_revision",
        "set_firmware_revision_text_sensor",
        SCOPE_HUB,
        "mdi:chip",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Firmware revision",
    ),
    Row(
        "serial_number",
        "set_serial_number_text_sensor",
        SCOPE_HUB,
        "mdi:identifier",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Serial number",
    ),
    Row(
        "device_clock",
        "set_device_clock_text_sensor",
        SCOPE_HUB,
        "mdi:clock-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Device clock",
    ),
    Row(
        "profile",
        "set_profile_text_sensor",
        SCOPE_HUB,
        "mdi:identifier",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Profile",
    ),
    Row(
        "status",
        "set_status_text_sensor",
        SCOPE_HUB,
        "mdi:list-status",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Measurement status",
    ),
]


def _text_sensor_schema(icon, entity_category):
    kwargs = {}
    if icon is not None:
        kwargs["icon"] = icon
    if entity_category is not None:
        kwargs["entity_category"] = entity_category
    return text_sensor.text_sensor_schema(**kwargs)


_DEFAULT_ROWS = [(row.key, row.default_name, row.scope) for row in TEXT_SENSORS]


def _inject_defaults(config):
    return inject_entity_defaults(config, _DEFAULT_ROWS)


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OMRON_ENTITY_PLATFORM_SCHEMA.extend(
        {
            cv.Optional(row.key): _text_sensor_schema(row.icon, row.entity_category)
            for row in TEXT_SENSORS
        }
    ),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OMRON_ID])
    config = resolve_entity_block(config, _DEFAULT_ROWS)
    user_index = omron_user_index(config)
    entities = parent.entities()

    zoned = _hub_has_time(config[CONF_OMRON_ID])
    for row in TEXT_SENSORS:
        sub = config.get(row.key)
        if sub is None:
            continue
        if row.key == "measurement_timestamp" and zoned:
            # Into the config rather than through a setter: this ESPHome interns
            # device classes into a string table at codegen and generates no
            # runtime call for them, so new_text_sensor has to see it.
            sub = {**sub, CONF_DEVICE_CLASS: DEVICE_CLASS_TIMESTAMP}
        entity = await text_sensor.new_text_sensor(sub)
        if row.scope == SCOPE_USER:
            cg.add(getattr(entities, row.setter)(user_index, entity))
        else:
            cg.add(getattr(entities, row.setter)(entity))
