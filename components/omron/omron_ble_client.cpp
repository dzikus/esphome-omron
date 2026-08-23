#include "omron_ble_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "omron_metrics.h"
#include "omron_standard_bp.h"

#ifdef USE_ESP32

namespace esphome::omron {

static const char *const TAG = "omron.ble";

namespace {

int8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

}  // namespace

void OmronBLEClient::set_bind_key(const std::string &bind_key) {
  this->bind_key_.fill(0);
  this->bind_key_set_ = false;
  if (bind_key.size() != this->bind_key_.size() * 2)
    return;
  for (size_t index = 0; index < this->bind_key_.size(); index++) {
    const int8_t high = hex_nibble(bind_key[index * 2]);
    const int8_t low = hex_nibble(bind_key[index * 2 + 1]);
    if (high < 0 || low < 0) {
      this->bind_key_.fill(0);
      return;
    }
    this->bind_key_[index] = static_cast<uint8_t>((high << 4) | low);
  }
  this->bind_key_set_ = true;
}

void OmronBLEClient::set_profile(OmronProfileId profile_id) {
  this->profile_id_ = profile_id;
  this->profile_ = &get_profile(profile_id);

  // Which entities this profile can ever fill is decided in omron_publish, from
  // the catalog and never from a model-name conditional. Per-record `available`
  // bits still form the second gate, so a decoder cannot publish a field it did
  // not validate.
  this->entities_.set_entity_capabilities(capabilities_for_profile(*this->profile_));
  // Do not reset the session here. Detection calls this from inside a live one,
  // and a reset wipes the subscribed handles, notifications_configured_ and
  // transaction_start_pending_ - the profile is adopted correctly and then the
  // transfer can never start, so the link sits idle until the cuff hangs up a
  // minute later. Only a caller running before setup() can get away with it,
  // because setup() resets anyway.
}

// Everything that follows from knowing which cuff this is. Split out of setup()
// because with `profile: auto` the answer does not exist until the cuff has
// reported its model, which is one connection later. Runs exactly once either
// way - at setup for a configured profile, off the DIS read for a detected one.
//
// A false return means the profile cannot be used at all. At setup that is fatal
// to the component; after detection it is fatal to the session, because by then
// the component is the only thing that can report it.
bool OmronBLEClient::apply_profile_() {
  if (this->profile_ == nullptr || this->profile_->id == OmronProfileId::UNSUPPORTED ||
      this->profile_->gatt == nullptr) {
    ESP_LOGE(TAG, "[%s] Unsupported Omron profile", this->address_str());
    return false;
  }
  const ProfileAdapterError adapter_error =
      make_poll_layout(*this->profile_, this->poll_layout_, this->history_records_);
  if (adapter_error != ProfileAdapterError::NONE) {
    ESP_LOGE(TAG, "[%s] Invalid poll layout: %s", this->address_str(), profile_adapter_error_to_string(adapter_error));
    return false;
  }
  // The session must not be able to read a value that changed under it half way
  // through a transfer, so this is the last moment anything may touch it: for a
  // configured profile every setter has already run, and for a detected one the
  // first frame is still behind the settle window.
  this->configure_session_();

  if (this->profile_->unlock_mode == UnlockMode::CLASSIC_KEY && !this->bind_key_set_) {
    ESP_LOGE(TAG, "[%s] Profile %s requires an already-provisioned bindkey", this->address_str(),
             this->profile_->model);
    return false;
  }
  // How many people a cuff stores is a property of the hardware, so the profile
  // decides it and yaml only says which of them an entity block belongs to. A
  // block naming a user this model does not have is a configuration mistake that
  // would otherwise show up as entities that stay unknown forever.
  for (uint8_t user_index = this->profile_->user_count; user_index < OMRON_ENTITY_USER_SLOTS; user_index++) {
    if (this->entities_.user_entities_bound_(user_index)) {
      ESP_LOGE(TAG, "[%s] Entities configured for user %u, but profile %s stores %u user(s); they never publish",
               this->address_str(), static_cast<unsigned>(user_index + 1), this->profile_->model,
               static_cast<unsigned>(this->profile_->user_count));
    }
  }

  // The mirror of the check above, and the one that bites. Only a block saying
  // `user: N` carries measurements; configure just the cuff's own kind and every
  // record is read correctly with nowhere to go, silently. Most likely on a
  // single-user cuff, where declaring the number feels redundant.
  bool any_user_bound = false;
  for (uint8_t user_index = 0; user_index < this->profile_->user_count; user_index++)
    any_user_bound = any_user_bound || this->entities_.user_entities_bound_(user_index);
  if (!any_user_bound) {
    ESP_LOGE(TAG,
             "[%s] No entity block declares a user, so no measurement can be published. Add `user: 1` to a "
             "sensor/text_sensor/binary_sensor block for profile %s, which stores %u user(s).",
             this->address_str(), this->profile_->model, static_cast<unsigned>(this->profile_->user_count));
  }
  return true;
}

void OmronBLEClient::setup() {
  esp32_ble_client::BLEClientBase::setup();

  // Wiring, not configuration: none of these needs to know which cuff this is,
  // and they must not move into configure_session_, which a detecting node does
  // not reach until its first session has got as far as reading device
  // information. The bond cleanup is the one that bites: its begin() returns in
  // silence with no host, so an unwired one leaves `forget bond` printing its
  // line, doing nothing and keeping the bond, after which the next pairing
  // session takes the "already bonded" path and never registers.
  this->session_.set_host(this);
  this->session_.set_diagnostics(&this->diagnostics_);
  this->bond_cleanup_.set_host(this);
  this->bond_cleanup_.set_diagnostics(&this->diagnostics_);
  this->subscriptions_.set_host(this);
  this->history_.set_host(this);

  if (this->profile_ == nullptr && !this->profile_auto_) {
    ESP_LOGE(TAG, "[%s] No Omron profile selected; refusing to connect", this->address_str());
    this->mark_failed();
    return;
  }

  if (this->profile_auto_) {
    // Nothing that needs a memory map may run yet. The component still connects,
    // subscribes off the discovered stack and reads device information; the
    // profile arrives from that read, before the first EEPROM frame goes out.
    ESP_LOGI(TAG, "[%s] Profile detection requested; the cuff's own model string decides", this->address_str());
  } else if (!this->apply_profile_()) {
    this->mark_failed();
    return;
  }

  if (this->history_records_ != 0) {
    for (uint8_t user_index = 0; user_index < OMRON_ENTITY_USER_SLOTS; user_index++) {
      this->history_pref_[user_index] =
          global_preferences->make_preference<int64_t>(this->history_pref_hash_(user_index));
      if (!this->history_pref_[user_index].load(&this->history_epoch_[user_index]))
        this->history_epoch_[user_index] = 0;
      // Seeded, not just remembered: the queue is what every later comparison
      // asks, so a mark restored from flash has to reach it or the first session
      // after a reboot re-sends everything the node has already emitted.
      this->history_.seed_watermark(user_index, this->history_epoch_[user_index]);
    }
#if !defined(USE_API) || !defined(USE_API_HOMEASSISTANT_SERVICES)
    ESP_LOGW(TAG,
             "[%s] history_records is set, but the events it produces need 'homeassistant_services: true' under "
             "'api:'. Older records will be read and logged only.",
             this->address_str());
#endif
  }

  this->reset_session_();
  if (this->profile_ == nullptr) {
    // Detecting. There is no model to name and no confidence to report until the
    // cuff has been asked, and both are logged where the answer arrives.
    // Not an early return: everything below this point has to keep running for a
    // detected profile too, and a return here would silently exempt it.
    this->entities_.publish_profile_entity_("detecting");
  } else {
    this->entities_.publish_profile_entity_(this->profile_->model);
    // Most of the catalog is second-hand and has never seen one of these cuffs.
    // Say so at startup: a wrong memory map reads the wrong EEPROM region and
    // yields plausible numbers rather than an error.
    if (this->profile_->confidence == OmronProfileConfidence::REFERENCE_ONLY) {
      ESP_LOGW(TAG, "[%s] Profile %s is %s. Readings from it are unproven.", this->address_str(), this->profile_->model,
               profile_confidence_to_string(this->profile_->confidence));
    } else {
      ESP_LOGI(TAG, "[%s] Profile %s: %s", this->address_str(), this->profile_->model,
               profile_confidence_to_string(this->profile_->confidence));
    }
  }
  if (this->requires_per_session_cleanup_()) {
    this->begin_bond_cleanup_("startup preflight");
  }
}

void OmronBLEClient::dump_config() {
  // Per profile, not a blanket EXPERIMENTAL: confidence is a property of the
  // memory map, and the boot log is where somebody looks for it.
  ESP_LOGCONFIG(TAG, "Omron BLE client (%s)",
                this->profile_ != nullptr ? profile_confidence_to_string(this->profile_->confidence)
                : this->profile_auto_     ? "detecting the profile from the cuff"
                                          : "no profile selected");
  esp32_ble_client::BLEClientBase::dump_config();
  if (this->profile_ != nullptr && this->profile_->gatt != nullptr) {
    // Bond before discovery and drop the bond afterwards are both overridable
    // in yaml, so print what this hub will actually do rather than what the
    // catalog says. Printing the catalog reads as proof that a session bonded
    // when an override had switched that off.
    const bool bond_first = this->requires_os_bond_();
    const char *bond_first_source = "";
    if (this->require_bond_set_)
      bond_first_source = bond_first == this->require_bond_ ? " (yaml)" : " (yaml overridden: cleanup needs it)";
    // "all" rather than 255, which is a sentinel and not a count anybody set.
    char history_records_text[16];
    const char *history_records_label = "all";
    if (this->history_records_ != HISTORY_RECORDS_ALL) {
      snprintf(history_records_text, sizeof(history_records_text), "%u", static_cast<unsigned>(this->history_records_));
      history_records_label = history_records_text;
    }
    ESP_LOGCONFIG(TAG,
                  "  Profile ID: %u\n"
                  "  Security mode: %u (catalog)\n"
                  "  Bond policy: %u (catalog)\n"
                  "  Bond before discovery: %s%s\n"
                  "  Drop bond after session: %s%s\n"
                  "  Token required: %s\n"
                  "  Bind key configured: %s\n"
                  "  History records: %s\n"
                  "  GATT RX/TX channels: %u/%u\n"
                  "  AUTH timeout: %u ms\n"
                  "  Bond cleanup timeout: %u ms",
                  static_cast<unsigned>(this->profile_id_), static_cast<unsigned>(this->profile_->security_mode),
                  static_cast<unsigned>(this->profile_->bond_policy), YESNO(bond_first), bond_first_source,
                  YESNO(this->requires_per_session_cleanup_()), this->keep_bond_set_ ? " (yaml)" : "",
                  YESNO(this->profile_->token_required), YESNO(this->bind_key_set_), history_records_label,
                  static_cast<unsigned>(this->profile_->gatt->rx_channel_count),
                  static_cast<unsigned>(this->profile_->gatt->tx_channel_count),
                  static_cast<unsigned>(this->auth_timeout_ms_),
                  static_cast<unsigned>(this->bond_cleanup_.timeout_ms()));
  }
}

void OmronBLEClient::loop() {
  esp32_ble_client::BLEClientBase::loop();

  // Ahead of the state gates below: history outlives the session that produced
  // it, and the queue has to keep draining while the link is torn down and the
  // bond is being removed.
  if (this->history_.tick(millis()))
    this->enable_loop();

  // BLEClientBase keeps INIT while the shared stack is inactive.
  if (this->state() == esp32_ble_tracker::ClientState::INIT)
    return;

  if (this->bond_cleanup_.pending()) {
    // Only while the link is idle. Inspecting the bond list mid-connection is
    // not something ESP-IDF promises anything about, and the machine's own
    // timeout clock does not start until this first runs - so a slow teardown
    // does not eat the budget.
    if (this->state() == esp32_ble_tracker::ClientState::IDLE)
      this->bond_cleanup_.tick(millis());
    if (this->bond_cleanup_.pending())
      this->enable_loop();
    return;
  }

  if (this->security_started_ && !this->connection_gate_.auth_ok() && !this->connection_gate_.failed() &&
      (this->state() == esp32_ble_tracker::ClientState::CONNECTING ||
       this->state() == esp32_ble_tracker::ClientState::CONNECTED) &&
      millis() - this->auth_started_at_ >= this->auth_timeout_ms_) {
    this->fail_session_("Timed out waiting for bonded AUTH_CMPL");
  }

  // A peer that stays connected but never finishes service discovery, or never
  // acknowledges a write-with-response, would otherwise hold the connection
  // slot until something else tears the link down.
  if (this->connection_gate_.discovery_started() && this->diagnostics_.phase == SessionPhase::DISCOVERING &&
      millis() - this->discovery_started_at_ >= DISCOVERY_TIMEOUT_MS) {
    this->fail_session_("Timed out waiting for SEARCH_CMPL");
  }
  if (this->write_in_flight_ && millis() - this->write_started_at_ >= WRITE_ACK_TIMEOUT_MS) {
    this->fail_session_("Timed out waiting for the write-with-response acknowledgement");
  }

  // A target the stack took and then never answered for. The machine owns that
  // clock, because there is no event to wait on - the base class swallowed the
  // descriptor lookup - and asking again is the only recovery. It ticks only
  // while subscribing, so the parked-for-encryption state is left to its own
  // timeout rather than being retried underneath it.
  this->subscriptions_.tick(millis());

  if (!this->subscriptions_.empty() && !this->notifications_configured_ &&
      millis() - this->subscription_started_at_ >= SUBSCRIPTION_TIMEOUT_MS) {
    this->fail_session_("Timed out configuring GATT notifications");
  }

  if (this->transaction_start_pending_ && this->notifications_configured_ &&
      millis() - this->notifications_ready_at_ >= NOTIFY_SETTLE_MS) {
    // A detected profile arrives from the device information read, which runs
    // inside this same settle window. If the window closes without one there is
    // no memory map to read, and opening the transfer would put an address on
    // the wire that nothing chose.
    this->transaction_start_pending_ = false;
    if (this->profile_ != nullptr && !this->live_only_) {
      this->session_.begin(this->unlock_handle_ != 0);
    } else if (!this->live_only_ && !this->begin_live_only_("Profile detection did not identify this cuff")) {
      this->fail_session_("Profile detection did not identify this cuff");
    } else {
      // Either the fallback was already chosen when no Omron service turned up,
      // or it was chosen just now because the model string meant nothing. Both
      // arm the window here, where the subscription is known to be in place.
      this->live_only_started_at_ = millis();
      this->live_only_window_open_ = true;
    }
  }

  // The live-only window. Nothing here can make a measurement happen; it exists
  // so the link is up if one does, and so the node says which of the two it was
  // rather than sitting connected forever.
  if (this->live_only_ && this->live_only_window_open_ &&
      millis() - this->live_only_started_at_ >= LIVE_ONLY_WINDOW_MS) {
    this->live_only_window_open_ = false;
    this->finish_poll_(this->live_only_published_ ? "live measurement received over the standard service"
                                                  : "live only: the cuff indicated no measurement");
  }

  // Reply timeouts and their retries belong to the protocol, not to the link.
  this->session_.tick(millis());
}

#ifdef USE_ESP32_BLE_DEVICE
bool OmronBLEClient::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  const bool is_our_cuff = device.address_uint64() == this->get_address();
  const uint32_t now = millis();
  if (is_our_cuff) {
    // Read from every advertisement but published far more rarely. An awake
    // cuff advertises several times a second, and publishing each one buries the
    // log and the recorder under a diagnostic nobody reads at that resolution.
    if (!this->rssi_published_ || now - this->rssi_published_at_ >= RSSI_PUBLISH_INTERVAL_MS) {
      this->rssi_published_ = true;
      this->rssi_published_at_ = now;
      this->entities_.publish_rssi_entity_(device.get_rssi());
    }
  }
  if (!this->ble_user_enabled_)
    return false;
  // A node configured for detection has no profile yet and gets one only by
  // connecting, so refusing the advertisement here is the first half of a
  // deadlock: no advertisement, no session, no model, no profile, forever.
  if ((this->profile_ == nullptr && !this->profile_auto_) || this->bond_cleanup_blocking())
    return false;
  if (!this->auto_connect_ && !this->poll_requested_)
    return false;

  // Set when the frame asks for a session in its own right, which the silence
  // window may not refuse. Declared here because the flags go out of scope
  // first.
  bool advertisement_demands_session = false;
  if (is_our_cuff) {
    OmronAdvertisementFlags flags;
    const bool readable = this->read_advertisement_flags_(device, flags);
    // Logged even when a button press is about to bypass the gate: whether the
    // cuff was offering pairing separates "it refuses outside pairing mode"
    // from "it offered and we failed".
    if (readable) {
      // Slots, not registrations. The field is one less than the number of
      // sequence blocks, so a two-person cuff reads 1 forever whoever has
      // paired, and it says nothing about how many phones are registered.
      ESP_LOGD(TAG, "[%s] Advertisement: pairing=%s invalid_time=%s forced_transfer=%s user_slots=%u%s",
               this->address_str(), YESNO(flags.pairing_mode), YESNO(flags.invalid_time), YESNO(flags.forced_transfer),
               static_cast<unsigned>(flags.user_register_count) + 1U,
               this->poll_requested_ ? " (gate bypassed by request)" : "");
      // Remembered because the connection that follows cannot see the
      // advertisement, and whether pairing was on offer decides whether
      // touching security at all is safe.
      this->advertised_pairing_mode_ = flags.pairing_mode;
      advertisement_demands_session = flags.wants_session();
    }
    // Advertising at all is the request, because the cuff only starts when its
    // button is pressed or a measurement ends. Waiting for a flag bit instead
    // leaves a transfer press advertising with none of them set and nothing
    // happening. The scheduler's minimum gap, not the flags, is what stops this
    // reconnecting on every frame of the burst.
    if (!this->poll_requested_ && !readable) {
      ESP_LOGV(TAG, "[%s] Advertisement carries no readable Omron flags; not connecting", this->address_str());
      return false;
    }
  }

  // The cuff re-advertises every few hundred milliseconds while awake, so
  // without this gate each frame would start a fresh connect.
  if (is_our_cuff)
    this->scheduler_.note_advertisement(now, advertisement_demands_session);
  const bool busy = this->state() != esp32_ble_tracker::ClientState::IDLE;
  if (!this->scheduler_.should_poll(now, busy, this->bond_cleanup_.pending()))
    return false;

  // BLEClientBase intentionally only parses when auto_connect_ is true. A
  // one-shot Poll now request temporarily opens that same path without changing
  // the configured persistent auto_connect setting.
  const bool configured_auto_connect = this->auto_connect_;
  if (this->poll_requested_)
    this->auto_connect_ = true;
  const bool parsed = esp32_ble_client::BLEClientBase::parse_device(device);
  this->auto_connect_ = configured_auto_connect;
  return parsed;
}
#endif

#ifdef USE_ESP32_BLE_DEVICE
bool OmronBLEClient::read_advertisement_flags_(const esp32_ble_tracker::ESPBTDevice &device,
                                               OmronAdvertisementFlags &flags) {
  const auto company = esp32_ble::ESPBTUUID::from_uint16(OMRON_COMPANY_ID);
  for (const auto &manufacturer : device.get_manufacturer_datas()) {
    if (manufacturer.uuid != company)
      continue;
    if (!parse_advertisement_flags(manufacturer.data, flags))
      continue;
    // Dumped whenever the bytes change rather than on every advertisement: the
    // cuff repeats itself several times a second while awake, and the only
    // interesting moment is the one where something moved. The decoded numbers
    // are per user, so this is also what says which person just measured.
    if (manufacturer.data != this->last_logged_manufacturer_data_) {
      this->last_logged_manufacturer_data_ = manufacturer.data;
      std::string sequences;
      for (uint8_t index = 0; index < flags.sequence_count; index++) {
        char entry[24];
        snprintf(entry, sizeof(entry), "%suser %u seq %u", sequences.empty() ? "" : ", ",
                 static_cast<unsigned>(index + 1), static_cast<unsigned>(flags.user_sequence[index]));
        sequences += entry;
      }
      ESP_LOGI(TAG, "[%s] Advertisement data (format 0x%02X): %s%s%s", this->address_str(),
               static_cast<unsigned>(flags.format),
               format_hex_pretty(manufacturer.data.data(), manufacturer.data.size()).c_str(),
               sequences.empty() ? "" : " -> ", sequences.c_str());
    }
    return true;
  }
  return false;
}
#endif

void OmronBLEClient::set_ble_user_enabled(bool enabled) {
  if (this->ble_user_enabled_ == enabled)
    return;
  this->ble_user_enabled_ = enabled;
  if (enabled) {
    ESP_LOGI(TAG, "[%s] BLE enabled; waiting for the cuff to ask for a session", this->address_str());
    return;
  }

  // A queued manual poll must not survive the switch, or it fires the moment
  // the switch comes back on and surprises whoever turned it off.
  this->poll_requested_ = false;
  ESP_LOGI(TAG, "[%s] BLE disabled by switch; standing down", this->address_str());
  if (this->state() != esp32_ble_tracker::ClientState::IDLE)
    this->disconnect();
}

void OmronBLEClient::request_poll() {
  this->poll_requested_ = true;
  this->connect_attempt_ = 0;
  this->scheduler_.request_poll();
  if (this->bond_cleanup_.failed()) {
    ESP_LOGW(TAG, "[%s] Poll requested after a failed bond cleanup; retrying with a possibly stale bond",
             this->address_str());
  } else if (this->bond_cleanup_.pending()) {
    ESP_LOGI(TAG, "[%s] Poll request queued until selective bond cleanup completes", this->address_str());
  } else if (this->state() == esp32_ble_tracker::ClientState::IDLE) {
    ESP_LOGI(TAG, "[%s] Poll requested; waiting for the next matching advertisement", this->address_str());
  } else {
    ESP_LOGI(TAG, "[%s] Poll requested while state=%s; it is queued behind the session in flight", this->address_str(),
             esp32_ble_tracker::client_state_to_string(this->state()));
  }
}

void OmronBLEClient::request_pairing() {
  if (!this->bind_key_set_) {
    ESP_LOGE(TAG, "[%s] Pairing needs a bindkey in the hub configuration; nothing armed", this->address_str());
    return;
  }
  // A cuff that unlocks with a session token does not implement key programming
  // and answers the request with nothing at all, at either frame width. Arming
  // it would cost a deliberate trip to the cuff and a held button for no result.
  if (this->profile_ != nullptr && this->profile_->unlock_mode != UnlockMode::CLASSIC_KEY) {
    ESP_LOGE(TAG, "[%s] Profile %s authenticates with a session token, not a stored key; it has no key to program",
             this->address_str(), this->profile_->model);
    return;
  }
  this->session_.arm_pairing();
  ESP_LOGI(TAG, "[%s] Pairing armed. Hold the cuff's button until -P- blinks; the next session programs the key.",
           this->address_str());
  this->request_poll();
}

// Registration rides on a session that pairs, and a session pairs only when our
// side holds no bond. So a node that has bonded once has no way back into a
// registering session, and a birth date or user number set afterwards would
// stay in yaml forever. Dropping our own record is the way back, and it is the
// same forced cleanup a stale bond already triggers.
void OmronBLEClient::forget_bond() {
  // Mid-session the record is in use: taking it out from under the link would
  // clobber the phase the timeouts read. The teardown path drops it instead.
  if (this->state() != esp32_ble_tracker::ClientState::IDLE) {
    this->forget_bond_requested_ = true;
    ESP_LOGI(TAG, "[%s] Bond will be dropped once the session in flight ends", this->address_str());
    return;
  }
  ESP_LOGI(TAG,
           "[%s] Dropping this node's bond. Hold the cuff's button until -P- blinks: the next session pairs afresh "
           "and registers this node in the cuff.",
           this->address_str());
  this->begin_bond_cleanup_("bond dropped on request", true);
}

void OmronBLEClient::connect() {
  // Every session rather than once at boot, because the boot line falls into
  // the seconds an OTA reboot costs the logger and a session then looks like it
  // proved something about a setting nobody could see.
  this->log_local_address_();
  this->configure_key_distribution_();
  // Detection has to be allowed through: it connects precisely because it has
  // no profile yet.
  if (this->profile_ == nullptr && !this->profile_auto_) {
    ESP_LOGE(TAG, "[%s] Connect rejected: profile is not configured", this->address_str());
    return;
  }
  if (this->bond_cleanup_blocking()) {
    ESP_LOGW(TAG, "[%s] Connect deferred: selective bond cleanup still pending", this->address_str());
    return;
  }

  const auto state = this->state();
  if (state == esp32_ble_tracker::ClientState::CONNECTING || state == esp32_ble_tracker::ClientState::CONNECTED ||
      state == esp32_ble_tracker::ClientState::ESTABLISHED || state == esp32_ble_tracker::ClientState::DISCONNECTING) {
    ESP_LOGW(TAG, "[%s] Connect rejected without resetting the active lifecycle (state=%s)", this->address_str(),
             esp32_ble_tracker::client_state_to_string(state));
    return;
  }

  this->reset_session_();
  this->diagnostics_.begin_session(millis());
  this->session_started_ = true;
  if (this->connect_attempt_ < CONNECT_ATTEMPTS)
    this->connect_attempt_++;
  this->scheduler_.note_poll_started(millis());
  esp32_ble_client::BLEClientBase::connect();
  if (this->state() == esp32_ble_tracker::ClientState::IDLE) {
    this->publish_status_("BLE connection request failed");
    this->finish_diagnostics_(false);
    this->reset_session_();
    if (this->requires_per_session_cleanup_())
      this->begin_bond_cleanup_("connection request failure");
    this->poll_requested_ = false;
    this->scheduler_.clear_request();
    return;
  }
  this->diagnostics_.phase = SessionPhase::WAITING_FOR_OPEN;
  this->poll_requested_ = false;
  this->scheduler_.clear_request();
}

bool OmronBLEClient::event_matches_interface_(esp_gatt_if_t gattc_if) const {
  return gattc_if == ESP_GATT_IF_NONE || gattc_if == this->gattc_if_;
}

bool OmronBLEClient::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  if (event == ESP_GATTC_OPEN_EVT)
    return this->handle_open_event_(gattc_if, param);

  if (event == ESP_GATTC_CONNECT_EVT)
    return this->handle_connect_event_(gattc_if, param);

  if (event == ESP_GATTC_SEARCH_RES_EVT) {
    if (!this->event_matches_interface_(gattc_if) || this->conn_id_ != param->search_res.conn_id)
      return false;
    if (!this->connection_gate_.open_ok() || !this->connection_gate_.auth_ok() ||
        !this->connection_gate_.discovery_started()) {
      this->fail_session_("SEARCH_RES arrived before the security gate");
      return true;
    }
  }

  if (event == ESP_GATTC_SEARCH_CMPL_EVT) {
    if (!this->event_matches_interface_(gattc_if) || this->conn_id_ != param->search_cmpl.conn_id)
      return false;
    if (!this->connection_gate_.open_ok() || !this->connection_gate_.auth_ok() ||
        !this->connection_gate_.discovery_started()) {
      this->fail_session_("SEARCH_CMPL arrived before the security gate");
      return true;
    }
    if (param->search_cmpl.status != ESP_GATT_OK) {
      this->fail_session_("GATT service discovery failed", param->search_cmpl.status);
      return true;
    }
  }

  const bool handled = esp32_ble_client::BLEClientBase::gattc_event_handler(event, gattc_if, param);
  if (!handled)
    return false;

  if (event == ESP_GATTC_SEARCH_CMPL_EVT) {
    if (!this->resolve_gatt_and_subscribe_())
      return true;
  } else if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT) {
    this->handle_register_for_notify_(param);
  } else if (event == ESP_GATTC_WRITE_DESCR_EVT) {
    this->handle_descriptor_write_(param);
  } else if (event == ESP_GATTC_WRITE_CHAR_EVT) {
    this->handle_characteristic_write_(param);
  } else if (event == ESP_GATTC_NOTIFY_EVT) {
    this->handle_notification_(param);
  } else if (event == ESP_GATTC_READ_CHAR_EVT) {
    this->handle_device_information_read_(param);
  }

  return true;
}

bool OmronBLEClient::handle_connect_event_(esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
  // Replaces the base handler rather than running after it. ESPHome's base
  // sends an MTU request here, "immediately as recommended by ESP-IDF", which
  // saves about 3 ms and costs this cuff the whole session: an ATT request in
  // the same millisecond the bonding starts is a GATT operation against a host
  // the device has not yet authenticated, and it answers by terminating the
  // link (0x13). Nothing may touch the link until security settles; the MTU
  // request is not dropped, only deferred to just before discovery.
  if (!this->event_matches_interface_(gattc_if) || !this->check_addr(param->connect.remote_bda))
    return false;

  // Everything downstream reads this: skipping it leaves discovery and every
  // later request pointed at connection 0.
  this->conn_id_ = param->connect.conn_id;
  ESP_LOGD(TAG, "[%s] ESP_GATTC_CONNECT_EVT", this->address_str());
  this->apply_connection_action_(this->connection_gate_.on_connect());
  return true;
}

bool OmronBLEClient::handle_open_event_(esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
  // Calling BLEClientBase for OPEN would immediately start service discovery.
  // Keep this branch deliberately close to the base handler, stopping
  // immediately before esp_ble_gattc_search_service().
  if (!this->event_matches_interface_(gattc_if) || !this->check_addr(param->open.remote_bda))
    return false;

  this->service_count_ = 0;
  ESP_LOGD(TAG, "[%s] ESP_GATTC_OPEN_EVT status=%d", this->address_str(), param->open.status);

  if (this->state() == esp32_ble_tracker::ClientState::IDLE) {
    ESP_LOGD(TAG, "[%s] Ignoring late OPEN in IDLE state", this->address_str());
    return true;
  }

  if (this->state() != esp32_ble_tracker::ClientState::CONNECTING) {
    ESP_LOGW(TAG, "[%s] OPEN arrived in unexpected state %s", this->address_str(),
             esp32_ble_tracker::client_state_to_string(this->state()));
  }

  if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN) {
    (void)this->connection_gate_.on_open(false);
    ESP_LOGW(TAG, "[%s] Connection open failed, status=%d", this->address_str(), param->open.status);
    this->publish_status_("BLE OPEN failed");
    this->finish_diagnostics_(false);
    this->set_idle_();
    this->reset_session_();
    if (this->requires_per_session_cleanup_())
      this->begin_bond_cleanup_("OPEN failure");
    return true;
  }

  if (this->want_disconnect_) {
    this->unconditional_disconnect();
    return true;
  }

  this->set_state(esp32_ble_tracker::ClientState::CONNECTED);
  this->entities_.publish_connection_entity_(true);
  const ConnectionAction action = this->connection_gate_.on_open(true);
  if (this->connection_gate_.auth_ok()) {
    ESP_LOGI(TAG, "[%s] OPEN: security gate already satisfied", this->address_str());
    this->diagnostics_.phase = SessionPhase::DISCOVERING;
  } else {
    ESP_LOGI(TAG, "[%s] OPEN: waiting for bonded AUTH_CMPL before discovery", this->address_str());
    this->diagnostics_.phase = SessionPhase::WAITING_FOR_AUTH;
  }
  this->apply_connection_action_(action);
  return true;
}

void OmronBLEClient::start_discovery_() {
  if (this->connection_gate_.discovery_started() || this->state() != esp32_ble_tracker::ClientState::CONNECTED)
    return;

  this->connection_gate_.mark_discovery_started();
  this->diagnostics_.phase = SessionPhase::DISCOVERING;
  this->discovery_started_at_ = millis();
  // Deferred out of CONNECT_EVT, where the base class puts it: this is the
  // first moment the cuff has authenticated us, so it is the first moment any
  // ATT traffic is safe. Records arrive in 48-byte reads that need the larger
  // MTU, so the request still has to happen, just not before bonding.
  const esp_err_t mtu_error = esp_ble_gattc_send_mtu_req(this->gattc_if_, this->conn_id_);
  if (mtu_error != ESP_OK)
    ESP_LOGW(TAG, "[%s] MTU request failed (code=%d); continuing at the default MTU", this->address_str(), mtu_error);
  ESP_LOGI(TAG, "[%s] Security gate passed: starting GATT service discovery", this->address_str());
  const esp_err_t error = esp_ble_gattc_search_service(this->gattc_if_, this->conn_id_, nullptr);
  if (error != ESP_OK)
    this->fail_session_("esp_ble_gattc_search_service failed", error);
}

bool OmronBLEClient::add_notify_target_(uint16_t characteristic_handle, bool required) {
  auto *descriptor = this->get_config_descriptor(characteristic_handle);
  if (descriptor == nullptr) {
    if (required)
      ESP_LOGE(TAG, "[%s] Required characteristic 0x%04x has no CCCD", this->address_str(), characteristic_handle);
    return false;
  }
  this->subscriptions_.add(characteristic_handle, descriptor->handle, required);
  return true;
}

// OmronSubscriptionHost. Everything below is what only this class can do: talk
// to ESP-IDF, and decide what an outcome means for the session.
uint32_t OmronBLEClient::subscription_now_ms() {
  return millis();
}

const char *OmronBLEClient::subscription_address() {
  return this->address_str();
}

bool OmronBLEClient::subscription_register(uint16_t characteristic_handle, int &error) {
  const esp_err_t result = esp_ble_gattc_register_for_notify(this->gattc_if_, this->remote_bda_, characteristic_handle);
  error = static_cast<int>(result);
  return result == ESP_OK;
}

void OmronBLEClient::subscription_ready() {
  this->notifications_ready_();
}

void OmronBLEClient::subscription_failed(const char *reason, int error) {
  this->fail_session_(reason, error);
}

void OmronBLEClient::subscription_dropped_optional(uint16_t characteristic_handle, const char *why, int error) {
  ESP_LOGW(TAG, "[%s] Giving up on the optional notification at handle 0x%04X: %s (code=%d)", this->address_str(),
           static_cast<unsigned>(characteristic_handle), why, error);
  // The only optional target is the standard measurement, and forgetting its
  // handle is what stops a later notification being matched against it.
  if (this->standard_bp_handle_ == characteristic_handle)
    this->standard_bp_handle_ = 0;
}

void OmronBLEClient::subscription_retrying(uint16_t characteristic_handle, uint8_t attempt, uint8_t of) {
  ESP_LOGW(TAG, "[%s] No CCCD write for handle 0x%04X after %u ms; asking again (attempt %u of %u)",
           this->address_str(), static_cast<unsigned>(characteristic_handle),
           static_cast<unsigned>(OmronSubscriptions::RETRY_INTERVAL_MS), static_cast<unsigned>(attempt),
           static_cast<unsigned>(of));
}

bool OmronBLEClient::subscription_needs_encryption(int status) {
  // Only the first time, and only if nothing else has already started security:
  // answering a second refusal with a second esp_ble_set_encryption is how a
  // link gets two SMP procedures racing each other.
  if (status != ESP_GATT_INSUF_AUTHENTICATION && status != ESP_GATT_INSUF_ENCRYPTION)
    return false;
  if (this->security_started_)
    return false;
  this->security_started_ = true;
  this->auth_started_at_ = millis();
  this->diagnostics_.phase = SessionPhase::WAITING_FOR_AUTH;
  ESP_LOGI(TAG, "[%s] Cuff refused the subscription until the link is encrypted (status=%d); starting security",
           this->address_str(), status);
  const esp_err_t error = esp_ble_set_encryption(this->remote_bda_, ESP_BLE_SEC_ENCRYPT_NO_MITM);
  if (error != ESP_OK) {
    this->fail_session_("esp_ble_set_encryption failed", error);
    // Already failed the session; the machine must not also park on it.
    return false;
  }
  return true;
}

bool OmronBLEClient::resolve_gatt_and_subscribe_() {
  // With a configured profile the layout comes from it. Detecting one, there is
  // no profile yet - but the channel UUIDs are a property of the stack, not of
  // the model, and the stack is readable from the service list that discovery
  // has already filled in. Subscribing is therefore possible before the model is
  // known, which is what makes detection fit inside the first session at all.
  const OmronGattCapabilities *layout = this->profile_ != nullptr ? this->profile_->gatt : nullptr;
  if (layout == nullptr && this->profile_auto_) {
    switch (this->discovered_stack_()) {
      case OmronStack::CLASSIC:
        layout = &OMRON_CLASSIC_GATT;
        break;
      case OmronStack::MODERN:
        layout = &OMRON_MODERN_GATT;
        break;
      case OmronStack::UNKNOWN:
        // No proprietary service at all. There is no memory to read and no model
        // to identify, but the standard service does not need either.
        //
        // Cleared here rather than trusting the clear further down, which this
        // branch returns before reaching.
        this->subscriptions_.clear();
        if (!this->subscribe_standard_measurement_(true) ||
            !this->begin_live_only_("This device exposes neither Omron GATT service")) {
          this->fail_session_("Profile detection found neither Omron GATT service");
          return false;
        }
        this->session_.set_rx_channel_count(0);
        this->subscription_started_at_ = millis();
        this->subscriptions_.begin(millis());
        return true;
    }
    ESP_LOGD(TAG, "[%s] Detecting a profile over the %s stack", this->address_str(),
             layout == &OMRON_CLASSIC_GATT ? "classic" : "modern");
  }
  if (layout == nullptr) {
    this->fail_session_("Profile has no GATT layout");
    return false;
  }

  const OmronGattCapabilities &gatt = *layout;
  if (gatt.rx_channel_count == 0 || gatt.rx_channel_count > this->rx_handles_.size() || gatt.tx_channel_count == 0 ||
      gatt.tx_channel_count > this->tx_handles_.size()) {
    this->fail_session_("Profile has an invalid GATT channel count");
    return false;
  }

  this->diagnostics_.phase = SessionPhase::SUBSCRIBING;
  const auto service_uuid = esp32_ble_tracker::ESPBTUUID::from_raw(std::string(gatt.parent_service_uuid));
  if (this->get_service(service_uuid) == nullptr) {
    this->fail_session_("Required Omron GATT service was not discovered");
    return false;
  }

  this->subscriptions_.clear();
  this->rx_handle_count_ = gatt.rx_channel_count;
  this->tx_handle_count_ = gatt.tx_channel_count;
  for (uint8_t channel = 0; channel < this->rx_handle_count_; channel++) {
    if (gatt.rx_channel_uuids[channel] == nullptr) {
      this->fail_session_("Profile has a null RX UUID");
      return false;
    }
    auto *characteristic = this->get_characteristic(
        service_uuid, esp32_ble_tracker::ESPBTUUID::from_raw(std::string(gatt.rx_channel_uuids[channel])));
    if (characteristic == nullptr ||
        (characteristic->properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) == 0) {
      this->fail_session_("Required Omron RX characteristic is missing or cannot notify");
      return false;
    }
    this->rx_handles_[channel] = characteristic->handle;
    if (!this->add_notify_target_(characteristic->handle, true)) {
      this->fail_session_("Required Omron RX CCCD is missing");
      return false;
    }
  }

  for (uint8_t channel = 0; channel < this->tx_handle_count_; channel++) {
    if (gatt.tx_channel_uuids[channel] == nullptr) {
      this->fail_session_("Profile has a null TX UUID");
      return false;
    }
    auto *characteristic = this->get_characteristic(
        service_uuid, esp32_ble_tracker::ESPBTUUID::from_raw(std::string(gatt.tx_channel_uuids[channel])));
    if (characteristic == nullptr ||
        (characteristic->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) == 0) {
      this->fail_session_("Required Omron TX characteristic is missing or not writable");
      return false;
    }
    this->tx_handles_[channel] = characteristic->handle;
    this->tx_properties_[channel] = characteristic->properties;
  }

  // Detecting a profile, resolve the unlock characteristic whenever the stack
  // declares one and let the detected profile decide later whether to use it.
  // The alternative would be discovering the model and only then finding out
  // that the handle it needs was never looked up.
  const bool wants_unlock = this->profile_ != nullptr ? requires_protocol_unlock(*this->profile_)
                                                      : gatt.unlock_characteristic_uuid != nullptr;
  if (wants_unlock) {
    if (gatt.unlock_characteristic_uuid == nullptr) {
      this->fail_session_("Profile requires unlock but has no unlock UUID");
      return false;
    }
    auto *characteristic = this->get_characteristic(
        service_uuid, esp32_ble_tracker::ESPBTUUID::from_raw(std::string(gatt.unlock_characteristic_uuid)));
    if (characteristic == nullptr ||
        (characteristic->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) == 0 ||
        (characteristic->properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) == 0) {
      this->fail_session_("Required Omron unlock characteristic is missing or has incompatible properties");
      return false;
    }
    this->unlock_handle_ = characteristic->handle;
    this->unlock_properties_ = characteristic->properties;
    if (!this->add_notify_target_(characteristic->handle, true)) {
      this->fail_session_("Required Omron unlock CCCD is missing");
      return false;
    }
  }

  // Only while the model is still unknown, which here means `profile: auto`
  // before the device information read has answered. A session with a profile
  // does not subscribe it at all.
  //
  // Nothing else that talks to these cuffs does either. One reference never
  // touches the standard service; the other reaches for it only after its own
  // memory read has come back with nothing, and calls that a diagnostic in its
  // own words. The cuff measured here has never sent a single notification on
  // it, so on a working profile this was a CCCD write and a subscription target
  // per session, bought with a hypothesis nothing has confirmed.
  //
  // It stays required where it is the whole of the session - see the branch
  // above for a device exposing neither Omron service, and begin_live_only_,
  // which refuses without a handle.
  if (this->profile_ == nullptr)
    this->subscribe_standard_measurement_(false);

  this->session_.set_rx_channel_count(this->rx_handle_count_);
  this->subscription_started_at_ = millis();
  this->subscriptions_.begin(millis());
  return true;
}

// The standard blood pressure measurement, 0x2A35. Required only when it is the
// single thing this session has: with a profile it is a bonus channel and
// losing it must not cost the transfer. Every outcome is logged, because
// whether the cuff offers the characteristic at all is the first thing anyone
// asks about a measurement that never arrives.
bool OmronBLEClient::subscribe_standard_measurement_(bool required) {
  auto *standard_bp = this->get_characteristic(0x1810, 0x2A35);
  if (standard_bp == nullptr) {
    ESP_LOGD(TAG, "[%s] No standard blood pressure measurement characteristic", this->address_str());
    return false;
  }
  if ((standard_bp->properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) == 0) {
    ESP_LOGD(TAG, "[%s] Standard blood pressure characteristic cannot notify", this->address_str());
    return false;
  }
  if (!this->add_notify_target_(standard_bp->handle, required)) {
    ESP_LOGD(TAG, "[%s] Standard blood pressure characteristic has no CCCD", this->address_str());
    return false;
  }
  this->standard_bp_handle_ = standard_bp->handle;
  ESP_LOGD(TAG, "[%s] Subscribing to the standard blood pressure measurement", this->address_str());
  return true;
}

// Model number, firmware revision, serial number, in that order. The list is
// short on purpose: manufacturer and the hardware and software revisions are
// three more round trips for strings nobody acts on.
// Service and characteristic, because the last of these is not device
// information at all: 0x2A49 is the standard Blood Pressure Feature, the one
// statement about detections a cuff makes without being identified first.
struct DeviceInformationRead {
  uint16_t service;
  uint16_t characteristic;
};
static constexpr DeviceInformationRead DEVICE_INFORMATION_CHARACTERISTICS[] = {
    {0x180A, 0x2A24}, {0x180A, 0x2A26}, {0x180A, 0x2A25}, {0x1810, 0x2A49}};

void OmronBLEClient::start_device_information_() {
  if (this->device_information_done_)
    return;
  this->device_information_index_ = 0;
  this->request_next_device_information_();
}

void OmronBLEClient::request_next_device_information_() {
  while (this->device_information_index_ < std::size(DEVICE_INFORMATION_CHARACTERISTICS)) {
    const DeviceInformationRead &target = DEVICE_INFORMATION_CHARACTERISTICS[this->device_information_index_];
    const uint16_t uuid = target.characteristic;
    auto *characteristic = this->get_characteristic(target.service, uuid);
    if (characteristic == nullptr) {
      ESP_LOGD(TAG, "[%s] Device information 0x%04X absent", this->address_str(), uuid);
      this->device_information_index_++;
      continue;
    }
    this->device_information_handle_ = characteristic->handle;
    const esp_err_t error =
        esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, characteristic->handle, ESP_GATT_AUTH_REQ_NONE);
    if (error == ESP_OK)
      return;
    ESP_LOGD(TAG, "[%s] Device information 0x%04X could not be read (code=%d)", this->address_str(), uuid, error);
    this->device_information_index_++;
  }
  this->device_information_handle_ = 0;
  this->device_information_done_ = true;
}

void OmronBLEClient::handle_device_information_read_(esp_ble_gattc_cb_param_t *param) {
  if (this->device_information_handle_ == 0 || param->read.handle != this->device_information_handle_)
    return;

  if (param->read.status == ESP_GATT_OK && param->read.value_len != 0) {
    // Fixed-length strings come padded, and a trailing NUL or space would go
    // straight into the entity.
    std::string value(reinterpret_cast<const char *>(param->read.value), param->read.value_len);
    while (!value.empty() && (value.back() == '\0' || value.back() == ' '))
      value.pop_back();
    switch (DEVICE_INFORMATION_CHARACTERISTICS[this->device_information_index_].characteristic) {
      case 0x2A49:
        this->apply_standard_features_({param->read.value, param->read.value_len});
        break;
      case 0x2A24:
        this->entities_.publish_model_number_entity_(value);
        ESP_LOGI(TAG, "[%s] Cuff reports model %s", this->address_str(), value.c_str());
        this->check_reported_model_(value);
        break;
      case 0x2A26:
        this->entities_.publish_firmware_revision_entity_(value);
        ESP_LOGI(TAG, "[%s] Cuff reports firmware %s", this->address_str(), value.c_str());
        break;
      case 0x2A25:
        // Published but never logged: it identifies this particular device and
        // logs get pasted into issues and chat windows.
        this->entities_.publish_serial_number_entity_(value);
        ESP_LOGI(TAG, "[%s] Serial number read and published", this->address_str());
        break;
      default:
        break;
    }
  }
  this->device_information_index_++;
  this->request_next_device_information_();
}

// Which of the two proprietary stacks this cuff actually exposes, asked of the
// discovered service list rather than of the configured profile. Both UUIDs are
// looked up because the question is what the device has, not whether it has
// what we expected: a profile pointing at the wrong stack is precisely the case
// worth reporting.
OmronStack OmronBLEClient::discovered_stack_() {
  const bool classic =
      this->get_service(esp32_ble_tracker::ESPBTUUID::from_raw(std::string(OMRON_CLASSIC_GATT.parent_service_uuid))) !=
      nullptr;
  const bool modern =
      this->get_service(esp32_ble_tracker::ESPBTUUID::from_raw(std::string(OMRON_MODERN_GATT.parent_service_uuid))) !=
      nullptr;
  // Never seen on any cuff here, but a device exposing both would make the
  // stack useless as a discriminator rather than making it wrong.
  if (classic == modern)
    return OmronStack::UNKNOWN;
  return classic ? OmronStack::CLASSIC : OmronStack::MODERN;
}

void OmronBLEClient::apply_standard_features_(std::span<const uint8_t> data) {
  if (!parse_standard_bp_features(data, this->standard_features_)) {
    ESP_LOGD(TAG, "[%s] Blood Pressure Feature is %u byte(s); expected 2", this->address_str(),
             static_cast<unsigned>(data.size()));
    return;
  }
  this->standard_features_known_ = true;
  ESP_LOGI(TAG, "[%s] Cuff features 0x%04X: movement=%d cuff=%d irregular=%d rate range=%d position=%d bonds=%d",
           this->address_str(), static_cast<unsigned>(this->standard_features_.raw),
           this->standard_features_.body_movement ? 1 : 0, this->standard_features_.cuff_fit ? 1 : 0,
           this->standard_features_.irregular_pulse ? 1 : 0, this->standard_features_.pulse_rate_range ? 1 : 0,
           this->standard_features_.measurement_position ? 1 : 0, this->standard_features_.multiple_bond ? 1 : 0);
  // Only the live-only path derives entities from this. With a profile the
  // catalog is the better source, and overriding it here would replace a
  // per-model statement with a generic one.
  if (this->live_only_)
    this->apply_live_only_capabilities_();
}

// Entities for a cuff whose model is unknown. The rule - claim a detection flag
// only where 0x2A49 says the cuff performs it - lives in omron_publish, which
// takes a null features pointer to mean the cuff never answered that read.
void OmronBLEClient::apply_live_only_capabilities_() {
  this->entities_.set_entity_capabilities(
      capabilities_for_live_only(this->standard_features_known_ ? &this->standard_features_ : nullptr));
}

// Reading nothing out of memory, and publishing whatever the standard
// characteristic indicates while the link is up. This is what a cuff gets when
// its model cannot be resolved to a memory map: no history, no settings, no
// clock, but a live measurement is still a measurement.
//
// Unproven on the hardware here: 0x2A35 has never notified on the one cuff this
// component was written against, which may be because it indicates during a
// measurement while this node connects afterwards. Built because a cuff that
// cannot be mapped otherwise gets nothing at all, and logged loudly enough that
// the next person can tell which of those two happened.
bool OmronBLEClient::begin_live_only_(const char *why) {
  if (this->standard_bp_handle_ == 0) {
    ESP_LOGE(TAG, "[%s] %s, and this cuff does not offer the standard measurement characteristic either",
             this->address_str(), why);
    return false;
  }
  this->live_only_ = true;
  // The window is armed where the settle window closes, not here: this can run
  // before the subscription has even been requested, and a deadline started then
  // would spend itself waiting for its own CCCD write.
  this->apply_live_only_capabilities_();
  ESP_LOGW(TAG,
           "[%s] %s. Falling back to the standard blood pressure service for %u s: live measurements only, "
           "no history and no settings.",
           this->address_str(), why, static_cast<unsigned>(LIVE_ONLY_WINDOW_MS / 1000));
  this->entities_.publish_profile_entity_("live only");
  return true;
}

// The one check standing between a mistyped profile and a plausible-looking
// reading. Nothing downstream can catch it: wrong addresses do not fail, they
// return other bytes, and blood pressure decoded out of the wrong region still
// lands in the range a person would believe.
//
// This reports and does not act. The trade name table rests on a single measured
// DIS string, and a component that refused to read on the strength of that would
// be betting a working node on one data point.
void OmronBLEClient::check_reported_model_(const std::string &reported) {
  const OmronStack stack = this->discovered_stack_();
  const ModelIdentification identification = identify_model(reported, stack);

  // Adoption happens here because the model is the first of the three reads, so
  // the profile lands well before the settle window closes, and because a
  // failure has to name the string that failed.
  if (this->profile_auto_ && this->profile_ == nullptr) {
    if (identification.profile == nullptr) {
      ESP_LOGE(TAG,
               "[%s] Cannot identify this cuff from \"%s\" (%s, %u candidate(s), %u known). Reading it needs a memory "
               "map and guessing one produces plausible numbers from the wrong region, so set `profile:` explicitly.",
               this->address_str(), reported.c_str(), model_identity_to_string(identification.identity),
               static_cast<unsigned>(identification.candidates),
               static_cast<unsigned>(identification.known_candidates));
      return;
    }
    this->set_profile(identification.profile->id);
    if (!this->apply_profile_()) {
      this->profile_ = nullptr;
      return;
    }
    this->model_verdict_ = ProfileVerdict::CONFIRMED;
    // At WARN, and saying what to type. Detection is mostly run to find out what
    // to configure, so the answer has to survive a log at default level and
    // arrive as the exact key the schema accepts - not as a model id the reader
    // then has to transliterate.
    ESP_LOGW(TAG, "[%s] Detected %s from \"%s\" (%s), %s. Pin it with `profile: %s` in the yaml.", this->address_str(),
             identification.profile->model, reported.c_str(), model_identity_to_string(identification.identity),
             profile_confidence_to_string(identification.profile->confidence),
             profile_config_key(*identification.profile).c_str());
    return;
  }

  if (this->profile_ == nullptr)
    return;

  const ProfileVerdict verdict = verify_configured_profile(*this->profile_, identification);
  this->model_verdict_ = verdict;

  switch (verdict) {
    case ProfileVerdict::MISMATCH:
      ESP_LOGE(TAG,
               "[%s] Configured profile %s does not match this cuff. It reports \"%s\", which this catalog reads as "
               "%s: settings at 0x%04X and records at 0x%04X, against the configured 0x%04X and 0x%04X. Readings "
               "decoded from the wrong region still look like blood pressure, so check the profile before trusting "
               "them.",
               this->address_str(), this->profile_->model, reported.c_str(), identification.profile->model,
               static_cast<unsigned>(identification.profile->settings_read_address),
               static_cast<unsigned>(identification.profile->users[0].record_start_address),
               static_cast<unsigned>(this->profile_->settings_read_address),
               static_cast<unsigned>(this->profile_->users[0].record_start_address));
      break;
    case ProfileVerdict::COMPATIBLE:
      ESP_LOGI(TAG, "[%s] Cuff identified as %s; configured profile %s addresses the same memory", this->address_str(),
               identification.profile->model, this->profile_->model);
      break;
    case ProfileVerdict::CONFIRMED:
      ESP_LOGI(TAG, "[%s] Cuff identified as %s, matching the configured profile", this->address_str(),
               identification.profile->model);
      break;
    case ProfileVerdict::UNVERIFIED:
      // Not a complaint about the configuration. Most of the catalog has never
      // been on a link here, and a cuff whose reported string means nothing to
      // this table is the expected case rather than a problem.
      ESP_LOGD(TAG, "[%s] Reported model \"%s\" is %s here (%u candidate(s), %u known); profile %s not verified",
               this->address_str(), reported.c_str(), model_identity_to_string(identification.identity),
               static_cast<unsigned>(identification.candidates), static_cast<unsigned>(identification.known_candidates),
               this->profile_->model);
      break;
  }
}

// Translation only: which event this was, and whether it succeeded. What it
// means for the queue is OmronSubscriptions'.
void OmronBLEClient::handle_register_for_notify_(esp_ble_gattc_cb_param_t *param) {
  this->subscriptions_.on_register_result(param->reg_for_notify.handle, param->reg_for_notify.status == ESP_GATT_OK,
                                          static_cast<int>(param->reg_for_notify.status));
}

void OmronBLEClient::handle_descriptor_write_(esp_ble_gattc_cb_param_t *param) {
  this->subscriptions_.on_descriptor_written(param->write.handle, param->write.status == ESP_GATT_OK,
                                             static_cast<int>(param->write.status));
}

void OmronBLEClient::notifications_ready_() {
  this->notifications_configured_ = true;
  this->notifications_ready_at_ = millis();
  this->transaction_start_pending_ = true;
  ESP_LOGI(TAG, "[%s] Required RX/unlock subscriptions configured; waiting %u ms before the transfer",
           this->address_str(), static_cast<unsigned>(NOTIFY_SETTLE_MS));
  // The negotiation was fired at connect and its result was never read. It is
  // read here, once, at the last moment before the first frame goes out.
  //
  // A cuff that answers the request with less than its own block needs cannot
  // deliver a full read reply in one notification, and that failure arrives as
  // a payload length mismatch or as silence - neither of which names the MTU.
  // Almost every profile in the catalog is unproven on a link, and the block
  // size is one of the few link properties one can state wrongly on its own.
  // Warned, not failed: the cuff may still answer short reads, and losing the
  // session outright would be a worse answer than a line saying why.

  // Block bytes, the frame's own eight around them, and the three ATT bytes a
  // notification spends before any of it. 56 + 8 + 3 = 67, which is the MTU a
  // cuff on the modern stack negotiates.
  static constexpr uint16_t ATT_NOTIFY_HEADER = 3;
  const uint16_t needed =
      static_cast<uint16_t>(this->poll_layout_.transfer_block_size + READ_RESPONSE_OVERHEAD + ATT_NOTIFY_HEADER);
  if (this->profile_ == nullptr) {
    // No block size to check against yet. Skipped rather than deferred: the
    // check is a warning about a link property that will not change between here
    // and the first frame, and the detected profile logs its own arrival.
    ESP_LOGD(TAG, "[%s] MTU %u; transfer block unknown until the profile is detected", this->address_str(),
             static_cast<unsigned>(this->mtu_));
  } else if (this->mtu_ < needed) {
    ESP_LOGW(TAG, "[%s] Negotiated MTU is %u; a %u-byte transfer block needs %u. Reads may come back short.",
             this->address_str(), static_cast<unsigned>(this->mtu_),
             static_cast<unsigned>(this->poll_layout_.transfer_block_size), static_cast<unsigned>(needed));
  } else {
    ESP_LOGD(TAG, "[%s] Negotiated MTU %u covers a %u-byte transfer block", this->address_str(),
             static_cast<unsigned>(this->mtu_), static_cast<unsigned>(this->poll_layout_.transfer_block_size));
  }
  // Into the settle window rather than ahead of it: the transfer waits on a
  // timer here anyway, so three reads on an unrelated service cost nothing they
  // were not already spending, and they happen once per boot.
  this->start_device_information_();
}

// --- OmronSessionHost ---
//
// The whole of what the session layer is allowed to ask of the link. Anything
// not here, the session does not get to know about: no handles, no write types,
// no bonding state.

uint32_t OmronBLEClient::session_now_ms() {
  return millis();
}

bool OmronBLEClient::session_wall_clock(OmronDateTime &now) {
#ifdef USE_TIME
  if (this->time_ == nullptr)
    return false;
  const ESPTime current = this->time_->now();
  if (!current.is_valid())
    return false;
  now = local_now_(current);
  return true;
#else
  (void)now;
  return false;
#endif
}

bool OmronBLEClient::session_random_nonce(std::span<uint8_t> data) {
  return random_bytes(data.data(), data.size());
}

bool OmronBLEClient::session_request_link_encryption() {
  // Once per connection, counting the one the connect path may already have
  // started. Two guards make up the single `!security_started_` this replaced:
  // the session's own, and this one, which knows about bonding.
  if (this->security_started_)
    return true;
  this->security_started_ = true;
  this->auth_started_at_ = millis();
  const esp_err_t error = esp_ble_set_encryption(this->remote_bda_, ESP_BLE_SEC_ENCRYPT_NO_MITM);
  if (error == ESP_OK)
    return true;
  ESP_LOGE(TAG, "[%s] esp_ble_set_encryption failed (code=%d)", this->address_str(), error);
  return false;
}

void OmronBLEClient::session_settings_read(const std::vector<uint8_t> &settings) {
  this->report_user_settings_(settings);
}

void OmronBLEClient::session_transfer_complete() {
  this->finalize_record_transaction_();
}

void OmronBLEClient::session_failed(const char *reason, int code) {
  this->fail_session_(reason, code);
}

WriteDispatch OmronBLEClient::session_write(SessionChannel channel, std::span<const uint8_t> data,
                                            bool prefer_no_response) {
  this->outgoing_channel_ = channel;
  this->outgoing_prefer_no_response_ = prefer_no_response;
  // The unlock characteristic is a channel of its own, so a command on it is
  // never split whatever the profile's TX count says.
  this->writer_.begin(data, this->tx_handle_count_, channel == SessionChannel::UNLOCK);
  return this->dispatch_next_write_();
}

WriteDispatch OmronBLEClient::dispatch_next_write_() {
  const bool unlock_channel = this->outgoing_channel_ == SessionChannel::UNLOCK;
  WriteFragment fragment{};
  if (!this->writer_.next(fragment)) {
    ESP_LOGE(TAG, "[%s] Command exceeds the configured TX channel count", this->address_str());
    return WriteDispatch::FAILED;
  }
  const uint16_t handle = unlock_channel ? this->unlock_handle_ : this->tx_handles_[fragment.channel];
  const esp_gatt_char_prop_t properties =
      unlock_channel ? this->unlock_properties_ : this->tx_properties_[fragment.channel];
  const size_t write_length = fragment.length;
  if (handle == 0) {
    ESP_LOGE(TAG, "[%s] No handle for the selected channel", this->address_str());
    return WriteDispatch::FAILED;
  }

  const bool prefer_no_response =
      this->outgoing_prefer_no_response_ || (!unlock_channel && this->tx_handle_count_ == 1);
  esp_gatt_write_type_t write_type;
  if (prefer_no_response && (properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
    write_type = ESP_GATT_WRITE_TYPE_NO_RSP;
  } else if ((properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0) {
    write_type = ESP_GATT_WRITE_TYPE_RSP;
  } else if ((properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
    write_type = ESP_GATT_WRITE_TYPE_NO_RSP;
  } else {
    ESP_LOGE(TAG, "[%s] Selected GATT characteristic is not writable", this->address_str());
    return WriteDispatch::FAILED;
  }

  // Deliberately never dumps the unlock characteristic. On classic profiles that
  // write carries the 16-byte bind key, and a debug log is not a place for it.
  //
  // VERBOSE, not DEBUG, for the same reason BLEClientBase keeps its own GATT
  // dumps there: writing to the UART inside a session delays time-sensitive BLE
  // work, and a burst of these fills the API send buffer, which drops log lines
  // rather than blocking. An event that went out then looks like one that did
  // not, because only the line announcing it was lost.
  //   logger: {logs: {omron.ble: VERBOSE}}
  // brings the frames back without turning the rest of the node verbose.
  if (!unlock_channel) {
    ESP_LOGV(TAG, "[%s] WRITE handle=0x%04X len=%u: %s", this->address_str(), handle,
             static_cast<unsigned>(write_length),
             format_hex_pretty(this->writer_.data() + fragment.offset, write_length).c_str());
  } else {
    ESP_LOGV(TAG, "[%s] WRITE handle=0x%04X len=%u: <unlock, not logged>", this->address_str(), handle,
             static_cast<unsigned>(write_length));
  }

  this->write_handle_ = handle;
  const esp_err_t error =
      esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, handle, static_cast<uint16_t>(write_length),
                               this->writer_.data() + fragment.offset, write_type, ESP_GATT_AUTH_REQ_NONE);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "[%s] GATT command write failed (code=%d)", this->address_str(), error);
    return WriteDispatch::FAILED;
  }
  this->writer_.advance(fragment);
  if (write_type == ESP_GATT_WRITE_TYPE_RSP) {
    this->write_in_flight_ = true;
    this->write_started_at_ = millis();
    return WriteDispatch::AWAITING_RESPONSE;
  }
  if (!this->writer_.complete())
    return this->dispatch_next_write_();
  return WriteDispatch::ON_THE_WIRE;
}

// What the cuff said about a write, reduced to the three cases the session
// distinguishes. Keeping the mapping here is what keeps esp_gatt_status_t out
// of the session layer.
static WriteOutcome write_outcome_for_status(esp_gatt_status_t status) {
  if (status == ESP_GATT_OK)
    return WriteOutcome::OK;
  if (status == ESP_GATT_INSUF_AUTHENTICATION || status == ESP_GATT_INSUF_ENCRYPTION)
    return WriteOutcome::NEEDS_ENCRYPTION;
  return WriteOutcome::FAILED;
}

void OmronBLEClient::handle_characteristic_write_(esp_ble_gattc_cb_param_t *param) {
  if (!this->write_in_flight_ || param->write.handle != this->write_handle_)
    return;
  this->write_in_flight_ = false;
  const WriteOutcome outcome = write_outcome_for_status(param->write.status);
  if (outcome != WriteOutcome::OK) {
    this->session_.on_write_response(this->outgoing_channel_, outcome, param->write.status);
    return;
  }
  // More fragments of the same command still to go. The session hears nothing
  // until the whole of it is on the wire.
  if (!this->writer_.complete()) {
    const WriteDispatch dispatch = this->dispatch_next_write_();
    if (dispatch == WriteDispatch::AWAITING_RESPONSE)
      return;
    this->session_.on_write_response(this->outgoing_channel_,
                                     dispatch == WriteDispatch::FAILED ? WriteOutcome::FAILED : WriteOutcome::OK, 0);
    return;
  }
  this->session_.on_write_response(this->outgoing_channel_, WriteOutcome::OK, param->write.status);
}

int8_t OmronBLEClient::rx_channel_for_handle_(uint16_t handle) const {
  for (uint8_t channel = 0; channel < this->rx_handle_count_; channel++) {
    if (this->rx_handles_[channel] == handle)
      return static_cast<int8_t>(channel);
  }
  return -1;
}

void OmronBLEClient::handle_notification_(esp_ble_gattc_cb_param_t *param) {
  if (this->session_failure_handled_)
    return;
  const uint16_t handle = param->notify.handle;
  // Every frame this cuff has ever sent, verbatim. Nothing else can tell a
  // wrong memory map apart from a mis-assembled one, and both surface as the
  // same parse error. At VERBOSE for the reason given at the matching WRITE
  // dump: these arrive in bursts, and a burst is what costs the API send buffer
  // the log lines around it.
  ESP_LOGV(TAG, "[%s] NOTIFY handle=0x%04X len=%u: %s", this->address_str(), handle,
           static_cast<unsigned>(param->notify.value_len),
           format_hex_pretty(param->notify.value, param->notify.value_len).c_str());
  // The standard characteristic is not part of the memory session at all: it is
  // a live reading on a service the cuff shares with every other blood-pressure
  // monitor, so it never reaches the session layer.
  if (handle == this->standard_bp_handle_) {
    this->publish_standard_measurement_({param->notify.value, param->notify.value_len});
    return;
  }
  const std::span<const uint8_t> payload{param->notify.value, param->notify.value_len};
  if (handle == this->unlock_handle_) {
    this->session_.on_unlock_notification(payload);
    return;
  }
  const int8_t channel = this->rx_channel_for_handle_(handle);
  if (channel >= 0)
    this->session_.on_protocol_notification(static_cast<uint8_t>(channel), payload);
}

void OmronBLEClient::report_user_settings_(const std::vector<uint8_t> &settings) {
  if (this->profile_ == nullptr)
    return;
  // Logged per user because this is the readback that proves registration took:
  // the version counter moves when a block write lands, and a bond that dies is
  // usually a counter that did not move.
  //
  // The per-user bit is logged raw and called nothing in particular. It reads
  // zero even for a user that has registered, so a label claiming otherwise
  // would have every session reporting a registration that did happen as one
  // that did not. What actually proves it is the version counter below.
  //
  // Nothing here is gated on that bit, however convenient it looks as a guard.
  // The models with a six-byte user block have no flag and no counter, only a
  // birth date - and refusing them here left them with a birth date entity that
  // was granted, published by nobody, and unknown for the life of the node.
  const bool has_counter = user_block_carries_version(*this->profile_);
  for (uint8_t user = 1; user <= this->profile_->user_count; user++) {
    OmronSettingsBlock block{};
    if (!user_settings_block(*this->profile_, user, block))
      continue;
    bool user_flag = false;
    const bool flag_known = user_registered_flag(*this->profile_, user, settings, user_flag);
    // The birth date is spelled out because it is the one field whose value we
    // choose ourselves, so it reads back as proof rather than as an echo.
    ESP_LOGI(TAG, "[%s] User %u settings read 0x%04X write 0x%04X: flag=%s born %04u-%02u-%02u raw %s",
             this->address_str(), static_cast<unsigned>(user),
             static_cast<unsigned>(this->profile_->settings_read_address + block.offset),
             static_cast<unsigned>(this->profile_->settings_write_address + block.offset),
             flag_known ? (user_flag ? "1" : "0") : "-", static_cast<unsigned>(1900 + settings[block.offset]),
             static_cast<unsigned>(settings[block.offset + 1]), static_cast<unsigned>(settings[block.offset + 2]),
             format_hex_pretty(settings.data() + block.offset, block.size).c_str());
    // The same numbers into Home Assistant, so the evidence that a registration
    // write survived is not a log line somebody had to be watching at the time.
    OmronUserSettingsData published{};
    if (!settings_entity_for_user(*this->profile_, user, settings, published))
      continue;
    // Stepping once per pairing is correct; a counter climbing on every poll
    // means the registration gate broke and every read is spending a write to
    // the device.
    if (has_counter) {
      ESP_LOGI(TAG, "[%s] User %u version counter: %u", this->address_str(), static_cast<unsigned>(user),
               static_cast<unsigned>(published.version));
    }
    this->entities_.publish_user_settings_(static_cast<uint8_t>(user - 1), published);
  }
}

void OmronBLEClient::configure_session_() {
  // Said again every session, on purpose. The identification runs off the DIS
  // read, which happens once per boot, so a node that has been up for a month
  // has that line a month back in a log nobody kept. The condition it describes
  // applies to every reading published since.
  if (this->model_verdict_ == ProfileVerdict::MISMATCH && this->profile_ != nullptr) {
    ESP_LOGW(TAG, "[%s] Reading with profile %s, which this cuff's own model string contradicts", this->address_str(),
             this->profile_->model);
  }
  OmronSessionConfig config;
  config.profile = this->profile_;
  config.layout = this->poll_layout_;
  config.bind_key = this->bind_key_;
  config.bind_key_set = this->bind_key_set_;
  config.end_session = this->end_session_;
  config.register_as_user = this->register_as_user_;
  config.write_birth_date_users = this->write_birth_date_users_;
  config.full_read_on_pairing = this->full_read_on_pairing_;
  config.clock_sync_threshold_set = this->clock_sync_threshold_set_;
  config.clock_sync_threshold_s = this->clock_sync_threshold_s_;
  for (size_t user = 0; user < OMRON_MAX_USERS; user++) {
    const BirthDate &configured = this->birth_dates_[user];
    config.birth_dates[user] = OmronDateTime{configured.year, configured.month, configured.day, 0, 0, 0};
  }
  this->session_.configure(config);
  // configure() resets the session, and the reset clears paired_this_session_.
  // With a detected profile that runs in the middle of the session that just
  // bonded, and the flag it clears is the one deciding whether the user block
  // is written - so the client owns the fact and restates it after every
  // configure rather than the session remembering it across a reset.
  //
  // Not cosmetic: a host that bonds and never registers at the application
  // level has its bond deleted by the cuff, and every later session then fails
  // encryption.
  this->session_.set_paired_this_session(this->paired_this_session_);
  this->session_.set_pairing_advertised(this->advertised_pairing_mode_);
}

std::string OmronBLEClient::format_local_datetime_(const OmronDateTime &value) const {
  const std::string naive = format_datetime(value);
#ifdef USE_TIME
  // The offset in force on the record's own date, not today's. Taking today's
  // would stamp a July reading with the January offset every winter, and the
  // whole point of a zone-aware value is that Home Assistant then trusts it.
  // recalc_timestamp_local resolves the ambiguous and skipped hours the same
  // way libc does, so this is not a reimplementation of that arithmetic.
  if (this->time_ != nullptr) {
    ESPTime local{};
    local.year = value.year;
    local.month = value.month;
    local.day_of_month = value.day;
    local.hour = value.hour;
    local.minute = value.minute;
    local.second = value.second;
    local.recalc_timestamp_local();
    if (local.timestamp > 0) {
      const int64_t offset = civil_seconds(value) - static_cast<int64_t>(local.timestamp);
      // A whole-day offset is not a timezone; it means the conversion went
      // wrong and a naive value beats a confidently wrong one.
      if (offset > -86400 && offset < 86400) {
        const int64_t minutes = offset / 60;
        const int64_t magnitude = minutes < 0 ? -minutes : minutes;
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%c%02u:%02u", minutes < 0 ? '-' : '+',
                      static_cast<unsigned>(magnitude / 60), static_cast<unsigned>(magnitude % 60));
        return naive + suffix;
      }
    }
  }
#endif
  return naive;
}

HarvestRequest OmronBLEClient::build_harvest_request_() {
  HarvestRequest request;
  request.profile = this->profile_;
  request.layout = &this->poll_layout_;
  request.memory = &this->session_.record_memory();
  request.plans = &this->session_.record_plans();
  request.cutoff_set = this->ignore_before_set_;
  request.cutoff_epoch = this->ignore_before_epoch_;
  request.history_records = this->history_records_;
  for (size_t user = 0; user < OMRON_MAX_USERS && user < OMRON_ENTITY_USER_SLOTS; user++)
    request.watermark[user] = this->history_.watermark(static_cast<uint8_t>(user));
  OmronDateTime now{};
  if (this->session_wall_clock(now)) {
    request.now_known = true;
    request.now_epoch = civil_seconds(now);
  }
  // What is left of the queue, so a ring arriving while Home Assistant is away
  // cannot grow it without bound.
  request.history_budget = this->history_.room();
  return request;
}

void OmronBLEClient::finalize_record_transaction_() {
  // Which reading wins is omron_harvest's; what is left here is turning that
  // answer into entities, events and a status line.
  const HarvestResult harvest = harvest_records(this->build_harvest_request_());

  // In order: what the ring gave up and what it owes as events, then the cuff's
  // clock, then the entity values.
  for (uint8_t user_index = 0; user_index < OMRON_ENTITY_USER_SLOTS && user_index < OMRON_MAX_USERS; user_index++) {
    const HarvestedUser &user = harvest[user_index];

    // One line a session, not one a record: at INFO, where a per-slot debug
    // line would not be seen at all.
    if (user.dropped_before_cutoff != 0) {
      ESP_LOGI(TAG, "[%s] User %u: %u record(s) dropped as older than the configured cut-off, %u kept",
               this->address_str(), static_cast<unsigned>(user_index + 1),
               static_cast<unsigned>(user.dropped_before_cutoff), static_cast<unsigned>(user.kept));
    }
    if (user.dropped_in_future != 0) {
      ESP_LOGW(TAG, "[%s] User %u: %u record(s) stamped in the future; not reported", this->address_str(),
               static_cast<unsigned>(user_index + 1), static_cast<unsigned>(user.dropped_in_future));
    }
    // A ring that hands over bytes and decodes none of them is what a profile
    // aimed at the wrong region looks like, and otherwise leaves the same trace
    // as an empty ring. At WARN: for anyone bringing up a profile nobody has
    // run against their hardware, this line is the whole diagnosis.
    if (user.unparsed != 0) {
      ESP_LOGW(TAG, "[%s] User %u: %u slot(s) read but not decoded (first reason: %s); %u decoded", this->address_str(),
               static_cast<unsigned>(user_index + 1), static_cast<unsigned>(user.unparsed),
               measurement_parse_error_to_string(user.first_parse_error), static_cast<unsigned>(user.parsed));
    }
    // The plan named a slot the transfer never delivered. Not a decoding
    // question at all - either the address fell outside the ring or the read
    // came back short, and both are faults on our side of the link.
    if (user.unreadable != 0) {
      ESP_LOGW(TAG, "[%s] User %u: %u planned slot(s) never came back from the cuff", this->address_str(),
               static_cast<unsigned>(user_index + 1), static_cast<unsigned>(user.unreadable));
    }
    if (user.history_truncated) {
      ESP_LOGW(TAG, "[%s] History queue is full at %u; dropped the rest of user %u's ring", this->address_str(),
               static_cast<unsigned>(OmronHistoryQueue::CAPACITY), static_cast<unsigned>(user_index + 1));
    }

    for (const HarvestedRecord &record : user.history) {
      HistoryEvent event;
      event.measurement = record.measurement;
      event.user_index = user_index;
      event.slot = record.slot;
      this->history_.push(event);
    }
    if (user.watermark_advanced) {
      // Held in RAM until the queue drains. Writing it per event would put two
      // rings' worth of flash writes behind one poll, and recording it before
      // the events have gone would mark them sent while they were still queued.
      this->history_.note_watermark(user_index, user.watermark);
      this->enable_loop();
      ESP_LOGD(TAG, "[%s] User %u queued %u new record(s) of %u read", this->address_str(),
               static_cast<unsigned>(user_index + 1), static_cast<unsigned>(user.history.size()),
               static_cast<unsigned>(user.kept));
    }
  }

  this->publish_device_clock_();

  uint8_t users_decoded = 0;
  uint8_t users_published = 0;
  for (uint8_t user_index = 0; user_index < OMRON_ENTITY_USER_SLOTS && user_index < OMRON_MAX_USERS; user_index++) {
    const HarvestedUser &user = harvest[user_index];
    if (!user.valid)
      continue;
    users_decoded++;
    // Deduplication is per user as well: one person measuring twice a day must
    // not suppress the other person's unchanged latest reading, and vice versa.
    if (!this->record_store_.accept(user_index, user.newest.fingerprint))
      continue;
    this->publish_selected_measurement_(user_index, user.newest);
    users_published++;
  }

  // Which of the four ways a transfer can come back empty this was is decided in
  // omron_publish, where the distinction is held by a test rather than by
  // whoever edits this branch next.
  this->finish_poll_(
      poll_outcome_status(harvest, users_decoded, users_published, !this->session_.record_plans().empty()));
}

uint32_t OmronBLEClient::history_pref_hash_(uint8_t user_index) const {
  // Folded from the MAC so two cuffs on one node never share a slot, and mixed
  // with the user so two people on one cuff never do either.
  uint32_t hash = 2166136261UL;
  for (uint8_t shift = 0; shift < 48; shift += 8)
    hash = (hash ^ static_cast<uint8_t>(this->address_ >> shift)) * 16777619UL;
  hash = (hash ^ user_index) * 16777619UL;
  return hash ^ 0x4F4D524EUL;
}

bool OmronBLEClient::all_zero_(std::span<const uint8_t> data) {
  return std::ranges::all_of(data, [](uint8_t value) { return value == 0; });
}

// OmronHistoryQueueHost. The pacing, the ordering and the rule that the flash
// mark may only be written once the queue is empty all live in
// OmronHistoryQueue, where a host test can drive them.
uint32_t OmronBLEClient::history_now_ms() {
  return millis();
}

void OmronBLEClient::history_save_watermark(uint8_t user_index, int64_t epoch) {
  if (user_index >= OMRON_ENTITY_USER_SLOTS)
    return;
  this->history_epoch_[user_index] = epoch;
  this->history_pref_[user_index].save(&this->history_epoch_[user_index]);
}

void OmronBLEClient::history_emit(const HistoryEvent &pending) {
  const OmronMeasurement &measurement = pending.measurement;
  ESP_LOGI(TAG, "[%s] User %u history slot %u: %s %u/%u mmHg %u bpm", this->address_str(),
           static_cast<unsigned>(pending.user_index + 1), static_cast<unsigned>(pending.slot),
           format_datetime(measurement.timestamp).c_str(), static_cast<unsigned>(measurement.systolic_mm_hg),
           static_cast<unsigned>(measurement.diastolic_mm_hg), static_cast<unsigned>(measurement.pulse_bpm));
#if defined(USE_API) && defined(USE_API_HOMEASSISTANT_SERVICES)
  std::map<std::string, std::string> data;
  data["address"] = this->address_str();
  data["user"] = to_string(static_cast<unsigned>(pending.user_index + 1));
  data["timestamp"] = this->format_local_datetime_(measurement.timestamp);
  data["systolic"] = to_string(static_cast<unsigned>(measurement.systolic_mm_hg));
  data["diastolic"] = to_string(static_cast<unsigned>(measurement.diastolic_mm_hg));
  data["pulse"] = to_string(static_cast<unsigned>(measurement.pulse_bpm));
  data["irregular_pulse"] = measurement.irregular_heartbeat ? "1" : "0";
  data["body_movement"] = measurement.movement_detected ? "1" : "0";
  data["cuff_fit"] = measurement.cuff_flag ? "1" : "0";
  // Key 000a, and a number rather than a flag. Not "improper_position", which
  // is a different field with its own keys that this record does not carry.
  data["consecutive_measurement"] = to_string(static_cast<unsigned>(measurement.consecutive_measurement));
  this->fire_homeassistant_event("esphome.omron_measurement", data);
#endif
  this->enable_loop();
}

void OmronBLEClient::publish_device_clock_() {
  if (this->poll_layout_.clock_size == 0 || this->profile_ == nullptr)
    return;
  const std::vector<uint8_t> raw =
      this->session_.record_memory().read(this->poll_layout_.clock_address, this->poll_layout_.clock_size);
  if (raw.size() != this->poll_layout_.clock_size)
    return;

  OmronDateTime clock{};
  const ClockParseError error = parse_device_clock(raw, this->profile_->clock_fields_offset, clock);
  // A failed checksum means the window itself is wrong, so nothing in it is
  // worth showing. A bad date with a good checksum means the cuff's clock was
  // never set, which is worth showing exactly as it reads.
  if (error != ClockParseError::NONE && error != ClockParseError::INVALID_DATE) {
    ESP_LOGD(TAG, "[%s] Device clock rejected: %s", this->address_str(), clock_parse_error_to_string(error));
    return;
  }
  if (error == ClockParseError::INVALID_DATE) {
    ESP_LOGI(TAG, "[%s] Device clock is not set (reads %04u-%02u-%02u %02u:%02u:%02u)", this->address_str(),
             static_cast<unsigned>(clock.year), static_cast<unsigned>(clock.month), static_cast<unsigned>(clock.day),
             static_cast<unsigned>(clock.hour), static_cast<unsigned>(clock.minute),
             static_cast<unsigned>(clock.second));
    this->entities_.publish_clock_entities_(this->format_local_datetime_(clock), NAN);
    return;
  }

  // Drift is only meaningful against a real clock, and NaN is how an entity
  // says "no value" rather than "zero drift".
  float drift = NAN;
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    const ESPTime now = this->time_->now();
    if (now.is_valid())
      drift = static_cast<float>(civil_seconds(clock) - civil_seconds(local_now_(now)));
  }
#endif

  const std::string formatted = this->format_local_datetime_(clock);
  ESP_LOGD(TAG, "[%s] Device clock %s (drift %.0f s)", this->address_str(), formatted.c_str(), drift);
  this->entities_.publish_clock_entities_(formatted, drift);
}

void OmronBLEClient::publish_selected_measurement_(uint8_t user_index, const HarvestedRecord &selected) {
  // The timestamp is formatted here because the zone offset needs this node's
  // own time source; everything else about the record is decided in
  // omron_publish, where a test can hold it.
  this->entities_.publish_user_measurement_(
      user_index, entity_from_record(selected.measurement, user_index,
                                     this->format_local_datetime_(selected.measurement.timestamp)));
}

void OmronBLEClient::publish_standard_measurement_(std::span<const uint8_t> data) {
  StandardBloodPressureMeasurement measurement;
  // Bytes past the last field the flags declare are ignored rather than fatal.
  // Every field is read left to right from those flags, so a longer packet
  // cannot have shifted anything already decoded, and this is the path a cuff
  // nobody has mapped falls back to - the one place least able to afford losing
  // a reading over a suffix it did not need.
  const StandardBpError error = parse_standard_blood_pressure_measurement(data, measurement, false);
  if (error != StandardBpError::NONE) {
    ESP_LOGD(TAG, "[%s] Standard BP notification rejected: %s", this->address_str(),
             standard_bp_error_to_string(error));
    return;
  }
  // Ranges, unit conversion and above all which person this belongs to are
  // decided in omron_publish. Attribution is the part worth a test: a live
  // notification with no user id on a two-person cuff must be dropped rather
  // than filed under slot one.
  std::string timestamp;
  if (measurement.has_timestamp) {
    timestamp = this->format_local_datetime_(OmronDateTime{measurement.timestamp.year, measurement.timestamp.month,
                                                           measurement.timestamp.day, measurement.timestamp.hour,
                                                           measurement.timestamp.minute, measurement.timestamp.second});
  }
  uint8_t user_index = 0;
  OmronEntityData entity;
  const StandardPublishDecision decision =
      standard_measurement_entity(measurement, this->profile_, timestamp, user_index, entity);
  if (decision != StandardPublishDecision::PUBLISH) {
    ESP_LOGD(TAG, "[%s] Standard BP notification not published: %s", this->address_str(),
             standard_publish_decision_to_string(decision));
    return;
  }
  this->entities_.publish_user_measurement_(user_index, entity);
  this->live_only_published_ = true;
  this->entities_.publish_status_entity_(this->live_only_ ? "live standard GATT; no memory map for this cuff"
                                                          : "live standard GATT; EEPROM poll still active");
}

// Profile and status describe the cuff and its last poll, not a person, so they
// must not travel through the measurement publisher: routed that way, a status
// update overwrites a fresh measurement's derived metrics with NaN.
void OmronBLEClient::publish_status_(const std::string &status) {
  this->entities_.publish_profile_entity_(this->profile_ == nullptr ? "unsupported" : this->profile_->model);
  this->entities_.publish_status_entity_(status);
}

void OmronBLEClient::finish_diagnostics_(bool success) {
  if (!this->session_started_)
    return;
  const uint32_t now = millis();
  this->diagnostics_.finish_session(now, success);
  this->entities_.publish_poll_duration_entity_(this->diagnostics_.last_poll_duration_ms / 1000.0f);
  this->session_started_ = false;
  // Which rings this session may claim to have collected: the session owns that
  // memory, because it is the thing that decides whether to read them again.
  this->session_.finish(success);
  // Drives the next-poll decision: success returns to the normal interval, a
  // failure starts the exponential backoff.
  this->scheduler_.note_poll_finished(now, success);
}

void OmronBLEClient::finish_poll_(const std::string &status) {
  this->publish_status_(status);
  this->connect_attempt_ = 0;
  this->finish_diagnostics_(true);
  // The outcome is already recorded in the counters and the status text; the
  // phase tracks what the client is doing now, which is tearing the link down.
  this->diagnostics_.phase = SessionPhase::DISCONNECTING;
  this->disconnect();
}

void OmronBLEClient::reset_session_() {
  this->connection_gate_.reset(this->requires_os_bond_());
  this->security_started_ = false;
  // Cleared with the rest of the per-session state, so a later session cannot
  // inherit a pairing that belonged to an earlier one and write a registration
  // block it did not earn.
  this->paired_this_session_ = false;
  this->session_failure_handled_ = false;
  this->session_started_ = false;
  this->auth_started_at_ = 0;
  this->rx_handles_.fill(0);
  this->tx_handles_.fill(0);
  this->tx_properties_.fill(0);
  this->rx_handle_count_ = 0;
  this->tx_handle_count_ = 0;
  this->unlock_handle_ = 0;
  this->unlock_properties_ = 0;
  this->standard_bp_handle_ = 0;
  // The live-only decision belongs to a session, not to the node: the next
  // connection re-reads device information only once per boot, but a cuff that
  // was unidentifiable is re-examined from whatever it presents this time.
  this->live_only_ = false;
  this->live_only_started_at_ = 0;
  this->live_only_window_open_ = false;
  this->live_only_published_ = false;
  this->subscriptions_.clear();
  this->subscription_started_at_ = 0;
  this->notifications_ready_at_ = 0;
  this->notifications_configured_ = false;
  this->transaction_start_pending_ = false;
  this->writer_.clear();
  this->outgoing_channel_ = SessionChannel::PROTOCOL;
  this->outgoing_prefer_no_response_ = false;
  this->write_handle_ = 0;
  this->write_in_flight_ = false;
  this->write_started_at_ = 0;
  this->discovery_started_at_ = 0;
  this->session_.reset();
}

void OmronBLEClient::fail_session_(const char *reason, int code) {
  if (this->session_failure_handled_)
    return;
  this->session_failure_handled_ = true;
  // Before anything else, so a session that is mid-frame stops touching the
  // wire whether the failure came from it or from the link underneath it.
  this->session_.abort();
  if (code == 0) {
    ESP_LOGE(TAG, "[%s] %s", this->address_str(), reason);
  } else {
    ESP_LOGE(TAG, "[%s] %s (code=%d)", this->address_str(), reason, code);
  }
  this->publish_status_(reason);
  this->finish_diagnostics_(false);
  // The cuff blinks Err until the memory session it opened is closed, and
  // dropping the link does not close it. Whatever went wrong up here, the
  // device should not be left showing an error to whoever walks past it.
  if (this->close_memory_session_()) {
    this->set_timeout("teardown", END_TEARDOWN_DELAY_MS, [this]() { this->disconnect(); });
    return;
  }
  this->disconnect();
}

bool OmronBLEClient::close_memory_session_() {
  if (!this->end_session_ || this->tx_handle_count_ == 0 || this->tx_handles_[0] == 0)
    return false;
  // Only once the start opcode has been acknowledged is there a session to
  // close; before that the end opcode would be answering a question nobody
  // asked.
  if (!this->session_.transfer_open())
    return false;
  if (this->state() != esp32_ble_tracker::ClientState::CONNECTED &&
      this->state() != esp32_ble_tracker::ClientState::ESTABLISHED)
    return false;

  auto frame = make_end_request();
  const esp_gatt_char_prop_t properties = this->tx_properties_[0];
  const esp_gatt_write_type_t write_type =
      (properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0 ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
  const esp_err_t error =
      esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->tx_handles_[0],
                               static_cast<uint16_t>(frame.size()), frame.data(), write_type, ESP_GATT_AUTH_REQ_NONE);
  if (error != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Could not close the memory session (code=%d); the display may show Err", this->address_str(),
             error);
    return false;
  }
  ESP_LOGD(TAG, "[%s] Closing the memory session before teardown", this->address_str());
  return true;
}

}  // namespace esphome::omron

#endif  // USE_ESP32
