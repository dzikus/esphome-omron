import datetime

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import esp32_ble, esp32_ble_client, esp32_ble_tracker
from esphome.components import time as time_
from esphome.components.esp32_ble import BTLoggers
from esphome.const import (
    CONF_BINDKEY,
    CONF_DAY,
    CONF_DEVICE_ID,
    CONF_DEVICES,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_MONTH,
    CONF_NAME,
    CONF_PLATFORM,
    CONF_TIME_ID,
    CONF_YEAR,
)
from esphome.core import CORE

DOMAIN = "omron"


AUTO_LOAD = [
    "binary_sensor",
    "button",
    "esp32_ble_client",
    "sensor",
    "switch",
    "text_sensor",
]
DEPENDENCIES = ["esp32_ble_tracker"]
CODEOWNERS = ["@dzikus"]
MULTI_CONF = True

CONF_AUTO_CONNECT = "auto_connect"
CONF_KEEP_BOND = "keep_bond"
CONF_REQUIRE_BOND = "require_bond"
CONF_AUTH_TIMEOUT = "auth_timeout"
CONF_BOND_CLEANUP_TIMEOUT = "bond_cleanup_timeout"
CONF_HISTORY_RECORDS = "history_records"
CONF_IGNORE_RECORDS_BEFORE = "ignore_records_before"
CONF_CLOCK_SYNC_THRESHOLD = "clock_sync_threshold"
CONF_END_SESSION = "end_session"
CONF_OMRON_ID = "omron_id"
CONF_PROFILE = "profile"
# `profile: auto` - let the cuff's own model string decide, instead of naming a
# profile here. Opt-in rather than the default because the mapping from what a
# cuff reports to a memory map rests on one measured device, and nobody should
# get automatic selection without having asked for it.
PROFILE_AUTO = "auto"
CONF_BIRTH_DATE = "birth_date"

CONF_REGISTER_AS_USER = "register_as_user"
CONF_WRITE_BIRTH_DATE = "write_birth_date"
CONF_EXCHANGE_IDENTITY_KEYS = "exchange_identity_keys"
CONF_ACCEPT_SECURITY_REQUEST = "accept_security_request"
CONF_FULL_READ_ON_PAIRING = "full_read_on_pairing"
CONF_USER = "user"
CONF_USERS = "users"
CONF_NAME_PREFIX = "name_prefix"

# Ceiling on per-user entity slots, mirroring OMRON_MAX_USERS in
# omron_profiles.h. How many users a given cuff really has is a property of its
# model and stays in the profile catalog: configuring a user the model does not
# store is reported by the component at startup.
OMRON_MAX_USERS = 2

# Entities decoded from a measurement record belong to the person who took it.
# Entities describing the radio link, the poll, or the model belong to the cuff.
SCOPE_USER = "user"
SCOPE_HUB = "hub"

omron_ns = cg.esphome_ns.namespace("omron")
OmronBLEClient = omron_ns.class_("OmronBLEClient", esp32_ble_client.BLEClientBase)
OmronProfileId = omron_ns.enum("OmronProfileId", is_class=True)

# Every profile that can be named here. `auto` is the other way in, and it is
# opt-in rather than the default: what a cuff reports about itself maps onto a
# memory map through one measured device, and two of these trade names cover two
# different maps.
OMRON_PROFILES = {
    "hem_6161t": OmronProfileId.HEM_6161T,
    "hem_6232t": OmronProfileId.HEM_6232T,
    "hem_7142t2": OmronProfileId.HEM_7142T2,
    "hem_7146t": OmronProfileId.HEM_7146T,
    "hem_7151t": OmronProfileId.HEM_7151T,
    "hem_7155t": OmronProfileId.HEM_7155T,
    "hem_7155t_mw": OmronProfileId.HEM_7155T_MW,
    "hem_7155t_k4": OmronProfileId.HEM_7155T_K4,
    "hem_7155t_mw3": OmronProfileId.HEM_7155T_MW3,
    "hem_7320t": OmronProfileId.HEM_7320T,
    "hem_7322t": OmronProfileId.HEM_7322T,
    "hem_7342t": OmronProfileId.HEM_7342T,
    "hem_7530t": OmronProfileId.HEM_7530T,
    "hem_7600t": OmronProfileId.HEM_7600T,
    # Second-hand, never exercised on one of these cuffs. The component logs the
    # confidence level at setup, so the choice is visible rather than implied by
    # the option merely existing.
    "hem_6231t": OmronProfileId.HEM_6231T,
    "hem_6320t": OmronProfileId.HEM_6320T,
    "hem_6321t": OmronProfileId.HEM_6321T,
    "hem_7136t": OmronProfileId.HEM_7136T,
    "hem_7150t": OmronProfileId.HEM_7150T,
    "hem_7188t1": OmronProfileId.HEM_7188T1,
    "hem_7361t": OmronProfileId.HEM_7361T,
    "hem_7380t1": OmronProfileId.HEM_7380T1,
    "hem_7382t1": OmronProfileId.HEM_7382T1,
    "hem_7386t1": OmronProfileId.HEM_7386T1,
    # Each of these is commonly filed under a neighbouring family whose memory
    # map it does not share, which reads every record from the wrong address.
    "hem_1026t2": OmronProfileId.HEM_1026T2,
    "hem_7188t1_le": OmronProfileId.HEM_7188T1_LE,
    "hem_7196t1": OmronProfileId.HEM_7196T1,
    "hem_7377t1": OmronProfileId.HEM_7377T1,
    "hem_7511t": OmronProfileId.HEM_7511T,
    "hem_9601t": OmronProfileId.HEM_9601T,
    # Two entries because HEM-6401T is usually treated as one family and is
    # two memory maps: the 6410T and 6411T variants keep their readings at a
    # different address, with twice the record size.
    "hem_6401t": OmronProfileId.HEM_6401T,
    "hem_6410t": OmronProfileId.HEM_6410T,
    # Single-user rings that also keep a large block of some other measurement
    # above the 16-bit address space.
    "hem_7191t1": OmronProfileId.HEM_7191T1,
    "hem_7440t1": OmronProfileId.HEM_7440T1,
    # Each is its parent family's map with that variant's own ring depth, which
    # is the ceiling on a read no yaml option caps.
    "hem_716bt2_deep": OmronProfileId.HEM_716BT2_DEEP,
    "hem_7157t_deep": OmronProfileId.HEM_7157T_DEEP,
    "hem_7600t_deep": OmronProfileId.HEM_7600T_DEEP,
    "hem_9700t": OmronProfileId.HEM_9700T,
}

CUSTOM_KEY_PROFILES = {
    "hem_6161t",
    "hem_6232t",
    "hem_7151t",
    "hem_7155t",
    "hem_7320t",
    "hem_7322t",
    "hem_7342t",
    "hem_7530t",
    "hem_7600t",
    "hem_6231t",
    "hem_6320t",
    "hem_6321t",
    "hem_7136t",
    "hem_7150t",
    "hem_7361t",
    # Classic transport like the HEM-7322T it was split from, so it needs the
    # same already-provisioned key.
    "hem_7511t",
    "hem_9601t",
    "hem_6401t",
    "hem_6410t",
    # Ring-depth splits inherit their parent's transport, so the classic ones
    # inherit the need for an already-provisioned key too.
    "hem_7157t_deep",
    "hem_7600t_deep",
    "hem_9700t",
}


# Profiles whose cuff stores one person, not two. Everything absent from here
# stores two; no model in the catalog stores more.
#
# A mirror of user_count in omron_profiles.cpp. Keep the two in step: a profile
# listed here but storing two people silently loses the second person's records.
SINGLE_USER_PROFILES = {
    "hem_6161t",
    "hem_6231t",
    "hem_6320t",
    "hem_6401t",
    "hem_6410t",
    "hem_7136t",
    "hem_7142t2",
    "hem_7146t",
    "hem_7150t",
    "hem_7151t",
    "hem_7188t1",
    "hem_7188t1_le",
    "hem_7191t1",
    "hem_7440t1",
    "hem_7530t",
    "hem_7600t",
    "hem_9601t",
    # Same four splits: a deeper ring does not change how many people the cuff
    # stores, so each inherits its parent's user count.
    "hem_716bt2_deep",
    "hem_7157t_deep",
    "hem_7600t_deep",
    "hem_9700t",
}


def profile_user_count(profile):
    """How many people the selected cuff stores. Two unless the catalog says one.

    `auto` answers with the ceiling, which is the permissive answer and the only
    honest one: the model is not known until the cuff reports it, so every check
    that keys off the user count has to move to the device. The firmware does run
    them - apply_profile_ repeats them once the profile is adopted - but they
    land in the log rather than in `esphome config`, and that is the price of
    detection.
    """
    if not isinstance(profile, str) or profile.lower() == PROFILE_AUTO:
        return OMRON_MAX_USERS
    return 1 if profile.lower() in SINGLE_USER_PROFILES else OMRON_MAX_USERS


def _profile_or_auto(value):
    """A catalog profile, or `auto` to take the cuff's word for it.

    Spelled out rather than cv.Any(one_of, enum) because cv.Any reports the first
    alternative's failure: a typo in a profile name came back as "valid options
    are 'auto'", which hides all thirty-eight of them behind the one option that
    is not a profile at all. Delegating to cv.enum keeps its own error, which
    lists them.
    """
    if isinstance(value, str) and value.lower() == PROFILE_AUTO:
        return PROFILE_AUTO
    return cv.enum(OMRON_PROFILES, lower=True)(value)


def _birth_date(value):
    """A date the cuff can actually hold, which is not the same range as a date
    Home Assistant would put on a timeline.

    The cuff keeps the year as one byte offset from 1900, so it takes 1900 to
    2155, and the frame builder already checks exactly that. cv.date_time floors
    at 1970 because it is built for timestamps, and on a blood pressure monitor
    that refuses the birth date of anyone over 56.
    """
    if isinstance(value, datetime.datetime):
        parsed = value.date()
    elif isinstance(value, datetime.date):
        parsed = value
    else:
        try:
            parsed = datetime.date.fromisoformat(str(value).strip())
        except ValueError as err:
            raise cv.Invalid(f"{value} is not a date of the form 1955-11-05") from err
    if not 1900 <= parsed.year <= 2155:
        raise cv.Invalid(
            f"year {parsed.year} is outside what the cuff stores, which is 1900 to 2155"
        )
    return {
        CONF_YEAR: parsed.year,
        CONF_MONTH: parsed.month,
        CONF_DAY: parsed.day,
    }


def _require_existing_custom_key(config):
    profile = config.get(CONF_PROFILE)
    if (
        isinstance(profile, str)
        and profile.lower() in CUSTOM_KEY_PROFILES
        and CONF_BINDKEY not in config
    ):
        raise cv.Invalid(
            f"profile {profile} requires bindkey for its already-provisioned custom key"
        )
    return config


def _users_must_exist_on_this_cuff(config):
    """Every user number in the hub block has to be one the model actually has.

    Validating against the ceiling of two instead accepts the config, boots the
    node and does nothing, because the refusal happens on the device and is
    silent.

    register_as_user is the one that hurts. On a single-user profile the write
    builder answers UNSUPPORTED_MODEL and registration never happens, which is
    the state that makes the cuff discard this node's bond and fail encryption
    on every later session.
    """
    profile = config.get(CONF_PROFILE)
    count = profile_user_count(profile)
    if count >= OMRON_MAX_USERS:
        return config

    def refuse(key, number):
        raise cv.Invalid(
            f"user {number} does not exist on profile {profile}, which stores {count} "
            f"user(s). Use user 1, or pick the profile that matches the cuff.",
            path=[key],
        )

    register = config.get(CONF_REGISTER_AS_USER)
    if isinstance(register, int) and register > count:
        refuse(CONF_REGISTER_AS_USER, register)
    for entry in config.get(CONF_USERS) or []:
        number = entry.get(CONF_USER)
        if isinstance(number, int) and number > count:
            refuse(CONF_USERS, number)
    return config


def _birth_dates_must_have_a_way_out(config):
    """A birth date nobody is going to write is a line of yaml doing nothing.

    The date is only sent for a user this session either registers as or was
    told to write standalone. Configured on its own it is stored in the
    component and never leaves, with no log line to say so.
    """
    register = config.get(CONF_REGISTER_AS_USER)
    stranded = sorted(
        entry[CONF_USER]
        for entry in config.get(CONF_USERS) or []
        if CONF_BIRTH_DATE in entry
        and not entry.get(CONF_WRITE_BIRTH_DATE)
        and entry[CONF_USER] != register
    )
    if stranded:
        listed = ", ".join(str(number) for number in stranded)
        raise cv.Invalid(
            f"birth_date is set for user(s) {listed}, but nothing will write it. "
            f"Add write_birth_date: true to the same entry, or make it "
            f"register_as_user.",
            path=[CONF_USERS],
        )
    return config


CONFIG_SCHEMA = cv.All(
    _require_existing_custom_key,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OmronBLEClient),
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Required(CONF_PROFILE): _profile_or_auto,
            cv.Optional(CONF_BINDKEY): cv.bind_key,
            # Only needed to say how far the cuff's clock has drifted. The clock
            # itself is read and published either way.
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            # Records older than the newest one, per person, fetched in the same
            # session and reported as Home Assistant events. Entities always
            # show the newest, so zero costs one read per person and loses
            # nothing.
            #
            # Unset reads the whole written ring: every record the cursor
            # accounts for, even when the cuff reports none outstanding. Any cap
            # here is this component's own invention, not something the protocol
            # asks for.
            #
            # The cost is bounded twice over: the poll plan trims the request to
            # the slots the cursor says were written, so a cuff holding nine
            # records reads nine whatever this says, and a ring that has not
            # moved since the last successful session is skipped whole. A full
            # ring is seconds rather than minutes, because a transfer block
            # carries several records at once.
            #
            # Set it to bound the first poll of a fresh install, or to 0 for
            # entities only - the newest reading per person and no events at
            # all, which is all the entities can show anyway.
            cv.Optional(CONF_HISTORY_RECORDS): cv.int_range(min=0, max=100),
            # Drops records stamped before this date entirely - no entity, no
            # event, no watermark. A cuff whose clock has never been set stamps
            # every reading with one default date, so those measurements are
            # real and their timestamps are not, and nine of them sharing a
            # second cannot even be put in order.
            #
            # Off by default and deliberately a date rather than a hardcoded
            # 2019: that year is what one cuff happens to show, not a value
            # Omron documents. Set it above the day you first set the cuff's
            # clock. Careful on a cuff where a user has nothing newer - that
            # person's entities go empty rather than stale.
            cv.Optional(CONF_IGNORE_RECORDS_BEFORE): cv.date_time(
                date=True, time=False
            ),
            # How far the cuff's clock may drift before this component writes a
            # fresh time into it. Zero, the default, means every session.
            #
            # Raising it withholds a write that goes out anyway carrying the
            # user marker, so it saves nothing and only lets the clock drift.
            # It exists for someone who wants the cuff's own timekeeping left
            # alone, and needs a time source to do anything at all.
            cv.Optional(CONF_CLOCK_SYNC_THRESHOLD): cv.positive_time_period_seconds,
            # The transfer ends with the 0x0F opcode, which is what stops the
            # cuff blinking Err. Leave it on; off is a diagnostic.
            cv.Optional(CONF_END_SESSION, default=True): cv.boolean,
            # Whether a bond may carry identity keys. Bluedroid asks for the
            # peer's IRK by default and ESPHome never narrows that, which
            # esphome#17104 blames for unusable bonds. True keeps stock
            # behaviour and no profile here needs anything else.
            cv.Optional(CONF_EXCHANGE_IDENTITY_KEYS, default=True): cv.boolean,
            # How to answer a peer that asks for security. ESPHome always says
            # yes; saying no is what a host that does not bond at all would do.
            cv.Optional(CONF_ACCEPT_SECURITY_REQUEST, default=True): cv.boolean,
            # Registers this node with the cuff as that user, which is what
            # stops the cuff discarding its half of the bond. Without it every
            # reconnect fails with auth fail 97. Takes priority over every other
            # write in a session.
            cv.Optional(CONF_REGISTER_AS_USER): cv.int_range(
                min=1, max=OMRON_MAX_USERS
            ),
            # A session opened by a cuff blinking -P- re-reads every ring, even
            # one whose cursor has not moved. Off because the cursor check is
            # what keeps an ordinary press down to two frames; on, a press with
            # nothing new behind it costs a full transfer.
            cv.Optional(CONF_FULL_READ_ON_PAIRING, default=False): cv.boolean,
            # The sub-device the cuff's own entities file under.
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            # Everything that varies per person, in one place. A platform block
            # then only says which user it is for.
            cv.Optional(CONF_USERS): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required(CONF_USER): cv.int_range(
                            min=1, max=OMRON_MAX_USERS
                        ),
                        cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
                        cv.Optional(CONF_NAME_PREFIX): cv.All(
                            cv.string_strict, cv.Length(max=48)
                        ),
                        # Repeats until the stored date reads back as this one:
                        # the cuff can take a write and still refuse it at
                        # session end.
                        cv.Optional(CONF_BIRTH_DATE): _birth_date,
                        # Lets the date go out without registering as this
                        # person. Off by default: a user block written outside a
                        # registering session is a frame shape no other host
                        # sends. It exists because a cuff holds two people and a
                        # node registers as one, leaving the other's date no way
                        # in short of a second pairing.
                        cv.Optional(CONF_WRITE_BIRTH_DATE, default=False): cv.boolean,
                    }
                )
            ),
            cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
            cv.Optional(CONF_KEEP_BOND): cv.boolean,
            cv.Optional(CONF_REQUIRE_BOND): cv.boolean,
            cv.Optional(
                CONF_AUTH_TIMEOUT, default="20s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_BOND_CLEANUP_TIMEOUT, default="10s"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    # After the schema, because both need the parsed values rather than the raw
    # yaml, and both answer at `esphome config` time what the device used to
    # answer with silence.
    _users_must_exist_on_this_cuff,
    _birth_dates_must_have_a_way_out,
    esp32_ble.consume_connection_slots(1, "omron"),
)


# Shared by the entity platforms. Omron owns its connection instead of attaching
# a BLEClientNode to the stock ble_client component.
def _same_id(left, right):
    """Ids compared by their name, whatever object form they arrive in."""
    return str(getattr(left, "id", left)) == str(getattr(right, "id", right))


def _entity_users_must_exist(config):
    """An entity block cannot belong to a person this cuff does not store.

    Runs at final validation because the answer lives in two places: the profile
    is on the hub and the user number is on the platform block, and neither
    schema can see the other.

    Left to the device, this is an ESP_LOGE with no mark_failed() - the node
    boots, reads every record correctly, and has nowhere to put them. The only
    trace is one line in the boot log of a node that otherwise looks healthy.
    """
    count = profile_user_count(config.get(CONF_PROFILE))
    if count >= OMRON_MAX_USERS:
        return config
    full = fv.full_config.get()
    hub_id = config.get(CONF_ID)
    for domain in _ENTITY_DOMAINS:
        for block in full.get(domain) or []:
            if not isinstance(block, dict) or block.get(CONF_PLATFORM) != DOMAIN:
                continue
            if not _same_id(block.get(CONF_OMRON_ID), hub_id):
                continue
            number = block.get(CONF_USER)
            if isinstance(number, int) and number > count:
                raise cv.Invalid(
                    f"{domain} block is configured for user {number}, but profile "
                    f"{config.get(CONF_PROFILE)} stores {count} user(s). Those "
                    f"entities would never publish anything."
                )
    return config


FINAL_VALIDATE_SCHEMA = _entity_users_must_exist

OMRON_COMPONENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OMRON_ID): cv.use_id(OmronBLEClient),
    }
)

# One platform block addresses one person, and says only which one. The
# sub-device and the name prefix come from that person's entry under `users:`
# on the cuff, so adding a platform domain does not mean repeating them.
OMRON_ENTITY_PLATFORM_SCHEMA = OMRON_COMPONENT_SCHEMA.extend(
    {
        cv.Optional(CONF_USER, default=1): cv.int_range(min=1, max=OMRON_MAX_USERS),
    }
)


# Domains an omron entity platform can appear under, for counting sibling
# blocks on the same cuff.
_ENTITY_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "switch", "button")


def _yaml_root():
    # raw_config, because the sub-device has to be on each entity before its own
    # schema validates: the duplicate-name check keys on (device_id, platform,
    # name), and without one every block's "Systolic blood pressure" collides.
    # CORE.config is still None that early and only fills in for to_code.
    return CORE.raw_config or CORE.config or {}


def hub_config(config):
    """The `omron:` block this one belongs to, or empty when its id was
    generated rather than written and the raw yaml has nothing to match on."""
    declared = _yaml_root().get(DOMAIN) or []
    if isinstance(declared, dict):
        declared = [declared]
    declared = [hub for hub in declared if isinstance(hub, dict)]
    hub_id = config.get(CONF_OMRON_ID)
    for hub in declared:
        if _same_id(hub.get(CONF_ID), hub_id):
            return hub
    return declared[0] if len(declared) == 1 else {}


def hub_user_entry(config, user_number):
    """That person's entry under the cuff's `users:`, or empty if unnamed."""
    for entry in hub_config(config).get(CONF_USERS, []) or []:
        if not isinstance(entry, dict):
            continue
        try:
            if int(entry.get(CONF_USER)) == int(user_number):
                return entry
        except (TypeError, ValueError):
            continue
    return {}


def block_device_id(config):
    if CONF_USER in config:
        return hub_user_entry(config, config.get(CONF_USER)).get(CONF_DEVICE_ID)
    return hub_config(config).get(CONF_DEVICE_ID)


def sub_device_name(device_id):
    """Friendly name behind a sub-device id, or empty if it does not resolve."""
    if device_id is None:
        return ""
    for device in (_yaml_root().get("esphome") or {}).get(CONF_DEVICES, []) or []:
        if _same_id(device.get(CONF_ID), device_id):
            return str(device.get(CONF_NAME, "")).strip()
    return ""


def block_name_prefixes(config):
    """Prefixes that keep this block's default entity names to itself.

    Returns (user_prefix, hub_prefix). An entity's api key is a hash of its
    name with no device in it, and mqtt builds its topics and unique_id from
    the name alone. Two blocks taking the same default name therefore collapse
    into one entity on mqtt and on any client that addresses by key; only the
    native api plus Home Assistant 2025.8+ separates them, via (device_id,
    key). Two people on one cuff hit this without a second cuff anywhere in
    the config.

    The scope of the entity picks the axis, not the presence of `user:` in the
    block: `user` carries a schema default, so by to_code every block has one.
    """
    user_number = config.get(CONF_USER, 1)
    hub_id = str(config.get(CONF_OMRON_ID))
    users, hubs = set(), set()
    for domain in _ENTITY_DOMAINS:
        for block in CORE.config.get(domain, []) or []:
            if not isinstance(block, dict) or block.get(CONF_PLATFORM) != DOMAIN:
                continue
            block_hub = str(block.get(CONF_OMRON_ID))
            hubs.add(block_hub)
            if block_hub == hub_id:
                users.add(block.get(CONF_USER, 1))
    user_prefix = f"User {user_number}" if len(users) > 1 else ""
    hub_prefix = ""
    if len(hubs) > 1:
        text = hub_id[len(DOMAIN) + 1 :] if hub_id.startswith(f"{DOMAIN}_") else hub_id
        text = text.replace("_", " ").replace("-", " ").strip()
        hub_prefix = text[0].upper() + text[1:] if text else ""

    # A name written under `users:` always applies. A sub-device name stands in
    # for one that is not, but only where a prefix was going out anyway: with
    # nothing to disambiguate, the device name is what Home Assistant already
    # puts in front of the entity, and adding it here as well would rename
    # entities that never collided.
    entry = hub_user_entry(config, user_number)
    explicit = entry.get(CONF_NAME_PREFIX)
    if explicit is not None:
        user_prefix = str(explicit).strip()
    elif user_prefix:
        user_prefix = sub_device_name(entry.get(CONF_DEVICE_ID)) or user_prefix
    if hub_prefix:
        hub_prefix = (
            sub_device_name(hub_config(config).get(CONF_DEVICE_ID)) or hub_prefix
        )
    return user_prefix, hub_prefix


def resolve_entity_block(config, rows):
    """Prefixes this block's default entity names, at to_code because counting
    the sibling blocks needs the whole validated config."""
    user_prefix, hub_prefix = block_name_prefixes(config)
    if not user_prefix and not hub_prefix:
        return config
    config = dict(config)
    for key, default_name, scope in rows:
        prefix = user_prefix if scope == SCOPE_USER else hub_prefix
        sub = config.get(key)
        # Only the injected default is prefixed: a name the user wrote is theirs.
        if prefix and isinstance(sub, dict) and sub.get(CONF_NAME) == default_name:
            config[key] = {**sub, CONF_NAME: f"{prefix} {default_name}"}
    return config


def omron_user_index(config):
    """Zero-based user slot for an entity platform block.

    Runs from _inject_defaults too, which is before schema validation, so the
    value can still be anything the yaml contained. An unusable one falls back
    to the default slot and lets the schema report the real error.
    """
    try:
        return int(config.get(CONF_USER, 1)) - 1
    except (TypeError, ValueError):
        return 0


def inject_entity_defaults(config, rows, hidden=frozenset()):
    # Copy before mutation: validators may receive a shared mapping.
    config = dict(config)
    platform_device_id = block_device_id(config)
    # Presence of `user:`, not its value, decides what a block is for. A block
    # that names a person carries that person's measurements; a block that names
    # nobody carries the cuff's own radio and poll entities. Every entity then
    # has exactly one home and one sub-device, and neither kind can duplicate.
    # Declaring a key explicitly still overrides this in either direction.
    user_declared = CONF_USER in config
    for key, default_name, scope in rows:
        want = config.get(key, ...)
        if want is False:
            config.pop(key, None)
            continue
        if want is ... or want is None:
            if scope == SCOPE_HUB and user_declared:
                continue
            if scope == SCOPE_USER and not user_declared:
                continue
            sub = {}
        # is, not ==: a stray 'pulse: 1' equals True and must not read as one.
        elif want is True:
            sub = {}
        else:
            sub = want
        if not isinstance(sub, dict):
            raise cv.Invalid(
                f"'{key}' takes true, false, or the options for one entity. "
                f"To rename it write 'name: {sub}' under it.",
                path=[key],
            )
        sub = dict(sub)
        sub.setdefault(CONF_NAME, default_name)
        if platform_device_id is not None:
            sub.setdefault(CONF_DEVICE_ID, platform_device_id)
        if key in hidden:
            sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


async def to_code(config):
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.SMP)
    cg.add_define("USE_ESP32_BLE_UUID")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_client(var, config)
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    if config[CONF_PROFILE] == PROFILE_AUTO:
        cg.add(var.set_profile_auto())
    else:
        cg.add(var.set_profile(config[CONF_PROFILE]))
    if CONF_BINDKEY in config:
        cg.add(var.set_bind_key(config[CONF_BINDKEY]))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    # Left alone when unset: the component already defaults to the whole ring.
    if CONF_HISTORY_RECORDS in config:
        cg.add(var.set_history_records(config[CONF_HISTORY_RECORDS]))
    if CONF_CLOCK_SYNC_THRESHOLD in config:
        cg.add(
            var.set_clock_sync_threshold(
                int(config[CONF_CLOCK_SYNC_THRESHOLD].total_seconds)
            )
        )
    if CONF_IGNORE_RECORDS_BEFORE in config:
        # cv.date_time returns the parsed fields, not the string it matched, so
        # nothing here has to parse a date and no parser ships to the device.
        cut_off = config[CONF_IGNORE_RECORDS_BEFORE]
        cg.add(
            var.set_ignore_records_before(
                cut_off[CONF_YEAR], cut_off[CONF_MONTH], cut_off[CONF_DAY]
            )
        )
    cg.add(var.set_end_session(config[CONF_END_SESSION]))
    cg.add(var.set_exchange_identity_keys(config[CONF_EXCHANGE_IDENTITY_KEYS]))
    cg.add(var.set_accept_security_request(config[CONF_ACCEPT_SECURITY_REQUEST]))
    cg.add(var.set_full_read_on_pairing(config[CONF_FULL_READ_ON_PAIRING]))
    if CONF_REGISTER_AS_USER in config:
        cg.add(var.set_register_as_user(config[CONF_REGISTER_AS_USER]))
    for entry in config.get(CONF_USERS, []):
        user = entry[CONF_USER]
        if entry.get(CONF_WRITE_BIRTH_DATE):
            cg.add(var.allow_birth_date_write(user))
        date = entry.get(CONF_BIRTH_DATE)
        if date is not None:
            cg.add(
                var.set_birth_date(
                    user, date[CONF_YEAR], date[CONF_MONTH], date[CONF_DAY]
                )
            )
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    if CONF_KEEP_BOND in config:
        cg.add(var.set_keep_bond(config[CONF_KEEP_BOND]))
    if CONF_REQUIRE_BOND in config:
        cg.add(var.set_require_bond(config[CONF_REQUIRE_BOND]))
    cg.add(var.set_auth_timeout(config[CONF_AUTH_TIMEOUT].total_milliseconds))
    cg.add(
        var.set_bond_cleanup_timeout(
            config[CONF_BOND_CLEANUP_TIMEOUT].total_milliseconds
        )
    )
