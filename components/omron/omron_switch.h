#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome::omron {

class OmronBLEClient;

// Local master switch for this cuff's radio. It writes nothing to the device:
// turning it off only stops this node from connecting, and tears down a session
// already in flight.
//
// The cuff accepts one connection at a time and answers a second one with an
// SMP failure, so there has to be a way to stand down and let another host
// have it.
//
// A Component because switch_::Switch restores nothing on its own: the derived
// class has to ask for the stored state in its own setup(), or the configured
// restore mode is generated and read by nobody.
class OmronBleSwitch : public switch_::Switch, public Parented<OmronBLEClient>, public Component {
 public:
  void setup() override;

 protected:
  void write_state(bool state) override;
};

}  // namespace esphome::omron
