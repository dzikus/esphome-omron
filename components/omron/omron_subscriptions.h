#pragma once

// Turning on the notify channels this session needs, one CCCD at a time.
//
// ESP-IDF accepting a register-for-notify request is NOT the same as a CCCD
// write having started: ESPHome's base class looks the descriptor up, and when
// that answers NOT_FOUND it logs a warning and does nothing else. No write, no
// ESP_GATTC_WRITE_DESCR_EVT, and a queue waiting on an event that never comes.
// The per-target retry clock here is the only recovery.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::omron {

// Everything this machine is allowed to ask of the link. Deliberately small: it
// never sees a connection, a profile or a handle it was not given.
class OmronSubscriptionHost {
 public:
  virtual ~OmronSubscriptionHost() = default;
  virtual uint32_t subscription_now_ms() = 0;
  // False means the request could not even be made; `error` carries the code.
  virtual bool subscription_register(uint16_t characteristic_handle, int &error) = 0;
  virtual void subscription_ready() = 0;
  virtual void subscription_failed(const char *reason, int error) = 0;
  // The host forgets the handle; the remaining targets carry on.
  virtual void subscription_dropped_optional(uint16_t characteristic_handle, const char *why, int error) = 0;
  // The peer refused a CCCD write until the link is encrypted. Returning true
  // means the host has started security and this machine should park until
  // resume() - false means it could not, and the target is treated as failed.
  virtual bool subscription_needs_encryption(int status) = 0;
  virtual const char *subscription_address() = 0;
  // Diagnostic only, so a retry can say which handle it is re-asking for.
  virtual void subscription_retrying(uint16_t characteristic_handle, uint8_t attempt, uint8_t of) = 0;
};

class OmronSubscriptions {
 public:
  struct Target {
    uint16_t characteristic_handle{0};
    uint16_t descriptor_handle{0};
    bool required{false};
  };

  enum class Phase : uint8_t {
    IDLE,
    // Register-for-notify accepted, CCCD write expected. The phase that hangs.
    SUBSCRIBING,
    // The peer asked for encryption and the host is arranging it.
    AWAITING_ENCRYPTION,
    READY,
    FAILED,
  };

  void set_host(OmronSubscriptionHost *host) { this->host_ = host; }

  void clear();
  // Targets are added in the order they will be subscribed. Required ones cost
  // the session if they fail; optional ones are dropped and skipped.
  void add(uint16_t characteristic_handle, uint16_t descriptor_handle, bool required);
  bool empty() const { return this->targets_.empty(); }
  size_t size() const { return this->targets_.size(); }

  // With no targets this completes at once: a session that needs no
  // notifications is ready by definition.
  void begin(uint32_t now);
  void tick(uint32_t now);

  // ESP_GATTC_REG_FOR_NOTIFY_EVT. `ok` false means the peer refused.
  void on_register_result(uint16_t characteristic_handle, bool ok, int status);
  // ESP_GATTC_WRITE_DESCR_EVT for a CCCD.
  void on_descriptor_written(uint16_t descriptor_handle, bool ok, int status);
  // The host finished the encryption this machine parked for.
  void resume(uint32_t now);

  Phase phase() const { return this->phase_; }
  bool ready() const { return this->phase_ == Phase::READY; }
  // Neither finished nor given up. AWAITING_ENCRYPTION is deliberately excluded:
  // that wait has its own timeout on the host, and two clocks on one wait get
  // retuned by accident.
  bool pending() const { return this->phase_ == Phase::SUBSCRIBING; }

  // How long a target may sit with no CCCD write before it is asked again. A
  // subscription that works answers in tens of milliseconds, and every attempt
  // still has to fit inside the phase timeout the host applies.
  static constexpr uint32_t RETRY_INTERVAL_MS = 600;
  static constexpr uint8_t MAX_ATTEMPTS = 4;

 private:
  void subscribe_current_(uint32_t now);
  void advance_(uint32_t now);
  void drop_optional_or_fail_(const char *why, int error, uint32_t now);

  OmronSubscriptionHost *host_{nullptr};
  std::vector<Target> targets_{};
  size_t index_{0};
  Phase phase_{Phase::IDLE};
  uint32_t target_at_{0};
  uint8_t attempt_{0};
};

}  // namespace esphome::omron
