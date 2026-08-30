#include "omron_connection_gate.h"

namespace esphome::omron {

void OmronConnectionGate::reset(bool requires_os_bond) {
  this->requires_os_bond_ = requires_os_bond;
  this->open_ok_ = false;
  this->auth_ok_ = !requires_os_bond;
  this->discovery_started_ = false;
  this->failed_ = false;
}

ConnectionAction OmronConnectionGate::on_connect() const {
  return this->requires_os_bond_ ? ConnectionAction::START_SECURITY : ConnectionAction::NONE;
}

ConnectionAction OmronConnectionGate::on_open(bool success) {
  if (!success) {
    this->failed_ = true;
    return ConnectionAction::DISCONNECT;
  }
  this->open_ok_ = true;
  return this->maybe_start_discovery_();
}

ConnectionAction OmronConnectionGate::on_auth_complete(bool success, bool bonded) {
  if (!this->requires_os_bond_)
    return ConnectionAction::NONE;
  if (!success || !bonded) {
    this->failed_ = true;
    return ConnectionAction::DISCONNECT;
  }
  this->auth_ok_ = true;
  return this->maybe_start_discovery_();
}

ConnectionAction OmronConnectionGate::maybe_start_discovery_() const {
  if (!this->failed_ && this->open_ok_ && this->auth_ok_ && !this->discovery_started_)
    return ConnectionAction::START_DISCOVERY;
  return ConnectionAction::NONE;
}

void OmronConnectionGate::mark_discovery_started() {
  this->discovery_started_ = true;
}

}  // namespace esphome::omron
