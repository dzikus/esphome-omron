#include "omron_diagnostics.h"

namespace esphome::omron {

void OmronDiagnostics::begin_session(uint32_t now_ms) {
  this->session_started_ms = now_ms;
  this->last_protocol_error = ProtocolError::NONE;
  this->connection_attempts++;
  this->phase = SessionPhase::CONNECTING;
}

void OmronDiagnostics::finish_session(uint32_t now_ms, bool success) {
  this->last_poll_duration_ms = now_ms - this->session_started_ms;
  if (success) {
    this->successful_polls++;
    this->phase = SessionPhase::COMPLETE;
  } else {
    this->phase = SessionPhase::ERROR;
  }
}

}  // namespace esphome::omron
