#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/core/preferences.h"
#ifdef USE_API
#include "esphome/components/api/custom_api_device.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "omron_advertisement.h"
#include "omron_bond_cleanup.h"
#include "omron_command_writer.h"
#include "omron_connection_gate.h"
#include "omron_diagnostics.h"
#include "omron_entities.h"
#include "omron_harvest.h"
#include "omron_history_queue.h"
#include "omron_measurement.h"
#include "omron_memory.h"
#include "omron_model_id.h"
#include "omron_poll_plan.h"
#include "omron_profile_adapter.h"
#include "omron_profiles.h"
#include "omron_protocol.h"
#include "omron_publish.h"
#include "omron_record_store.h"
#include "omron_scheduler.h"
#include "omron_session.h"
#include "omron_settings_write.h"
#include "omron_standard_bp.h"
#include "omron_subscriptions.h"
#include "omron_transaction.h"
#include "omron_unlock.h"

#ifdef USE_ESP32

namespace esphome::omron {

// Transport and entities: GATT handles, splitting a command across channels,
// write types, bonding, discovery, and turning a decoded record into something
// Home Assistant can show. The shape of a session - what goes on the wire and
// in which order - is OmronSession's, which has no ESP-IDF in it and is
// therefore reachable from the host suite. Keep that line where it is.
//
// Verified on a HEM-7155T-MW3: reads, writes, and pairing that survives one
// long press of the cuff's button. Everything else in the catalog is
// second-hand and has never seen hardware - see OmronProfileConfidence.
class OmronBLEClient final : public esp32_ble_client::BLEClientBase,
                             public OmronSessionHost,
                             public OmronBondCleanupHost,
                             public OmronSubscriptionHost,
                             public OmronHistoryQueueHost
#ifdef USE_API
    ,
                             public esphome::api::CustomAPIDevice
#endif
{
 public:
  OmronBLEClient() = default;
  // One of these owns a GATT connection, a bond and a session. There is exactly
  // one per cuff and it is registered with the tracker by address; a copy would
  // hold the same handles twice.
  OmronBLEClient(const OmronBLEClient &) = delete;
  OmronBLEClient &operator=(const OmronBLEClient &) = delete;

  // Held, not inherited. Codegen binds entities through here, which is why the
  // reference is public; everything that fills them is friendship above.
  OmronEntities &entities() { return this->entities_; }

  void setup() override;
  void loop() override;
  void dump_config() override;

#ifdef USE_ESP32_BLE_DEVICE
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;
#endif
  void connect() override;
  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

  void set_profile(OmronProfileId profile_id);
  // `profile: auto`. Records the intent only; the profile arrives from the
  // device information read of the first connection.
  void set_profile_auto() { this->profile_auto_ = true; }
  void set_bind_key(const std::string &bind_key);
  void set_auth_timeout(uint32_t timeout_ms) { this->auth_timeout_ms_ = timeout_ms; }
  // Slots older than the newest, per person. Entities carry the newest; the
  // rest leave as events, because entity state cannot be backdated.
  void set_history_records(uint8_t count) { this->history_records_ = count; }
  // Seconds of drift tolerated before the clock is refreshed. Zero, the
  // default, refreshes every session.
  void set_clock_sync_threshold(int64_t seconds) {
    this->clock_sync_threshold_s_ = seconds;
    this->clock_sync_threshold_set_ = true;
  }
  // Records stamped before this are dropped whole: no entity, no event, no
  // watermark. A cuff whose clock was never set stamps every reading with one
  // default date, so those readings are real and their timestamps are not, and
  // several sharing a second cannot even be ordered. Off unless configured: on
  // such a cuff it empties a user who has nothing newer.
  void set_ignore_records_before(uint16_t year, uint8_t month, uint8_t day) {
    this->ignore_before_epoch_ = civil_seconds(OmronDateTime{year, month, day, 0, 0, 0});
    this->ignore_before_set_ = true;
  }
  // Leaving the end opcode out keeps the cuff blinking Err and buys nothing.
  // A lever, on by default.
  void set_end_session(bool end_session) { this->end_session_ = end_session; }
  // One-based, zero for off. Registers this node with the cuff as that user,
  // which is what stops the cuff discarding the bond. See
  // OmronSession::maybe_queue_registration_writes_.
  void set_register_as_user(uint8_t user_number) { this->register_as_user_ = user_number; }
  void set_exchange_identity_keys(bool enabled) { this->exchange_identity_keys_ = enabled; }
  void set_accept_security_request(bool enabled) { this->accept_security_request_ = enabled; }
  // Per user, because the setting is. One purpose per session, so several
  // users take several sessions.
  void set_birth_date(uint8_t user_number, uint16_t year, uint8_t month, uint8_t day) {
    if (user_number == 0 || user_number > OMRON_MAX_USERS)
      return;
    this->birth_dates_[user_number - 1] = BirthDate{year, month, day};
  }
  // Whose birth date may be written without registering as them. Off unless
  // yaml names a user, because it writes a user block in a session that
  // registers nobody - a shape no other host sends.
  void allow_birth_date_write(uint8_t user_number) {
    if (user_number >= 1 && user_number <= OMRON_MAX_USERS)
      this->write_birth_date_users_ |= static_cast<uint8_t>(1U << (user_number - 1));
  }
  void set_full_read_on_pairing(bool full_read) { this->full_read_on_pairing_ = full_read; }
  void set_bond_cleanup_timeout(uint32_t timeout_ms) { this->bond_cleanup_.set_timeout_ms(timeout_ms); }
  // Overrides the catalog's bond policy. The modern 7155T family is often
  // filed as per-session, but the cuff keeps its own record: drop ours and it
  // stops answering outside -P- mode, which is where every post-measurement
  // reconnect happens.
  void set_keep_bond(bool keep_bond) {
    this->keep_bond_ = keep_bond;
    this->keep_bond_set_ = true;
  }
  // Overrides the catalog's security mode. Set false to connect and go straight
  // to discovery without bonding at all.
  void set_require_bond(bool require_bond) {
    this->require_bond_ = require_bond;
    this->require_bond_set_ = true;
  }
  void request_poll();
  // Arms the one write this component makes to the cuff's own configuration:
  // programming a pairing key. Nothing happens until the next session, and the
  // cuff only accepts it while its display blinks -P-.
  void request_pairing();
  // Drops this node's own bond record so the next session pairs afresh. The
  // cuff takes a user block only from a session that pairs, and one only pairs
  // when this side holds no bond, so without this a node that has bonded once
  // can never register a birth date or a different user number.
  void forget_bond();
  // Off stands this node down: no new connections, and a session in flight is
  // torn down so the cuff is free for whoever else wants it.
  void set_ble_user_enabled(bool enabled);
#ifdef USE_TIME
  // Without a time source the cuff's clock is still read and published; only
  // the drift against real time is unknowable.
  void set_time(time::RealTimeClock *clock_source) { this->time_ = clock_source; }
#endif

  // OmronSessionHost: what the session layer needs and only the link can answer.
  WriteDispatch session_write(SessionChannel channel, std::span<const uint8_t> data, bool prefer_no_response) override;
  uint32_t session_now_ms() override;
  bool session_wall_clock(OmronDateTime &now) override;
  bool session_random_nonce(std::span<uint8_t> data) override;
  bool session_request_link_encryption() override;
  void session_settings_read(const std::vector<uint8_t> &settings) override;
  void session_transfer_complete() override;
  void session_failed(const char *reason, int code) override;
  const char *session_address() override { return this->address_str(); }

  // Only an in-flight cleanup may block. A failed one must not, or the client
  // deadlocks: the block itself makes connect and disconnect unreachable, and
  // those are the only paths that can start another cleanup and clear it. A
  // stale bond is recoverable - the peer rejects auth and we bond again.
  bool bond_cleanup_blocking() const { return this->bond_cleanup_.pending(); }

  // OmronBondCleanupHost: the three things the cleanup machine needs that only
  // ESP-IDF can answer.
  uint32_t bond_now_ms() override;
  BondLookupResult bond_lookup(int &error) override;
  bool bond_remove(int &error) override;
  void bond_forget_attribute_cache() override;
  const char *bond_address() override;

  // OmronSubscriptionHost. The three things only ESP-IDF can answer, plus what
  // this class does with an outcome the machine has no opinion about.
  uint32_t subscription_now_ms() override;
  bool subscription_register(uint16_t characteristic_handle, int &error) override;
  void subscription_ready() override;
  void subscription_failed(const char *reason, int error) override;
  void subscription_dropped_optional(uint16_t characteristic_handle, const char *why, int error) override;
  bool subscription_needs_encryption(int status) override;
  const char *subscription_address() override;
  void subscription_retrying(uint16_t characteristic_handle, uint8_t attempt, uint8_t of) override;

 protected:
  void on_disconnect_complete(esp_err_t reason) override;

  bool handle_connect_event_(esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
  bool handle_open_event_(esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
  bool event_matches_interface_(esp_gatt_if_t gattc_if) const;
  void apply_connection_action_(ConnectionAction action);
  void start_security_();
  void start_discovery_();
  void reset_session_();
  void fail_session_(const char *reason, int code = 0);
  // Best-effort end opcode on the way out of a failed session. Returns true if
  // one went on the wire, which is the caller's cue to let it drain before
  // dropping the link.
  bool close_memory_session_();

  bool resolve_gatt_and_subscribe_();
  bool add_notify_target_(uint16_t characteristic_handle, bool required);
  void handle_register_for_notify_(esp_ble_gattc_cb_param_t *param);
  // Standard device information, 0x180A. Once per boot: the strings do not
  // change while the cuff is powered. A failure is logged and dropped, since
  // none of it is needed to read a measurement.
  void start_device_information_();
  void request_next_device_information_();
  void handle_device_information_read_(esp_ble_gattc_cb_param_t *param);
  // What the cuff says it is, against what it was configured as. Runs off the
  // model read, which lands before any address from the profile is used.
  void check_reported_model_(const std::string &reported);
  OmronStack discovered_stack_();
  // Everything that follows from knowing which cuff this is. Runs at setup for
  // a configured profile, off the model read for a detected one.
  bool apply_profile_();
  // Fallback for a cuff this component cannot map: publish what the standard
  // service indicates and read no memory at all.
  bool begin_live_only_(const char *why);
  void apply_live_only_capabilities_();
  void apply_standard_features_(std::span<const uint8_t> data);
  bool subscribe_standard_measurement_(bool required);
  void handle_descriptor_write_(esp_ble_gattc_cb_param_t *param);
  void notifications_ready_();

  // One call per fragment; each acknowledged fragment resumes here.
  WriteDispatch dispatch_next_write_();
  void handle_characteristic_write_(esp_ble_gattc_cb_param_t *param);
  void handle_notification_(esp_ble_gattc_cb_param_t *param);
#ifdef USE_ESP32_BLE_DEVICE
  bool read_advertisement_flags_(const esp32_ble_tracker::ESPBTDevice &device, OmronAdvertisementFlags &flags);
#endif
  // Logs the settings block per user and publishes the two fields worth an
  // entity. Both, deliberately: the log is for a session being watched, the
  // entities for every session that is not.
  void report_user_settings_(const std::vector<uint8_t> &settings);
  // Called once at setup, after the poll layout is built.
  void configure_session_();
  void configure_key_distribution_();
  void log_local_address_();
  void finalize_record_transaction_();

  void publish_device_clock_();
  // Gathers what only this node knows: the cut-off, the saved watermarks, the
  // wall clock, the room left in the event queue.
  HarvestRequest build_harvest_request_();

  // OmronHistoryQueueHost. The queue paces itself to one event per loop pass:
  // two rings' worth back to back overflowed the API write buffer, whose
  // overflow path asks the heap for more, and a failed allocation with
  // exceptions stubbed out aborts rather than returning.
  uint32_t history_now_ms() override;
  void history_emit(const HistoryEvent &pending) override;
  void history_save_watermark(uint8_t user_index, int64_t epoch) override;
  uint32_t history_pref_hash_(uint8_t user_index) const;
  void publish_selected_measurement_(uint8_t user_index, const HarvestedRecord &selected);
  void publish_standard_measurement_(std::span<const uint8_t> data);
  void publish_status_(const std::string &status);
  void finish_poll_(const std::string &status);
  void finish_diagnostics_(bool success);

  int8_t rx_channel_for_handle_(uint16_t handle) const;
#ifdef USE_TIME
  // ESPTime's fields are already local wall clock, which is the same thing the
  // cuff stores. Its timestamp is not: that one is UTC.
  static OmronDateTime local_now_(const ESPTime &now) {
    return OmronDateTime{now.year,
                         static_cast<uint8_t>(now.month),
                         static_cast<uint8_t>(now.day_of_month),
                         static_cast<uint8_t>(now.hour),
                         static_cast<uint8_t>(now.minute),
                         static_cast<uint8_t>(now.second)};
  }
#endif
  // Whether a key slot holds anything, for a log line that must not carry the
  // key itself.
  static bool all_zero_(std::span<const uint8_t> data);
  // The same value with the UTC offset that applied on its own date, which is
  // what Home Assistant needs before it will treat a string as a timestamp.
  // Falls back to the naive form when there is no time source to ask.
  std::string format_local_datetime_(const OmronDateTime &value) const;

  bool requires_os_bond_() const;
  bool requires_per_session_cleanup_() const;
  // A cuff that drops the link while bonding has not refused, it has not
  // started. Re-establishing the whole connect is the answer rather than an
  // error path, and only running out of attempts counts as a failure.
  void maybe_retry_connect_();
  void begin_bond_cleanup_(const char *reason, bool force = false);
  // BTA_DM_AUTH_FAIL_BASE + SMP_ENC_FAIL, which is HCI_ERR_KEY_MISSING: the
  // cuff says it does not know the key, not that the key is wrong. It discards
  // the bond of a host that never registers itself, so seeing this again means
  // registration stopped happening rather than that key handling broke.
  static constexpr int AUTH_FAIL_ENCRYPTION = 97;
  BondLookupResult lookup_own_bond_(esp_err_t *error);

  // Three, which is enough for a peer that drops once or twice while bonding
  // and few enough that a cuff which is simply gone stops being chased.
  static constexpr uint8_t CONNECT_ATTEMPTS = 3;
  // Discovery on these cuffs completes in well under a second when it works at
  // all; the write acknowledgement is a single ATT round trip.
  static constexpr uint32_t DISCOVERY_TIMEOUT_MS = 15000;
  static constexpr uint32_t WRITE_ACK_TIMEOUT_MS = 5000;
  // A handful of CCCD writes. Its own constant, not auth_timeout: two unrelated
  // phases behind one knob get retuned by accident.
  static constexpr uint32_t SUBSCRIPTION_TIMEOUT_MS = 10000;
  static constexpr int MAX_BOND_RECORDS_TO_SCAN = 64;
  // Signal strength is a diagnostic, not a measurement. One value a minute says
  // everything a connect-poll-disconnect device can say about its link quality.
  static constexpr uint32_t RSSI_PUBLISH_INTERVAL_MS = 60000;
  // Unset means "whatever the profile says", which is zero for most models and
  // 600 s for six families. Zero refreshes every session and costs no extra
  // write: the clock frame goes out regardless because it carries the user
  // marker, so this only decides whether its bytes are current.
  bool clock_sync_threshold_set_{false};
  int64_t clock_sync_threshold_s_{0};
  static constexpr uint32_t NOTIFY_SETTLE_MS = 750;
  // Long enough for the end opcode to reach the cuff before the link drops.
  static constexpr uint32_t END_TEARDOWN_DELAY_MS = 300;

  const OmronProfile *profile_{nullptr};
  OmronProfileId profile_id_{};
  bool profile_auto_{false};
  // Whether this session is the one that bonded. Held here because adopting a
  // detected profile reconfigures the session and that resets it, and this flag
  // decides whether the registration write goes out at all.
  bool paired_this_session_{false};
  // Live-only: no profile could be resolved, so nothing reads memory this
  // session and the link stays up for a bounded window in case the cuff
  // indicates a measurement over the standard service.
  bool live_only_{false};
  // When the window opened, not when it closes. A stored deadline is compared
  // against millis() absolutely, and that comparison is the one thing in this
  // component that does not survive the wrap.
  uint32_t live_only_started_at_{0};
  bool live_only_window_open_{false};
  bool live_only_published_{false};
  StandardBpFeatures standard_features_{};
  bool standard_features_known_{false};
  // Long enough for a measurement already under way to finish, short enough
  // that an unidentifiable cuff does not hold the connection slot.
  static constexpr uint32_t LIVE_ONLY_WINDOW_MS = 45000;
  // Kept rather than only logged: the model is read once per boot, so the line
  // saying the profile is wrong scrolls past exactly once.
  ProfileVerdict model_verdict_{ProfileVerdict::UNVERIFIED};

  uint32_t auth_timeout_ms_{20000};
  uint32_t auth_started_at_{0};

  OmronEntities entities_{};
  OmronBondCleanup bond_cleanup_{};

  OmronConnectionGate connection_gate_{};
  OmronDiagnostics diagnostics_{};
  bool security_started_{false};
  bool session_failure_handled_{false};
  bool session_started_{false};

  OmronBindKey bind_key_{};
  bool bind_key_set_{false};
  bool keep_bond_{false};
  bool keep_bond_set_{false};
  bool require_bond_{true};
  bool require_bond_set_{false};

  std::array<uint16_t, OMRON_MAX_GATT_CHANNELS> rx_handles_{};
  std::array<uint16_t, OMRON_MAX_GATT_CHANNELS> tx_handles_{};
  std::array<esp_gatt_char_prop_t, OMRON_MAX_GATT_CHANNELS> tx_properties_{};
  uint8_t rx_handle_count_{0};
  uint8_t tx_handle_count_{0};
  uint16_t unlock_handle_{0};
  esp_gatt_char_prop_t unlock_properties_{0};
  uint16_t standard_bp_handle_{0};
  uint8_t device_information_index_{0};
  uint16_t device_information_handle_{0};
  bool device_information_done_{false};

  OmronSubscriptions subscriptions_{};
  // The phase clock stays here: it bounds the whole subscription step, while the
  // per-target retry that notices a CCCD write which never comes belongs to the
  // machine. Two different questions, deliberately not one knob.
  uint32_t subscription_started_at_{0};
  uint32_t notifications_ready_at_{0};
  bool notifications_configured_{false};
  bool transaction_start_pending_{false};
  bool advertised_pairing_mode_{false};
  std::vector<uint8_t> last_logged_manufacturer_data_;

  PollLayout poll_layout_{};
  OmronRecordStore record_store_{};
  // What goes on the wire and in which order. Everything about the session
  // except the link it rides on.
  OmronSession session_{};
  uint8_t register_as_user_{0};
  uint8_t write_birth_date_users_{0};
  bool full_read_on_pairing_{false};
  bool exchange_identity_keys_{true};
  bool accept_security_request_{true};
  struct BirthDate {
    uint16_t year{0};
    uint8_t month{0};
    uint8_t day{0};
  };
  std::array<BirthDate, OMRON_MAX_USERS> birth_dates_{};

  // Unset in yaml means the whole written ring. An explicit 0 still means
  // "newest only, no events".
  uint8_t history_records_{HISTORY_RECORDS_ALL};
  bool ignore_before_set_{false};
  int64_t ignore_before_epoch_{0};
  bool end_session_{true};
  // Newest record already reported, per person, kept across reboots. Without it
  // every restart would replay the whole ring into Home Assistant as if those
  // measurements had just been taken.
  std::array<int64_t, OMRON_ENTITY_USER_SLOTS> history_epoch_{};
  std::array<ESPPreferenceObject, OMRON_ENTITY_USER_SLOTS> history_pref_{};
  // Owns the rule that the flash mark may only be written once the queue has
  // drained.
  OmronHistoryQueue history_{};

  // The command being fed to the link; which channel each fragment belongs on
  // is OmronCommandWriter's.
  OmronCommandWriter writer_{};
  SessionChannel outgoing_channel_{SessionChannel::PROTOCOL};
  bool outgoing_prefer_no_response_{false};
  uint16_t write_handle_{0};
  OmronPollScheduler scheduler_{};
  bool write_in_flight_{false};
  uint32_t write_started_at_{0};
  uint32_t discovery_started_at_{0};

  uint32_t rssi_published_at_{0};
  bool rssi_published_{false};

#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

  // Filled by the bond lookup: which keys the stored record actually carries.
  uint8_t bond_key_mask_{0};
  bool poll_requested_{false};
  // Connects spent on the poll the cuff is currently asking for. Reset when a
  // poll completes or when someone asks for a new one, not on every connect.
  uint8_t connect_attempt_{0};
  bool ble_user_enabled_{true};
  bool stale_bond_suspected_{false};
  // Set when the button is pressed during a session, spent by the teardown.
  bool forget_bond_requested_{false};
};

}  // namespace esphome::omron

#endif  // USE_ESP32
