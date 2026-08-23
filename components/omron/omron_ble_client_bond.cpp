#include <cstring>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "omron_ble_client.h"

#ifdef USE_ESP32

// Bonding, link security and this node's own address. Part of OmronBLEClient,
// in its own translation unit: all of it is ESP-IDF and none of it is testable
// on the host, but it is the half of the client most often read on its own.
//
// Several comments below quote HCI codes and timings, because the choices they
// explain look arbitrary without them. Auth fail 97 is the one to read first:
// it appears in the SMP layer and is not caused there.

namespace esphome::omron {

static const char *const TAG = "omron.ble";

void OmronBLEClient::log_local_address_() {
  esp_bd_addr_t addr{};
  uint8_t addr_type = 0;
  if (esp_ble_gap_get_local_used_addr(addr, &addr_type) != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Could not read this node's own BLE address", this->address_str());
    return;
  }
  // Type 0 is the public identity address, anything else is a random one. Worth
  // a line every session because the cuff keys its bond on the identity handed
  // over while bonding, not on the address it sees, so this is what tells the
  // two apart when a bond stops being accepted.
  ESP_LOGI(TAG, "[%s] Own BLE address at connect: %02X:%02X:%02X:%02X:%02X:%02X (type %u, %s)", this->address_str(),
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], static_cast<unsigned>(addr_type),
           addr_type == 0 ? "public" : "random");
}

void OmronBLEClient::configure_key_distribution_() {
  if (this->exchange_identity_keys_)
    return;
  // ESPHome itself sets only IO capability, so the default is bluedroid's
  // ENC|ID|CSR and every bond takes the peer's IRK - which the cuff hands out as
  // sixteen zero bytes, the pattern esphome#17104 blames for unusable bonds.
  // Narrowing it changes nothing on these cuffs, and this build never passes a
  // peer IRK to the controller anyway (CONTROLLER_RPA_LIST_ENABLE is FALSE
  // without CONFIG_BT_BLE_RPA_SUPPORTED).
  //
  // Per connect, not once: bluedroid reads the masks while building the pairing
  // request, and a later call supersedes an earlier one.
  uint8_t key_mask = ESP_BLE_ENC_KEY_MASK;
  esp_err_t error = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &key_mask, sizeof(key_mask));
  if (error != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Could not narrow the keys we distribute: %d", this->address_str(), static_cast<int>(error));
    return;
  }
  error = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &key_mask, sizeof(key_mask));
  if (error != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Could not narrow the keys we ask for: %d", this->address_str(), static_cast<int>(error));
    return;
  }
  ESP_LOGI(TAG, "[%s] Key distribution: encryption key only, no identity key in either direction", this->address_str());
}

void OmronBLEClient::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // Bonded reconnect: answer with the encryption itself rather than a security
  // response. The base class replies esp_ble_gap_security_rsp(..., true) and
  // leaves bluedroid to decide what that means; skipping it is deliberate, one
  // request and one answer.
  //
  // The bond is looked up here rather than carried in on a flag because the
  // request arrives about 100 ms BEFORE ESP_GATTC_CONNECT_EVT, so anything
  // armed from the connect path is too late and the base class answers first.
  //
  // Not what makes reconnection work - registering in the cuff's memory is,
  // see OmronSession::maybe_queue_registration_writes_ - but it is the order
  // the device expects.
  if (event == ESP_GAP_BLE_SEC_REQ_EVT && this->requires_os_bond_() &&
      this->check_addr(param->ble_security.ble_req.bd_addr)) {
    esp_err_t lookup_error = ESP_OK;
    if (this->lookup_own_bond_(&lookup_error) == BondLookupResult::PRESENT) {
      this->security_started_ = true;
      this->auth_started_at_ = millis();
      ESP_LOGI(TAG, "[%s] Cuff asked for security; encrypting with the stored key", this->address_str());
      const esp_err_t error = esp_ble_set_encryption(this->remote_bda_, ESP_BLE_SEC_ENCRYPT_NO_MITM);
      if (error != ESP_OK)
        this->fail_session_("esp_ble_set_encryption failed", error);
      return;
    }
  }

  // Declining the way a host that does not bond would. Off by default and kept
  // as a lever; no cuff here needs it.
  //
  // Never while the cuff is offering pairing: a long press is this side asking
  // to bond on purpose, and declining in the same breath produces failed
  // attempts and reason 86. Declining is only for sessions nobody asked to bond
  // in.
  if (event == ESP_GAP_BLE_SEC_REQ_EVT && !this->accept_security_request_ && !this->advertised_pairing_mode_ &&
      this->check_addr(param->ble_security.ble_req.bd_addr)) {
    ESP_LOGI(TAG, "[%s] Cuff asked for security; declining, the way a host that does not bond would",
             this->address_str());
    esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, false);
    return;
  }
  // Preserve ESPHome's address-filtered SEC_REQ response and paired_ tracking.
  esp32_ble_client::BLEClientBase::gap_event_handler(event, param);

  if (event != ESP_GAP_BLE_AUTH_CMPL_EVT || !this->requires_os_bond_() ||
      !this->check_addr(param->ble_security.auth_cmpl.bd_addr))
    return;

  const auto &auth = param->ble_security.auth_cmpl;
  // Deliberately outside the security_started_ check below. The cuff asks for
  // security on its own the moment the link is up, so the failure that matters
  // most arrives on a connection this side never started security on, and
  // gating this on our own request threw exactly that case away.
  if (!auth.success && auth.fail_reason == AUTH_FAIL_ENCRYPTION) {
    ESP_LOGW(TAG, "[%s] Encryption failed against a stored bond; dropping it so the next connection pairs instead",
             this->address_str());
    this->stale_bond_suspected_ = true;
  }

  if (!this->security_started_) {
    if (!auth.success) {
      ESP_LOGW(TAG, "[%s] Cuff-initiated authentication failed (reason=%d)", this->address_str(),
               static_cast<int>(auth.fail_reason));
    }
    return;
  }

  // Security the cuff asked for by refusing an ATT write, not something this
  // side decided at connect. Discovery is already done, so the connection gate
  // has nothing left to rule on and its bond-bit requirement does not apply.
  if (this->subscriptions_.phase() == OmronSubscriptions::Phase::AWAITING_ENCRYPTION) {
    if (!auth.success) {
      this->fail_session_("Cuff asked for encryption and then refused it", auth.fail_reason);
      return;
    }
    ESP_LOGI(TAG, "[%s] Link encrypted at the cuff's request (mode=0x%02x); retrying the subscription",
             this->address_str(), auth.auth_mode);
    this->subscriptions_.resume(millis());
    return;
  }

  const bool bonded = (auth.auth_mode & ESP_LE_AUTH_BOND) != 0;
  const ConnectionAction action = this->connection_gate_.on_auth_complete(auth.success, bonded);
  if (action == ConnectionAction::DISCONNECT) {
    this->fail_session_(auth.success ? "AUTH_CMPL succeeded without the BOND bit" : "AUTH_CMPL failed",
                        auth.success ? static_cast<int>(auth.auth_mode) : static_cast<int>(auth.fail_reason));
    return;
  }

  // The count is the other half of the question the connect log asks: a
  // successful pairing that leaves no record behind is a different fault from
  // a record the peer later refuses.
  ESP_LOGI(TAG, "[%s] AUTH_CMPL: bonded authentication succeeded (mode=0x%02x, %d bond record(s) stored)",
           this->address_str(), auth.auth_mode, esp_ble_get_bond_device_num());
  this->apply_connection_action_(action);
}

void OmronBLEClient::apply_connection_action_(ConnectionAction action) {
  switch (action) {
    case ConnectionAction::START_SECURITY:
      this->start_security_();
      break;
    case ConnectionAction::START_DISCOVERY:
      this->start_discovery_();
      break;
    case ConnectionAction::DISCONNECT:
      this->fail_session_("Connection gate rejected lifecycle event");
      break;
    case ConnectionAction::NONE:
      break;
  }
}

void OmronBLEClient::start_security_() {
  if (this->security_started_)
    return;

  esp_err_t lookup_error = ESP_OK;
  const BondLookupResult bond = this->lookup_own_bond_(&lookup_error);
  if (bond == BondLookupResult::ERROR)
    ESP_LOGW(TAG, "[%s] Bond list could not be read (code=%d)", this->address_str(), lookup_error);

  // Wait for the cuff to ask, then encrypt. It sends a security request and
  // accepts LE Enable Encryption about 17 ms later. Encrypting at connect
  // instead, before it has asked, answers SMP_ENC_FAIL within 120 ms even
  // against a bond seconds old.
  if (bond == BondLookupResult::PRESENT) {
    ESP_LOGI(TAG, "[%s] CONNECT: bond stored (keys 0x%02x peer-LTK=%s IRK=%s); waiting for the cuff to ask",
             this->address_str(), static_cast<unsigned>(this->bond_key_mask_),
             YESNO((this->bond_key_mask_ & ESP_BLE_ENC_KEY_MASK) != 0),
             YESNO((this->bond_key_mask_ & ESP_BLE_ID_KEY_MASK) != 0));
    this->security_started_ = true;
    this->auth_started_at_ = millis();
    this->diagnostics_.phase = SessionPhase::WAITING_FOR_AUTH;
    return;
  }

  // With no bond and the cuff not offering pairing, starting SMP makes it
  // terminate the link the same instant (disconnect 0x13, reported as
  // SMP_CONN_TOUT). Inside pairing mode the identical code bonds and reads every
  // record. A new bond is on offer only while the cuff says so, and asking
  // outside that window does not merely fail - it costs the whole session.
  if (!this->advertised_pairing_mode_) {
    ESP_LOGI(TAG, "[%s] CONNECT: no bond and the cuff is not offering pairing; going straight to discovery",
             this->address_str());
    this->apply_connection_action_(this->connection_gate_.on_auth_complete(true, true));
    return;
  }

  this->security_started_ = true;
  this->paired_this_session_ = true;
  this->session_.set_paired_this_session(true);
  this->auth_started_at_ = millis();
  this->diagnostics_.phase = SessionPhase::WAITING_FOR_AUTH;
  ESP_LOGI(TAG,
           "[%s] CONNECT: cuff is offering pairing and no bond is stored, pairing now (%d record(s) held for "
           "other peers)",
           this->address_str(), esp_ble_get_bond_device_num());
  const esp_err_t error = esp_ble_set_encryption(this->remote_bda_, ESP_BLE_SEC_ENCRYPT_NO_MITM);
  if (error != ESP_OK)
    this->fail_session_("esp_ble_set_encryption failed", error);
}

bool OmronBLEClient::requires_os_bond_() const {
  // Dropping the bond at the end of a session is only safe because the next
  // connect makes a new one, so these two are not independent settings: a hub
  // that cleans up must also bond. "Drop the bond and never make another" is
  // reachable here as two innocent-looking options, and it opens the link,
  // starts discovery against an unencrypted host, and gets hung up on in the
  // same millisecond with disconnect reason 19. Derived rather than left to a
  // profile to set both consistently.
  if (this->requires_per_session_cleanup_())
    return true;
  if (this->require_bond_set_)
    return this->require_bond_;
  return this->profile_ != nullptr && requires_os_bond(*this->profile_);
}

bool OmronBLEClient::requires_per_session_cleanup_() const {
  // A profile that never bonds has no bond to remove, whatever yaml asks for.
  if (this->profile_ == nullptr || this->profile_->security_mode != SecurityMode::OS_BOND)
    return false;
  if (this->keep_bond_set_)
    return !this->keep_bond_;
  return remove_bond_after_session(*this->profile_);
}

void OmronBLEClient::on_disconnect_complete(esp_err_t reason) {
  ESP_LOGD(TAG, "[%s] Full BLE teardown complete, reason=%d", this->address_str(), reason);
  // Keyed on security_started_, not session_started_: the failure path clears
  // the latter through finish_diagnostics_ long before the teardown completes,
  // so a condition hung on it is dead on arrival. security_started_ survives
  // until reset_session_() below, which is exactly the window this asks about.
  const bool during_security = this->security_started_ && !this->connection_gate_.discovery_started();
  if (this->session_started_) {
    this->publish_status_("BLE disconnected before poll completion");
    this->finish_diagnostics_(false);
  }
  this->entities_.publish_connection_entity_(false);
  this->reset_session_();
  if (this->forget_bond_requested_ || this->stale_bond_suspected_) {
    const char *why = this->forget_bond_requested_ ? "bond dropped on request" : "stale bond after encryption failure";
    this->forget_bond_requested_ = false;
    this->stale_bond_suspected_ = false;
    this->begin_bond_cleanup_(why, true);
  } else if (this->requires_per_session_cleanup_()) {
    this->begin_bond_cleanup_("post-disconnect");
  }
  // After the cleanup call, so the retry waits behind it: pairing afresh needs
  // the bond the cuff has already forgotten to be gone on our side too.
  if (during_security)
    this->maybe_retry_connect_();
}

void OmronBLEClient::maybe_retry_connect_() {
  if (!this->ble_user_enabled_)
    return;
  if (this->connect_attempt_ >= CONNECT_ATTEMPTS) {
    ESP_LOGW(TAG, "[%s] Cuff dropped the link during bonding on all %u attempts; giving up on this invitation",
             this->address_str(), static_cast<unsigned>(CONNECT_ATTEMPTS));
    // The chain ends here, so the next invitation starts counting from zero
    // instead of inheriting a used-up budget and never retrying again.
    this->connect_attempt_ = 0;
    return;
  }
  ESP_LOGI(TAG, "[%s] Cuff dropped the link during bonding (attempt %u/%u); re-establishing on its next advertisement",
           this->address_str(), static_cast<unsigned>(this->connect_attempt_), static_cast<unsigned>(CONNECT_ATTEMPTS));
  // Treated as a continuation of the invitation already given: the cuff need
  // not raise its flags again for a session it dropped mid-handshake.
  this->poll_requested_ = true;
  this->scheduler_.request_retry(millis());
}

void OmronBLEClient::begin_bond_cleanup_(const char *reason, bool force) {
  // A stale bond is dropped whatever the configured policy says. Keeping a key
  // the peer has forgotten is worse than having none: every later connection
  // then fails encryption instead of pairing afresh, and nothing ever recovers.
  if (!force && !this->requires_per_session_cleanup_())
    return;

  this->bond_cleanup_.begin(reason);
  this->enable_loop();
}

// OmronBondCleanupHost. The state machine is in omron_bond_cleanup.*; what is
// left here is the three things only ESP-IDF can answer.
uint32_t OmronBLEClient::bond_now_ms() {
  return millis();
}

const char *OmronBLEClient::bond_address() {
  return this->address_str();
}

bool OmronBLEClient::bond_remove(int &error) {
  const esp_err_t result = esp_ble_remove_bond_device(this->remote_bda_);
  error = static_cast<int>(result);
  return result == ESP_OK;
}

void OmronBLEClient::bond_forget_attribute_cache() {
  // BLEClientBase already cleans this when a connection is released. This is
  // the case the stack cannot see: the bond was removed deliberately while
  // nothing was connected, and the next session rediscovers everything from a
  // peer that now treats this node as a stranger.
  //
  // esp_ble_gattc_cache_clean takes an address rather than a connection and is
  // meant to be called while disconnected, which is where the cleanup runs.
  const esp_err_t error = esp_ble_gattc_cache_clean(this->remote_bda_);
  if (error != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Could not drop the cached attribute table (code=%d)", this->address_str(), error);
    return;
  }
  ESP_LOGI(TAG, "[%s] Dropped the cached attribute table along with the bond", this->address_str());
}

BondLookupResult OmronBLEClient::bond_lookup(int &error) {
  esp_err_t status = ESP_OK;
  const BondLookupResult result = this->lookup_own_bond_(&status);
  error = static_cast<int>(status);
  return result;
}

BondLookupResult OmronBLEClient::lookup_own_bond_(esp_err_t *error) {
  *error = ESP_OK;
  const int count = esp_ble_get_bond_device_num();
  if (count == 0)
    return BondLookupResult::ABSENT;
  if (count < 0 || count > MAX_BOND_RECORDS_TO_SCAN) {
    *error = ESP_FAIL;
    return BondLookupResult::ERROR;
  }

  std::vector<esp_ble_bond_dev_t> devices(static_cast<size_t>(count));
  int listed = count;
  *error = esp_ble_get_bond_device_list(&listed, devices.data());
  if (*error != ESP_OK)
    return BondLookupResult::ERROR;

  for (int i = 0; i < listed; i++) {
    // This deliberately removes only the configured Omron address. RPA versus
    // identity-address behaviour is still a hardware-verification item.
    if (std::memcmp(devices[static_cast<size_t>(i)].bd_addr, this->remote_bda_, sizeof(esp_bd_addr_t)) == 0) {
      // Re-encrypting a later connection needs the peer's own LTK, the PENC
      // key. A record exists as soon as either side distributed anything, so
      // "a bond is stored" and "we can re-encrypt with it" are two different
      // facts and only the mask tells them apart.
      const auto &keys = devices[static_cast<size_t>(i)].bond_key;
      this->bond_key_mask_ = keys.key_mask;
      // The mask says a key arrived, not what arrived. Two things in here can
      // explain an encryption that fails against a bond the stack believes it
      // holds: an all-zero IRK, which is what esphome#17104 reports poisoning
      // the resolving list, and an identity address that differs from the one
      // we connect to, which would send the controller looking for the wrong
      // record. Neither is visible from the mask.
      // Key material never reaches the log. What these lines answer is whether a
      // key is present and usable, and "is it all zero" answers exactly that
      // while printing nothing an attacker could use - a log stream reaches the
      // console, whoever runs esphome logs, and whatever collects it.
      if ((keys.key_mask & ESP_BLE_ENC_KEY_MASK) != 0) {
        ESP_LOGI(TAG, "[%s] Bond PENC: ediv=0x%04X size=%u level=%u ltk=%s", this->address_str(), keys.penc_key.ediv,
                 static_cast<unsigned>(keys.penc_key.key_size), static_cast<unsigned>(keys.penc_key.sec_level),
                 all_zero_(keys.penc_key.ltk) ? "ALL ZERO" : "present");
      }
      if ((keys.key_mask & ESP_BLE_ID_KEY_MASK) != 0) {
        // The identity address stays: it is the peer's public identity, it is
        // already in the config file, and a mismatch against the address we
        // connect to is one of the two failures this whole block exists to
        // catch. The IRK itself is key material and only its emptiness matters.
        ESP_LOGI(TAG, "[%s] Bond PID: identity=%02X:%02X:%02X:%02X:%02X:%02X (type %u) irk=%s", this->address_str(),
                 keys.pid_key.static_addr[0], keys.pid_key.static_addr[1], keys.pid_key.static_addr[2],
                 keys.pid_key.static_addr[3], keys.pid_key.static_addr[4], keys.pid_key.static_addr[5],
                 static_cast<unsigned>(keys.pid_key.addr_type), all_zero_(keys.pid_key.irk) ? "ALL ZERO" : "present");
      }
      return BondLookupResult::PRESENT;
    }
  }
  this->bond_key_mask_ = 0;
  return BondLookupResult::ABSENT;
}

}  // namespace esphome::omron

#endif  // USE_ESP32
