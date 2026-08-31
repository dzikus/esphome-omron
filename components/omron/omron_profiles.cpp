#include "omron_profiles.h"

#include <iterator>

namespace esphome::omron {

const OmronGattCapabilities OMRON_CLASSIC_GATT = {
    .parent_service_uuid = "ecbe3980-c9a2-11e1-b1bd-0002a5d5c51b",
    .rx_channel_uuids = {{
        "49123040-aee8-11e1-a74d-0002a5d5c51b",
        "4d0bf320-aee8-11e1-a0d9-0002a5d5c51b",
        "5128ce60-aee8-11e1-b84b-0002a5d5c51b",
        "560f1420-aee8-11e1-8184-0002a5d5c51b",
    }},
    .rx_channel_count = 4,
    .tx_channel_uuids = {{
        "db5b55e0-aee7-11e1-965e-0002a5d5c51b",
        "e0b8a060-aee7-11e1-92f4-0002a5d5c51b",
        "0ae12b00-aee8-11e1-a192-0002a5d5c51b",
        "10e1ba60-aee8-11e1-89e5-0002a5d5c51b",
    }},
    .tx_channel_count = 4,
    .unlock_characteristic_uuid = "b305b680-aee7-11e1-a730-0002a5d5c51b",
};

const OmronGattCapabilities OMRON_MODERN_GATT = {
    .parent_service_uuid = "0000fe4a-0000-1000-8000-00805f9b34fb",
    .rx_channel_uuids = {{
        "49123040-aee8-11e1-a74d-0002a5d5c51b",
        nullptr,
        nullptr,
        nullptr,
    }},
    .rx_channel_count = 1,
    .tx_channel_uuids = {{
        "db5b55e0-aee7-11e1-965e-0002a5d5c51b",
        nullptr,
        nullptr,
        nullptr,
    }},
    .tx_channel_count = 1,
    .unlock_characteristic_uuid = "b305b680-aee7-11e1-a730-0002a5d5c51b",
};

template <size_t N>
static constexpr size_t alias_count(const char *const (&)[N]) {
  return N;
}

static const char *const HEM_9601T_ALIASES[] = {
    "HEM-9601T-J3",
    "HEM-9601T_E3",
};

static const char *const HEM_9700T_ALIASES[] = {
    "HEM-9700T",
};

static const char *const HEM_6401T_ALIASES[] = {
    "HEM-6401",
    "HEM-6401T-Z",
    "HEM-6402T",
    "HEM-6402T-Z",
};

static const char *const HEM_6410T_ALIASES[] = {
    "HEM-6410T-Z", "HEM-6410T-Z_BP", "HEM-6410T-Z_BP+EV", "HEM-6411T-MAE", "HEM-6411T-MAJ",
};

static const char *const HEM_7191T1_ALIASES[] = {
    "HEM-7191T1-LCA",
    "HEM-7191T1-LZ",
};

static const char *const HEM_7440T1_ALIASES[] = {
    "HEM-7440T1-FLE",
    "HEM-7440T1-FLZ",
};

static const char *const HEM_6231T_ALIASES[] = {
    "HEM-6231T-SH",
    "HEM-6231T_Z",
};

static const char *const HEM_6320T_ALIASES[] = {
    "HEM-6320T-Z",
    "HEM-6323T",
    "HEM-6325T",
};

static const char *const HEM_6321T_ALIASES[] = {
    "HEM-6321T-Z",
    "HEM-6324T",
};

static const char *const HEM_7136T_ALIASES[] = {
    "HEM-7136T-SH3",
    "HEM-7138JT-SH",
    "HEM-7138T-SH",
    "HEM-7139T-SH3",
};

static const char *const HEM_7150T_ALIASES[] = {
    "HEM-7150T-CA", "HEM-7150T-Z",   "HEM-7153JT_ASH", "HEM-7153T_ASH", "HEM-7156T-BR",
    "HEM-7156T-LA", "HEM-7156T_AAP", "HEM-7156T",      "HEM-7156T_AP",
};

static const char *const HEM_7157T_DEEP_ALIASES[] = {
    "HEM-7157T-AP",
    "HEM-7158T-JC",
    "HEM-7158T_AP3",
};

static const char *const HEM_7188T1_ALIASES[] = {
    "HEM-7188T1",
};

static const char *const HEM_7361T_ALIASES[] = {
    "HEM-7361T",
};

static const char *const HEM_7380T1_ALIASES[] = {
    "HEM-7183T1-AP",
    "HEM-7183T1-CAP",
    "HEM-7183T1_FLBIN",
    "HEM-7183T1_FLIN",
    "HEM-7183T1_LAP",
    "HEM-7380T1-EBK",
    "HEM-7380T1-EOSL",
    // Bare family name, without a regional suffix. Devices do report these.
    "HEM-7194T1",
    "HEM-7194T1-FLAP",
    "HEM-7194T1-FLCAP",
    "HEM-7194T1_FLBIN",
    "HEM-7194T1_FLIN",
    "HEM-7380T",
    "HEM-7383T1-AP",
    "HEM-7384T1-NBBR",
};

static const char *const HEM_7382T1_ALIASES[] = {
    "HEM-7385T1-AJAZ3", "HEM-7387T1-AJAZ3", "HEM-7389T1-JM3", "HEM-7376T1-ACACD6", "HEM-7376T1-Z",
};

static const char *const HEM_7386T1_ALIASES[] = {
    "HEM-7386T1-AJF3",
    "HEM-7388T1-AJF3",
    "HEM-7381T1-AZ",
    "HEM-7382T1-AZAZ",
};

static const char *const HEM_6161T_ALIASES[] = {
    "HEM-6161T-D", "HEM-6161T-D/E", "HEM-6161T-E", "HEM-6161T-RU", "HEM-6161T2-BR", "HEM-7271L-SH3",
};

static const char *const HEM_6232T_ALIASES[] = {
    "HEM-6232T-AP", "HEM-6232T-D", "HEM-6232T-D/E", "HEM-6232T-E",
    "HEM-6232T-Z",  "HEM-6233T",   "HEM-6320T-SH",  "HEM-6322T-SH",
};

static const char *const HEM_7142T2_ALIASES[] = {
    "HEM-7138K-SH",
    "HEM-7140T1",
    "HEM-7140T1-AP",
    "HEM-7141T1-AP",
    "HEM-7142T1",
    "HEM-7142T1-AP",
    "HEM-7142T2-AP",
    "HEM-7142T2-Z",
    "HEM-7142T2-ZAZ",
    "HEM-7142T2_JAZ",
    // Ring depth unknown for this one, so it stays on the family's shallower
    // figure rather than following the -ZAZ that shares its stem. Reading less
    // than a device holds loses history; reading more walks slots nobody wrote.
    "HEM-716BT2",
};

// The same map as HEM-7142T2 with twice the ring. A separate profile because
// reading the whole ring by default makes the record count the real ceiling
// rather than a number nothing reaches.
static const char *const HEM_716BT2_DEEP_ALIASES[] = {
    "HEM-716BT2-ZAZ",
    "HEM-716CT2-Z",
};

static const char *const HEM_7146T_ALIASES[] = {
    "HEM-7143T1-AIN",
    "HEM-7143T1-AP",
    "HEM-7143T1-D",
    "HEM-7143T1-E",
    "HEM-7143T1_D",
    "HEM-7143T1_EBK",
    "HEM-7143T2-E",
    "HEM-7143T2_ESL",
    "HEM-7144T1-AU",
    "HEM-7144T2-BR",
    "HEM-7144T2-LA",
    "HEM-7146T2",
    "HEM-7146T2-EBK",
    "HEM-7146T2-ESL",
    "HEM-7146T2-JD",
    "HEM-7146T2-JF",
    // Shares this map but reports no cuff fit, so that entity stays unavailable.
    "HEM-7149T2-E",
    "HEM-716DT2-LA",
};

static const char *const HEM_7151T_ALIASES[] = {
    "HEM-7151T-Z",
};

static const char *const HEM_7155T_ALIASES[] = {
    "HEM-7155T-ALRU",   "HEM-7155T-D",   "HEM-7155T-EBK", "HEM-7155T-EBL", "HEM-7155T_AP", "HEM-7155T_ASH3BK",
    "HEM-7155T_ASH3SL", "HEM-7155T_ESL", "HEM-7340T-CA",  "HEM-7340T-Z",   "HEM-7341T-Z",
};

static const char *const HEM_7155T_K4_ALIASES[] = {
    "HEM-7155T_K4-D", "HEM-7155T_K4-EBK", "HEM-7155T_K4-ESL", "HEM-7340T_K4-CA", "HEM-7340T_K4-Z", "HEM-7341T_K4-Z",
};

static const char *const HEM_7155T_MW3_ALIASES[] = {
    "HEM-7155T_ESL1",
};

static const char *const HEM_7320T_ALIASES[] = {
    "HEM-7320T-CA", "HEM-7320T-CACS", "HEM-7320T-ZV", "HEM-7320T_TI-CA", "HEM-7320T_TI-Z", "HEM-8725T-WM",
};

static const char *const HEM_7322T_ALIASES[] = {
    "HEM-7321T-CA",
    "HEM-7321T_TI-CA",
    "HEM-7321T_TI-Z",
    "HEM-7280T-AP",
    "HEM-7280T-E",
    "HEM-7280C",
    "HEM-7280T",
    // Shares this map but carries no record sequence number, and that offset is
    // per profile rather than per alias. "Measurement sequence" therefore
    // publishes whatever sits at offset 10 for this variant alone.
    "HEM-7280T-D",
    "HEM-7280T_TI-D",
    "HEM-7280T_TI-E",
    "HEM-7281T",
    "HEM-7282T",
    "HEM-7321T-ZV",
    "HEM-7322T-D",
    "HEM-7322T-E",
    "HEM-8732K-SH",
    "HEM-8732T-SH",
};

static const char *const HEM_7342T_ALIASES[] = {
    "HEM-7159T_AP3",  "HEM-7342T-CA",     "HEM-7342T-Z",      "HEM-7342T1-ACACD6", "HEM-7342T1-ACDC6", "HEM-7343T",
    "HEM-7343T-Z",    "HEM-7361T-E",      "HEM-7344JT_ASH3",  "HEM-7344T_ASH3BK",  "HEM-7344T_ASH3SL", "HEM-7346T-AJC3",
    "HEM-7346T-AJE3", "HEM-7346T2-AJC32", "HEM-7346T2-AJE32", "HEM-7346T_ABR3",    "HEM-7346T_AP3",    "HEM-7347T-AJC3",
    "HEM-7347T-AJE3", "HEM-7347T2-AJC32", "HEM-7347T2-AJE32", "HEM-7349T_ABR",     "HEM-7361T-ALRU",   "HEM-7361T-AP",
    "HEM-7361T-D",    "HEM-7361T-EBK",    "HEM-7361T1-BS",    "HEM-7361T_ESL",
};

static const char *const HEM_7530T_ALIASES[] = {
    "HEM-6231T2-JC",  "HEM-6231T2-JE", "HEM-6231T2-JT3", "HEM-7271P-SH3", "HEM-7271T_SH3", "HEM-7530T-Z",
    "HEM-7530T1-BR3", "HEM-7530T_AP3", "HEM-7530T_E3",   "HEM-7530T_J3",  "HEM-7530T_JT3", "HEM-8630T-SH",
};

// The variants of the 7600T family with a hundred slots where the family
// carries ninety.
//
// The bare "HEM-7600T" belongs here but stays with the parent, because it is
// also the parent's model name and a name cannot resolve to two profiles. It
// therefore reads ninety of its hundred slots, the one known shortfall here.
static const char *const HEM_7600T_DEEP_ALIASES[] = {
    "HEM-7271T", "HEM-7600T-E", "HEM-7600T-Z", "HEM-7600T-ZCD6BK", "HEM-7600T2-JF",
};

static const char *const HEM_7600T_ALIASES[] = {
    "HEM-7270C",
    "HEM-7324C",
    "HEM-7325T",
    // A hundred-slot variant, kept here at ninety because this string is also
    // this profile's model name. See HEM_7600T_DEEP_ALIASES.
    "HEM-7600T",
    "HEM-7600T-SH3BK",
    "HEM-7600T_W",
    "HEM-7600T_W-SH3W",
    "HEM-7600T_W-Z",
    "HEM-9601T2-BR3",
};

// Five families that are commonly filed under a neighbour whose memory map they
// do not share. Everything below the map - GATT, security, unlock, byte order,
// record format, clock offset - is inherited from that neighbour, and that
// inheritance is an assumption. Hence REFERENCE_ONLY for all five.

static const char *const HEM_1026T2_ALIASES[] = {
    "HEM-1026T2-AJC",
    "HEM-1026T2-AJE",
    "HEM-1026T2-AKA",
};

static const char *const HEM_7188T1_LE_ALIASES[] = {
    "HEM-7188T1-LE",
    "HEM-7188T1-LEO",
};

static const char *const HEM_7196T1_ALIASES[] = {
    "HEM-7196T1-FLE",
    "HEM-7196T1-FLEO",
};

static const char *const HEM_7377T1_ALIASES[] = {
    "HEM-7377T1-ZAZ",
};

static const char *const HEM_7511T_ALIASES[] = {
    "HEM-7511T",
    "HEM-7510C",
};

static constexpr OmronUserMemoryLayout UNUSED_USER = {0, 0, 0, 0, 0, 0};

static constexpr OmronProfile PROFILE_CATALOG[] = {
    {
        .id = OmronProfileId::UNSUPPORTED,
        .model = "UNSUPPORTED",
        .gatt = nullptr,
        .security_mode = SecurityMode::NONE,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::NONE,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::UNSUPPORTED,
        .record_size = 0,
        .transmission_block_size = 0,
        .settings_read_address = 0,
        .settings_write_address = 0x0000,
        .settings_index_region_size = 0,
        .user_block_size = 0,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0,
        .time_region_end = 0,
        .users = {{UNUSED_USER, UNUSED_USER}},
        .user_count = 0,
        .equivalent_model_ids = nullptr,
        .equivalent_model_id_count = 0,
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        // The unsupported sentinel decodes nothing, so it describes no fields.
        .measurement_fields = 0,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_6161T,
        .model = "HEM-6161T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 30, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_6161T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6161T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        // Neither variant reports artifact detection, so that entity stays
        // unavailable rather than reading a permanent zero.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~MEASUREMENT_FIELD_ARTIFACT,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    {
        .id = OmronProfileId::HEM_6232T,
        .model = "HEM-6232T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 100, 0x00, 0x04, 0x00FF, -1}, {0x0860, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_6232T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6232T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    {
        .id = OmronProfileId::HEM_7142T2,
        .model = "HEM-7142T2",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 14, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7142T2_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7142T2_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    // HEM-7142T2 field for field, ring twice as deep. Only the record count and
    // the alias list differ; if anything else about that family ever changes,
    // it has to change here too.
    {
        .id = OmronProfileId::HEM_716BT2_DEEP,
        .model = "HEM-716BT2-DEEP",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 30, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_716BT2_DEEP_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_716BT2_DEEP_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        // No IHB, unlike the family this was cut from. Splitting a profile
        // splits the evidence with it: the parent may claim a field because one
        // of its other variants describes it, and neither of these two does.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~MEASUREMENT_FIELD_IHB,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7146T,
        .model = "HEM-7146T",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 30, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7146T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7146T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7151T,
        .model = "HEM-7151T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 80, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7151T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7151T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        // HEM-7151T-Z reports no IHB. The neighbouring 7150T family splits on
        // the same field, which is why that one keeps the union and this does
        // not.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~MEASUREMENT_FIELD_IHB,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    {
        .id = OmronProfileId::HEM_7155T,
        .model = "HEM-7155T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 60, 0x00, 0x04, 0x00FF, -1}, {0x0458, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7155T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7155T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    // Reachable only by an explicit yaml pin: it declares no equivalent model
    // ids, and no device reports the literal string below. An MW-firmware 7155T
    // announcing something like HEM-7155T-EBK matches the classic profile
    // instead, which shares the EEPROM addresses but declares the classic key
    // stack and a 0x10 transfer block rather than 0x38. No alias reaches it:
    // inventing one without a capture would be a guess.
    {
        .id = OmronProfileId::HEM_7155T_MW,
        .model = "HEM-7155T-MW",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 60, 0x00, 0x04, 0x00FF, -1}, {0x0458, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = nullptr,
        .equivalent_model_id_count = 0,
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7155T_K4,
        .model = "HEM-7155T-K4",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 60, 0x00, 0x04, 0x00FF, -1}, {0x06A8, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7155T_K4_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7155T_K4_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7155T_MW3,
        .model = "HEM-7155T-MW3",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        // User 2's cursor is at 0x02, measured: after a user-2 measurement the
        // byte at 0x02 stepped 0x0B -> 0x0C while 0x08 did not move. Reading
        // 0x08 keeps selecting a stale slot and new records are never fetched,
        // which is a slow failure rather than a loud one.
        .users = {{{0x02E8, 60, 0x00, 0x04, 0x00FF, -1}, {0x06A8, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7155T_MW3_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7155T_MW3_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        // Records decoded to numbers matching the cuff's own display, both
        // record bases and the index cursors read, the clock written and read
        // back, and a chosen birth date returned byte for byte.
        .confidence = OmronProfileConfidence::HARDWARE_VERIFIED,
    },
    {
        .id = OmronProfileId::HEM_7320T,
        .model = "HEM-7320T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x0286,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        .users = {{{0x02AC, 60, 0x00, 0x04, 0x00FF, -1}, {0x05F4, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7320T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7320T_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    {
        .id = OmronProfileId::HEM_7322T,
        .model = "HEM-7322T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x0286,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        .users = {{{0x02AC, 100, 0x00, 0x04, 0x00FF, -1}, {0x0824, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7322T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7322T_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    {
        .id = OmronProfileId::HEM_7342T,
        .model = "HEM-7342T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 100, 0x00, 0x04, 0x00FF, -1}, {0x06D8, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7342T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7342T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7530T,
        .model = "HEM-7530T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 90, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7530T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7530T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    {
        .id = OmronProfileId::HEM_7600T,
        .model = "HEM-7600T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x0286,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        // Ninety. The six hundred-slot variants have their own profile;
        // raising this one would change what every other variant here already
        // does, for a number nobody has measured on hardware either way.
        .users = {{{0x02AC, 90, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7600T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7600T_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_TESTED,
    },
    // HEM-7600T field for field, hundred-slot ring.
    {
        .id = OmronProfileId::HEM_7600T_DEEP,
        .model = "HEM-7600T-DEEP",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x0286,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        .users = {{{0x02AC, 100, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7600T_DEEP_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7600T_DEEP_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    // Everything below is second-hand and none of it has been near one of these
    // cuffs, so all of it is REFERENCE_ONLY. Selecting one of these reads
    // whatever region it claims, and a wrong address yields plausible-looking
    // garbage rather than an obvious failure, which is why the confidence level
    // is surfaced in the log at setup.
    {
        // No hardware. These three are usually filed under HEM-7600T with a
        // 14-byte record at 0x02AC, which is wrong; they are identical to each
        // other field for field and share none of that map.
        //
        // Transport is inherited rather than known: the WLS3.0 lineage is
        // classic stack with a host-programmed key. The clock offset is
        // NO_CLOCK rather than a guess, because the settings blocks are
        // 26/26/18 here and no definition places a clock, so the profile reads records
        // and does not touch the clock.
        .id = OmronProfileId::HEM_9601T,
        .model = "HEM-9601T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_24_GUARDED,
        .record_size = 0x18,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0356,
        .settings_write_address = 0x03B8,
        // Pointer region ahead of the user blocks.
        .settings_index_region_size = 20,
        .user_block_size = 26,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0,
        .time_region_end = 0,
        // 350 slots; cursor at offset 0, unsent counter at 4, fourteen-bit
        // cursor value and a one-slot bias.
        .users = {{{0x041A, 350, 0x00, 0x04, 0x3FFF, -1}, {0x0000, 0, 0x00, 0x00, 0x0000, 0}}},
        .user_count = 1,
        .equivalent_model_ids = HEM_9601T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_9601T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    // HEM-9601T field for field, with a thousand-slot ring. The deepest this
    // catalog carries, by a factor of three.
    {
        .id = OmronProfileId::HEM_9700T,
        .model = "HEM-9700T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_24_GUARDED,
        .record_size = 0x18,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0356,
        .settings_write_address = 0x03B8,
        .settings_index_region_size = 20,
        .user_block_size = 26,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0,
        .time_region_end = 0,
        .users = {{{0x041A, 1000, 0x00, 0x04, 0x3FFF, -1}, {0x0000, 0, 0x00, 0x00, 0x0000, 0}}},
        .user_count = 1,
        .equivalent_model_ids = HEM_9700T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_9700T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // Same lineage and settings geometry as HEM-7188T1-LE, to the byte.
        // It differs in the ring, 60 slots against 30, and in being single-user
        // where that one has a second area carrying no blood pressure.
        //
        // Beware the 36 KB block this family keeps at 0x010000: it is some other
        // measurement, not the readings, and its address does not fit the
        // 16-bit field this component uses. Blood pressure is the 16-byte
        // record at 0x01C4.
        .id = OmronProfileId::HEM_7191T1,
        .model = "HEM-7191T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        // Not stated for this family, so it follows its sibling.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        // Blocks 10/10/16/8 after a 24-byte pointer region: the clock runs
        // 44 to 60.
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        // 60 slots; cursor at 0, unsent counter at 4, eight-bit cursor value
        // and a one-slot bias.
        .users = {{{0x01C4, 60, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7191T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7191T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // The same shape again, with the widest pointer region in the catalog
        // and the clock further into the settings block because of it. Its
        // 192 KB block at 0x010000 is not blood pressure either.
        .id = OmronProfileId::HEM_7440T1,
        .model = "HEM-7440T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x008E,
        .settings_index_region_size = 34,
        .user_block_size = 10,
        // Blocks 10/10/16/48/8 after a 34-byte pointer region: the clock runs
        // 54 to 70.
        .clock_fields_offset = 8,
        .time_region_start = 54,
        .time_region_end = 70,
        .users = {{{0x02F0, 100, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7440T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7440T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // These devices record five kinds of measurement and blood pressure is
        // the fifth area. The first is the pedometer's daily data, which is why
        // this family is easily mistaken for having a 32-byte record.
        //
        // Two maps, not one: 6401T and 6402T keep readings at 0x1350 with a
        // 16-byte stride, 6410T and 6411T at 0x5590 with 32. HEM-6410T-Z is
        // commonly filed under the first and belongs to the second.
        //
        // Transport is inherited rather than known: WLB1.0 is the oldest
        // connect type here, so the family stays on classic-stack defaults.
        //
        // NO_CLOCK, deliberately. The clock is the FIRST settings block
        // on this family rather than the last, and the per-user field at offset
        // 6 is a write cursor rather than an unsent counter. Reading records
        // needs none of that; writing under the wrong map would step on the
        // index of hardware nobody here can test.
        .id = OmronProfileId::HEM_6401T,
        .model = "HEM-6401T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::PLAIN_DATE_VITAL,
        .record_size = 0x10,
        // 64 - 6 - 2, as everywhere else here. 0x10 also circulates for this
        // family, and is the first thing to try if reads come back short.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0100,
        .settings_write_address = 0x0160,
        // Pointer region ahead of the user blocks.
        .settings_index_region_size = 16,
        .user_block_size = 8,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0,
        .time_region_end = 0,
        // Cursor at 6, unsent counter at 14, fourteen-bit cursor value and no
        // slot bias. One user, which the memory map and Omron's model catalog
        // agree on.
        .users = {{{0x1350, 100, 0x06, 0x0E, 0x3FFF, 0}, {0x0000, 0, 0x00, 0x00, 0x0000, 0}}},
        .user_count = 1,
        .equivalent_model_ids = HEM_6401T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6401T_ALIASES),
        // 30 seconds, as this family states. Inert while the clock is not
        // written, and kept so nobody has to look it up twice.
        .clock_sync_threshold_s = 30,
        // These two variants state no record sequence number.
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        // No cuff fit and no consecutive index: this family measures five things
        // at once and reports the least about each. Both entities would
        // otherwise read a permanent OFF.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~(MEASUREMENT_FIELD_CUFF | MEASUREMENT_FIELD_CONSECUTIVE),
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // Same family, second map: data_5 at 0x5590 with a 32-byte stride. The
        // first 13 bytes are the 6401T record field for field; the rest is
        // material this component does not publish, including a four-byte
        // sequence number at 24 that the two-byte reader here cannot carry.
        .id = OmronProfileId::HEM_6410T,
        .model = "HEM-6410T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::PLAIN_DATE_VITAL,
        .record_size = 0x20,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0100,
        // Settings write base is 0x170 here, not the 0x160 of the 6401T.
        .settings_write_address = 0x0170,
        .settings_index_region_size = 16,
        .user_block_size = 8,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0,
        .time_region_end = 0,
        .users = {{{0x5590, 100, 0x06, 0x0E, 0x3FFF, 0}, {0x0000, 0, 0x00, 0x00, 0x0000, 0}}},
        .user_count = 1,
        .equivalent_model_ids = HEM_6410T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6410T_ALIASES),
        // Refresh the clock every session.
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        // Same lean index as the 6401T half of this family.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~(MEASUREMENT_FIELD_CUFF | MEASUREMENT_FIELD_CONSECUTIVE),
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_6231T,
        .model = "HEM-6231T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 16,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 90, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_6231T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6231T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_6320T,
        .model = "HEM-6320T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0F74,
        .settings_write_address = 0x0F9A,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        .users = {{{0x0370, 100, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_6320T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6320T_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        // No variant of this family carries it: these records say nothing about
        // a position in a TruRead series.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~MEASUREMENT_FIELD_CONSECUTIVE,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_6321T,
        .model = "HEM-6321T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0F74,
        .settings_write_address = 0x0F9A,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        .users = {{{0x0370, 100, 0x00, 0x04, 0x00FF, -1}, {0x08E8, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_6321T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_6321T_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        // As 6320T: not carried.
        .measurement_fields = MEASUREMENT_FIELDS_ALL & ~MEASUREMENT_FIELD_CONSECUTIVE,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7136T,
        .model = "HEM-7136T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 16,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 60, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7136T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7136T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7150T,
        .model = "HEM-7150T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        // 64 - 6 - 2; see the field's own declaration.
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 60, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7150T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7150T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    // HEM-7150T field for field, hundred-slot ring.
    {
        .id = OmronProfileId::HEM_7157T_DEEP,
        .model = "HEM-7157T-DEEP",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 100, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7157T_DEEP_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7157T_DEEP_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7188T1,
        .model = "HEM-7188T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 14, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7188T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7188T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7361T,
        .model = "HEM-7361T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x10,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 0x10,
        .user_block_size = 14,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x0098, 100, 0x00, 0x04, 0x00FF, -1}, {0x06D8, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7361T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7361T_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7380T1,
        .model = "HEM-7380T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x01C4, 100, 0x00, 0x04, 0x00FF, -1}, {0x0804, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7380T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7380T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7382T1,
        .model = "HEM-7382T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0058,
        .settings_index_region_size = 28,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x30,
        .time_region_end = 0x40,
        .users = {{{0x080C, 60, 0x00, 0x04, 0x00FF, -1}, {0x0BCC, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7382T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7382T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        .id = OmronProfileId::HEM_7386T1,
        .model = "HEM-7386T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0058,
        .settings_index_region_size = 28,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x30,
        .time_region_end = 0x40,
        .users = {{{0x080C, 100, 0x00, 0x04, 0x00FF, -1}, {0x0E4C, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7386T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7386T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // Settings and first user are HEM-7155T-K4's, which is also WLD2.0 and
        // where everything below the map comes from. The second user is not:
        // 0x0928 against K4's 0x06A8. The file declares five data areas; only
        // the two of record size sixteen are people, the rest are other
        // measurements this component does not read.
        .id = OmronProfileId::HEM_1026T2,
        .model = "HEM-1026T2",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x02A4,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x02E8, 100, 0x00, 0x04, 0x00FF, -1}, {0x0928, 100, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_1026T2_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_1026T2_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // One person, not two. The file has further data areas at 0x0098 and
        // 0x0160, but they hold ten-byte records - a different measurement
        // sharing the memory, not a second user of this one.
        .id = OmronProfileId::HEM_7188T1_LE,
        .model = "HEM-7188T1-LE",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x01C4, 30, 0x00, 0x04, 0x00FF, -1}, UNUSED_USER}},
        .user_count = 1,
        .equivalent_model_ids = HEM_7188T1_LE_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7188T1_LE_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // HEM-7380T1's shape with the second user at 0x0584 instead of 0x0804,
        // which is why it cannot ride in that profile's alias list however
        // often it is listed there.
        .id = OmronProfileId::HEM_7196T1,
        .model = "HEM-7196T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0054,
        .settings_index_region_size = 24,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        .time_region_start = 0x2C,
        .time_region_end = 0x3C,
        .users = {{{0x01C4, 60, 0x00, 0x04, 0x00FF, -1}, {0x0584, 60, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7196T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7196T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // HEM-7382T1's shape, second user at 0x0D0C rather than 0x0BCC.
        .id = OmronProfileId::HEM_7377T1,
        .model = "HEM-7377T1",
        .gatt = &OMRON_MODERN_GATT,
        .security_mode = SecurityMode::OS_BOND,
        .bond_policy = BondPolicy::PERSISTENT,
        .unlock_mode = UnlockMode::TOKEN_KEY,
        .token_required = true,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::SAME_AS_RECORD,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x10,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0010,
        .settings_write_address = 0x0058,
        .settings_index_region_size = 28,
        .user_block_size = 10,
        .clock_fields_offset = 8,
        // 0x30, not the 0x2C this family is usually given. Its own file states
        // the clock fields at 56 and says nothing about the region; the two
        // variants beside it that do state one put it at 48 with the same 56.
        // Read at 0x2C the clock decodes four bytes early, and a session that
        // sets the time writes six bytes of date over the setting in front of
        // it. The user blocks end at 48, so they still fit.
        .time_region_start = 0x30,
        .time_region_end = 0x40,
        .users = {{{0x080C, 80, 0x00, 0x04, 0x00FF, -1}, {0x0D0C, 80, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7377T1_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7377T1_ALIASES),
        .clock_sync_threshold_s = 0,
        .record_sequence_offset = 10,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
    {
        // HEM-7322T in every field but the second user, which its file puts at
        // 0x0798 against the family's 0x0824. Classic transport, so it needs a
        // provisioned bind key exactly as its neighbour does.
        .id = OmronProfileId::HEM_7511T,
        .model = "HEM-7511T",
        .gatt = &OMRON_CLASSIC_GATT,
        .security_mode = SecurityMode::CUSTOM_KEY,
        .bond_policy = BondPolicy::NONE,
        .unlock_mode = UnlockMode::CLASSIC_KEY,
        .token_required = false,
        .byte_order = ByteOrder::LITTLE,
        .cursor_byte_order = CursorByteOrder::BIG,
        .record_format = RecordFormat::CLASSIC_VITAL_14,
        .record_size = 0x0E,
        .transmission_block_size = 0x38,
        .settings_read_address = 0x0260,
        .settings_write_address = 0x0286,
        .settings_index_region_size = 0x08,
        .user_block_size = 6,
        .clock_fields_offset = NO_CLOCK,
        .time_region_start = 0x14,
        .time_region_end = 0x1E,
        .users = {{{0x02AC, 90, 0x00, 0x04, 0x00FF, -1}, {0x0798, 90, 0x02, 0x06, 0x00FF, -1}}},
        .user_count = 2,
        .equivalent_model_ids = HEM_7511T_ALIASES,
        .equivalent_model_id_count = alias_count(HEM_7511T_ALIASES),
        .clock_sync_threshold_s = 600,
        .record_sequence_offset = NO_RECORD_SEQUENCE,
        .measurement_fields = MEASUREMENT_FIELDS_ALL,
        .confidence = OmronProfileConfidence::REFERENCE_ONLY,
    },
};

const char *profile_confidence_to_string(OmronProfileConfidence confidence) {
  switch (confidence) {
    case OmronProfileConfidence::HARDWARE_VERIFIED:
      return "hardware verified";
    case OmronProfileConfidence::REFERENCE_TESTED:
      return "reference tested, not on our hardware";
    case OmronProfileConfidence::REFERENCE_ONLY:
      return "transcribed from a catalog, unverified";
  }
  return "unknown";
}

static constexpr size_t PROFILE_CATALOG_SIZE = std::size(PROFILE_CATALOG);

static std::string_view trimmed_model(std::string_view model) {
  while (!model.empty()) {
    const char value = model.back();
    if (value != '\0' && value != ' ' && value != '\t' && value != '\r' && value != '\n')
      break;
    model.remove_suffix(1);
  }
  return model;
}

static bool model_equals(const char *candidate, std::string_view model) {
  return candidate != nullptr && candidate == model;
}

const OmronProfile &get_profile(OmronProfileId id) {
  for (const OmronProfile &profile : PROFILE_CATALOG) {
    if (profile.id == id)
      return profile;
  }
  return PROFILE_CATALOG[0];
}

bool profile_matches_model(const OmronProfile &profile, std::string_view model) {
  const std::string_view normalized = trimmed_model(model);
  if (normalized.empty())
    return false;
  if (model_equals(profile.model, normalized))
    return true;
  if (profile.equivalent_model_ids == nullptr)
    return false;
  for (size_t i = 0; i < profile.equivalent_model_id_count; i++) {
    if (model_equals(profile.equivalent_model_ids[i], normalized))
      return true;
  }
  return false;
}

const OmronProfile *profile_for_model(std::string_view model) {
  for (size_t i = 1; i < PROFILE_CATALOG_SIZE; i++) {
    if (profile_matches_model(PROFILE_CATALOG[i], model))
      return &PROFILE_CATALOG[i];
  }
  return nullptr;
}

const OmronProfile *profile_at(size_t index) {
  // Index 0 is the UNSUPPORTED sentinel, so callers enumerate from 1.
  if (index >= PROFILE_CATALOG_SIZE - 1)
    return nullptr;
  return &PROFILE_CATALOG[index + 1];
}

size_t profile_count() {
  return PROFILE_CATALOG_SIZE - 1;
}

}  // namespace esphome::omron
