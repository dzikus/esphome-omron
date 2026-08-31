#pragma once

#include <cstdint>

namespace esphome::omron {

// [[nodiscard]] for the same reason the protocol errors carry it: this is an
// instruction, not a report. Dropped, the gate has said "start discovery" and
// nobody did, and the session sits on the link until the peer gives up.
enum class [[nodiscard]] ConnectionAction : uint8_t {
  NONE = 0,
  START_SECURITY,
  START_DISCOVERY,
  DISCONNECT,
};

// Pure ordering gate used by the ESP-IDF event adapter. It intentionally knows
// nothing about GATT handles so both OPEN->AUTH and AUTH->OPEN are testable on
// the host without ESPHome.
class OmronConnectionGate {
 public:
  void reset(bool requires_os_bond);
  ConnectionAction on_connect() const;
  ConnectionAction on_open(bool success);
  ConnectionAction on_auth_complete(bool success, bool bonded);
  void mark_discovery_started();

  bool open_ok() const { return this->open_ok_; }
  bool auth_ok() const { return this->auth_ok_; }
  bool discovery_started() const { return this->discovery_started_; }
  bool failed() const { return this->failed_; }

 private:
  ConnectionAction maybe_start_discovery_() const;

  bool requires_os_bond_{false};
  bool open_ok_{false};
  bool auth_ok_{false};
  bool discovery_started_{false};
  bool failed_{false};
};

}  // namespace esphome::omron
