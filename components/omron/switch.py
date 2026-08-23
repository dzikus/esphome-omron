import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import (
    CONF_OMRON_ID,
    OMRON_ENTITY_PLATFORM_SCHEMA,
    SCOPE_HUB,
    inject_entity_defaults,
    omron_ns,
    resolve_entity_block,
)

# The cuff's radio, not a person's, and not a setting on the device: this switch
# writes nothing to the cuff. Off means this node stands down, which is what the
# official app needs, since the cuff answers a second concurrent connection with
# an SMP failure rather than sharing.

DEPENDENCIES = ["omron"]
CODEOWNERS = ["@dzikus"]

CONF_BLUETOOTH = "bluetooth"

OmronBleSwitch = omron_ns.class_(
    "OmronBleSwitch", switch.Switch, cg.Component, cg.Parented
)


_DEFAULT_ROWS = [(CONF_BLUETOOTH, "Bluetooth", SCOPE_HUB)]


def _inject_defaults(config):
    return inject_entity_defaults(config, _DEFAULT_ROWS)


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OMRON_ENTITY_PLATFORM_SCHEMA.extend(
        {
            cv.Optional(CONF_BLUETOOTH): switch.switch_schema(
                OmronBleSwitch,
                icon="mdi:bluetooth",
                entity_category=ENTITY_CATEGORY_CONFIG,
                # A reboot must never leave the cuff quietly unreachable, so the
                # restored default is on rather than off.
                default_restore_mode="RESTORE_DEFAULT_ON",
            ),
        }
    ),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OMRON_ID])
    config = resolve_entity_block(config, _DEFAULT_ROWS)
    sub = config.get(CONF_BLUETOOTH)
    if sub is None:
        return
    entity = await switch.new_switch(sub)
    # Registered as a component so its setup() runs and the stored state is
    # restored; without this the switch has no boot-time hook at all.
    await cg.register_component(entity, sub)
    await cg.register_parented(entity, parent)
