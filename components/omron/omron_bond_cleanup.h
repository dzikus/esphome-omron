#pragma once

#include <cstdint>

#include "omron_diagnostics.h"

namespace esphome::omron {

// Dropping this node's own bond record, and waiting until it is actually gone.
//
// It polls because ESP-IDF returns ESP_OK from esp_ble_remove_bond_device for
// "request taken", not "record gone", and ESPHome does not deliver
// REMOVE_BOND_DEV_COMPLETE_EVT to clients. With no event to wait on, watching
// the bond list is the only honest answer.
//
// Invariant: only a cleanup still running may block anything. A failed one that
// leaves the flag standing deadlocks the client, because the paths that could
// start another cleanup are the ones it blocks.

enum class BondLookupResult : uint8_t { ABSENT, PRESENT, ERROR };

class OmronBondCleanupHost {
 public:
  virtual ~OmronBondCleanupHost() = default;
  virtual uint32_t bond_now_ms() = 0;
  // Fills error with whatever the stack said when it could not tell.
  virtual BondLookupResult bond_lookup(int &error) = 0;
  // True means the request was accepted, not that the record is gone.
  virtual bool bond_remove(int &error) = 0;
  // The stack drops its attribute cache when a connection ends, but has no
  // reason to know the bond behind it was thrown away.
  virtual void bond_forget_attribute_cache() = 0;
  virtual const char *bond_address() = 0;
};

class OmronBondCleanup {
 public:
  // ARMED and LOOKING_UP are separate because the client only ticks this while
  // the link is IDLE: a cleanup asked for during a slow teardown must not spend
  // its timeout budget waiting for the disconnect. The clock starts on the
  // first tick, and the phase says so instead of a separate flag.
  enum class Phase : uint8_t {
    // Also where a finished cleanup lands: "no bond record" is the goal state.
    IDLE,
    ARMED,
    LOOKING_UP,
    REMOVE_REQUESTED,
    // Gave up, and deliberately not blocking: a stale bond is recoverable
    // because the peer refuses encryption and we bond again. A wedged client
    // is not.
    FAILED,
  };

  OmronBondCleanup() = default;
  OmronBondCleanup(const OmronBondCleanup &) = delete;
  OmronBondCleanup &operator=(const OmronBondCleanup &) = delete;

  void set_host(OmronBondCleanupHost *host) { this->host_ = host; }
  void set_diagnostics(OmronDiagnostics *diagnostics) { this->diagnostics_ = diagnostics; }
  void set_timeout_ms(uint32_t timeout_ms) { this->timeout_ms_ = timeout_ms; }
  uint32_t timeout_ms() const { return this->timeout_ms_; }

  // Idempotent while one is already running: a second reason arriving
  // mid-flight does not restart the clock.
  void begin(const char *reason);
  // One pass. Call it only while the link is idle: ESP-IDF promises nothing
  // about inspecting the bond list mid-connection.
  void tick(uint32_t now);

  Phase phase() const { return this->phase_; }
  // The invariant at the top of the file is about this and nothing else.
  bool pending() const {
    return this->phase_ == Phase::ARMED || this->phase_ == Phase::LOOKING_UP || this->phase_ == Phase::REMOVE_REQUESTED;
  }
  bool failed() const { return this->phase_ == Phase::FAILED; }

 private:
  // The bond list is a local table: asking more often spends the loop on
  // nothing, asking less often makes a forget-bond press feel broken.
  static constexpr uint32_t POLL_INTERVAL_MS = 250;

  OmronBondCleanupHost *host_{nullptr};
  OmronDiagnostics *diagnostics_{nullptr};
  uint32_t timeout_ms_{10000};
  uint32_t started_at_{0};
  uint32_t last_poll_at_{0};
  Phase phase_{Phase::IDLE};
  // Log dedup, not state. The list can refuse for as long as the timeout
  // allows, and one line a second saying the same thing buries the session.
  bool error_logged_{false};
};

}  // namespace esphome::omron
