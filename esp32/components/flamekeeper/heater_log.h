// The heater's fault log in words (Home Assistant's "Last heater fault"): one entry of the Flamekeeper
// safety MCU's log (heater.h HeaterLogEntry), with its fault, reason and step named.
//
// Plain C++ (host-tested in tests/test_link_client.cpp).
#pragma once

#include <cstddef>

#include "heater.h"

namespace esphome {
namespace flamekeeper {

// "#12 Overheat: the heat exchanger over its limit, while running, 3 h 25 min after a reset (heat
// exchanger 251.3 C, supply 23.80 V, board 46 C)"; "None" for no entry (nullptr). Cut to fit out.
void heater_log_text(const HeaterLogEntry *e, char *out, size_t cap);

}  // namespace flamekeeper
}  // namespace esphome
