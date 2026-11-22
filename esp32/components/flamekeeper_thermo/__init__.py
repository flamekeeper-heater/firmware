"""Bluetooth thermometers as a room temperature source (PROTOCOL.md).

Listens to ESPHome's BLE tracker and passes every advert it can decode (BTHome v2, Xiaomi
LYWSD03MMC with custom firmware, Govee, SwitchBot meters: flamekeeper/ble_thermo.*) to the hub's
thermometer list. The apps search for thermometers, pick one and set the room source to it; nothing
is paired. Its presence sets capability bit 7.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker
from esphome.components.flamekeeper import CONF_FLAMEKEEPER_ID, FlamekeeperHub
from esphome.const import CONF_ID
from esphome.core import CORE

CONF_SEARCH_WINDOW = "search_window"

CODEOWNERS = []
DEPENDENCIES = ["esp32", "flamekeeper"]
AUTO_LOAD = ["esp32_ble_tracker"]

flamekeeper_thermo_ns = cg.esphome_ns.namespace("flamekeeper_thermo")
ThermoListener = flamekeeper_thermo_ns.class_(
    "ThermoListener", esp32_ble_tracker.ESPBTDeviceListener, cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ThermoListener),
            cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub),
            # The scan window while an app searches for thermometers (the tracker's own window
            # the rest of the time). At most the tracker's interval.
            cv.Optional(CONF_SEARCH_WINDOW, default="100ms"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_FLAMEKEEPER_ID])
    cg.add(var.set_hub(hub))
    # Capability bit 7, before any entity restores room source 2. Through the hub: this may run
    # before the hub's set_heater has made the controller (a null controller crashes at boot).
    cg.add(hub.enable_bluetooth_room())
    scan = CORE.config.get("esp32_ble_tracker", {}).get("scan_parameters", {})
    interval_ms = scan["interval"].total_milliseconds if "interval" in scan else 320
    normal_ms = scan["window"].total_milliseconds if "window" in scan else 30
    search_ms = min(config[CONF_SEARCH_WINDOW].total_milliseconds, interval_ms)
    cg.add(var.set_windows(int(normal_ms / 0.625), int(max(search_ms, normal_ms) / 0.625)))
    await esp32_ble_tracker.register_ble_device(var, config)
