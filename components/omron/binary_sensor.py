from typing import NamedTuple

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
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


class Row(NamedTuple):
    """One binary sensor's whole definition.

    Named rather than a plain tuple because this table is unpacked in three
    places: a column added to a bare tuple would be read as the wrong field
    there instead of failing.
    """

    key: str
    setter: str
    scope: str
    device_class: str | None
    icon: str | None
    entity_category: str | None
    default_name: str


# cuff_fit is the one entity here with no device class, and that is deliberate:
# ON means the cuff was wrapped CORRECTLY, so `problem` would invert it. A loose
# wrap is reported by clearing the bit, not by setting it. The other three are
# problem-true. See OmronEntityData.
BINARY_SENSORS = [
    Row(
        "cuff_fit",
        "set_cuff_fit_binary_sensor",
        SCOPE_USER,
        None,
        "mdi:arm-flex",
        None,
        "Cuff fit",
    ),
    Row(
        "body_movement",
        "set_body_movement_binary_sensor",
        SCOPE_USER,
        DEVICE_CLASS_PROBLEM,
        "mdi:account-multiple",
        None,
        "Body movement",
    ),
    Row(
        "irregular_pulse",
        "set_irregular_pulse_binary_sensor",
        SCOPE_USER,
        DEVICE_CLASS_PROBLEM,
        "mdi:heart-multiple",
        None,
        "Irregular pulse",
    ),
    Row(
        "improper_position",
        "set_improper_position_binary_sensor",
        SCOPE_USER,
        DEVICE_CLASS_PROBLEM,
        "mdi:seat-recline-normal",
        None,
        "Improper position",
    ),
    # Hub scope, not user scope. The flag arrives inside a measurement record,
    # but where a value is stored says nothing about whose it is, and two people
    # do not have separate batteries.
    Row(
        "battery",
        "set_battery_binary_sensor",
        SCOPE_HUB,
        DEVICE_CLASS_BATTERY,
        None,
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Battery low",
    ),
    Row(
        "connected",
        "set_connected_binary_sensor",
        SCOPE_HUB,
        DEVICE_CLASS_CONNECTIVITY,
        None,
        ENTITY_CATEGORY_DIAGNOSTIC,
        "BLE connected",
    ),
]


def _binary_sensor_schema(device_class, icon, entity_category):
    kwargs = {}
    if device_class is not None:
        kwargs["device_class"] = device_class
    if icon is not None:
        kwargs["icon"] = icon
    if entity_category is not None:
        kwargs["entity_category"] = entity_category
    return binary_sensor.binary_sensor_schema(**kwargs)


_DEFAULT_ROWS = [(row.key, row.default_name, row.scope) for row in BINARY_SENSORS]


def _inject_defaults(config):
    return inject_entity_defaults(config, _DEFAULT_ROWS)


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OMRON_ENTITY_PLATFORM_SCHEMA.extend(
        {
            cv.Optional(row.key): _binary_sensor_schema(
                row.device_class, row.icon, row.entity_category
            )
            for row in BINARY_SENSORS
        }
    ),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OMRON_ID])
    config = resolve_entity_block(config, _DEFAULT_ROWS)
    user_index = omron_user_index(config)
    entities = parent.entities()

    for row in BINARY_SENSORS:
        sub = config.get(row.key)
        if sub is None:
            continue
        entity = await binary_sensor.new_binary_sensor(sub)
        if row.scope == SCOPE_USER:
            cg.add(getattr(entities, row.setter)(user_index, entity))
        else:
            cg.add(getattr(entities, row.setter)(entity))
