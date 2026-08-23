import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE

from . import (
    CONF_OMRON_ID,
    CONF_PROFILE,
    CUSTOM_KEY_PROFILES,
    OMRON_ENTITY_PLATFORM_SCHEMA,
    PROFILE_AUTO,
    SCOPE_HUB,
    inject_entity_defaults,
    omron_ns,
    resolve_entity_block,
)

# Three buttons, and none of them sends anything to the cuff on the press
# itself. Poll now schedules the same session as the normal cadence, no more and
# no less. Pair programs the key into the cuff, which is a lasting change and is
# why it needs a bindkey and exists only for the profiles that authenticate with
# one. Forget bond drops our own bond record so the next session pairs, and
# a session that pairs is the only kind the cuff accepts a user block from.
# All three act on the cuff rather than on a person, so they are created once per
# cuff even when two people have their own entity sets.

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["omron"]
CODEOWNERS = ["@dzikus"]

CONF_POLL_NOW = "poll_now"
CONF_PAIR = "pair"
CONF_FORGET_BOND = "forget_bond"

OmronPollNowButton = omron_ns.class_("OmronPollNowButton", button.Button, cg.Parented)
OmronPairButton = omron_ns.class_("OmronPairButton", button.Button, cg.Parented)
OmronForgetBondButton = omron_ns.class_(
    "OmronForgetBondButton", button.Button, cg.Parented
)


def _hub_programs_a_key(hub_id):
    # Key programming is the classic transport's flow: the host invents a 16-byte
    # key and writes it into the cuff. A token profile has no key to program and
    # answers the request with an error, so on those the button is a dead entity
    # in Home Assistant.
    #
    # `auto` keeps it, which is the permissive answer and the only honest one:
    # the profile is not known until the cuff has reported its model, and `auto`
    # is what somebody runs to find that out. Dropping the button here would
    # remove it from exactly the configuration that cannot yet say whether it is
    # needed, on a cuff that may well turn out to need it. The firmware refuses
    # the press with a message naming the detected profile.
    target = str(hub_id)
    for hub_conf in CORE.config.get("omron", []):
        if str(hub_conf.get(CONF_ID)) == target:
            profile = str(hub_conf.get(CONF_PROFILE, "")).lower()
            return profile == PROFILE_AUTO or profile in CUSTOM_KEY_PROFILES
    return False


_DEFAULT_ROWS = [
    (CONF_POLL_NOW, "Poll now", SCOPE_HUB),
    (CONF_PAIR, "Pair", SCOPE_HUB),
    (CONF_FORGET_BOND, "Forget bond", SCOPE_HUB),
]


def _inject_defaults(config):
    # Poll now is a diagnostic and stays out of the way. Pair does not: without
    # it the cuff has to be put into pairing mode for every single read, so it
    # is the first thing someone setting this up needs to find.
    return inject_entity_defaults(
        config, _DEFAULT_ROWS, hidden=frozenset({CONF_POLL_NOW})
    )


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OMRON_ENTITY_PLATFORM_SCHEMA.extend(
        {
            cv.Optional(CONF_POLL_NOW): button.button_schema(
                OmronPollNowButton,
                icon="mdi:refresh",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PAIR): button.button_schema(
                OmronPairButton,
                icon="mdi:key-plus",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_FORGET_BOND): button.button_schema(
                OmronForgetBondButton,
                icon="mdi:link-off",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
        }
    ),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OMRON_ID])
    config = resolve_entity_block(config, _DEFAULT_ROWS)
    for key in (CONF_POLL_NOW, CONF_PAIR, CONF_FORGET_BOND):
        sub = config.get(key)
        if sub is None:
            continue
        if key == CONF_PAIR and not _hub_programs_a_key(config[CONF_OMRON_ID]):
            # Every token-profile build sees this, because the key is injected
            # by default rather than written. Logged rather than dropped in
            # silence: the button is documented and the config did not refuse
            # it, so its absence needs saying.
            _LOGGER.info(
                "omron: skipping the pair button, profile authenticates with a "
                "session token and has no key to program"
            )
            continue
        entity = await button.new_button(sub)
        await cg.register_parented(entity, parent)
