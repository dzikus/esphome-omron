#pragma once

#include <cstdint>

#include "omron_protocol.h"

namespace esphome::omron {

enum class SessionPhase : uint8_t {
  IDLE = 0,
  CONNECTING,
  WAITING_FOR_OPEN,
  WAITING_FOR_AUTH,
  DISCOVERING,
  SUBSCRIBING,
  UNLOCKING,
  STARTING_TRANSFER,
  READING_INDEX,
  READING_RECORDS,
  ENDING_TRANSFER,
  DISCONNECTING,
  REMOVING_BOND,
  COMPLETE,
  ERROR,
};

struct OmronDiagnostics {
  SessionPhase phase{SessionPhase::IDLE};
  ProtocolError last_protocol_error{ProtocolError::NONE};
  uint32_t session_started_ms{0};
  uint32_t last_poll_duration_ms{0};
  uint32_t connection_attempts{0};
  uint32_t protocol_failures{0};
  // Frames and acknowledgements that arrived with no state to receive them.
  // Not failures, and each one was already being dropped - the count exists so
  // the dropping stops being invisible.
  uint32_t unexpected_replies{0};
  uint32_t cleanup_failures{0};
  uint32_t successful_polls{0};

  void begin_session(uint32_t now_ms);
  void finish_session(uint32_t now_ms, bool success);
};

}  // namespace esphome::omron
