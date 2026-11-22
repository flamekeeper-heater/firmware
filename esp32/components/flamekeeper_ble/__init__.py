"""Flamekeeper BLE service for the phone apps (PROTOCOL.md).

Its own GATT application next to ESPHome's BLE server (which carries the Device Information
service), with the security the protocol asks for: LE Secure Connections with the board's PIN,
bonding, and new pairings only inside the pairing window. The PIN, the Wi-Fi keys and the board
settings are the hub's (flamekeeper: board, pin / pin_efuse_key, home_assistant).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble
from esphome.components.flamekeeper import CONF_FLAMEKEEPER_ID, FlamekeeperHub
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32", "flamekeeper"]
AUTO_LOAD = ["esp32_ble", "esp32_ble_server"]

CONF_FLAMEKEEPER_BLE_ID = "flamekeeper_ble_id"
CONF_MAX_BONDS = "max_bonds"
CONF_PAIRING_WINDOW = "pairing_window"
CONF_OPEN_ON_POWER_UP = "open_on_power_up"

flamekeeper_ble_ns = cg.esphome_ns.namespace("flamekeeper_ble")
FlamekeeperBLE = flamekeeper_ble_ns.class_("FlamekeeperBLE", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FlamekeeperBLE),
        cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub),
        cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Optional(CONF_MAX_BONDS, default=8): cv.int_range(min=1, max=15),
        cv.Optional(CONF_PAIRING_WINDOW, default="2min"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=30), max=cv.TimePeriod(minutes=10)),
        ),
        # Power-up opens the window. Reset reason "power on" only.
        cv.Optional(CONF_OPEN_ON_POWER_UP, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
    cg.add(var.set_max_bonds(config[CONF_MAX_BONDS]))
    cg.add(var.set_window_ms(config[CONF_PAIRING_WINDOW]))
    cg.add(var.set_open_on_power_up(config[CONF_OPEN_ON_POWER_UP]))
    ble = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    esp32_ble.register_gap_event_handler(ble, var)
    esp32_ble.register_gatts_event_handler(ble, var)
    # Bonds and their keys live in NVS (they survive OTA updates); room for max_bonds of them.
    from esphome.components.esp32 import add_idf_sdkconfig_option

    add_idf_sdkconfig_option("CONFIG_BT_SMP_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_SMP_MAX_BONDS", max(config[CONF_MAX_BONDS], 8))
