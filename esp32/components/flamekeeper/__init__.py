"""Flamekeeper hub: the shared controller in front of a heater backend, and the shared Home Assistant
entities (climate, switch, select, number, button, sensor and text_sensor platforms of this
component). Every board profile configures it the same way; only `heater_id` names the backend
(safety_link on the Flamekeeper board).

The controller, the BLE codec and the pairing window are plain C++ host-tested in tests/run.sh.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.components import time as time_
from esphome.const import CONF_ID

CODEOWNERS = []
AUTO_LOAD = ["sensor", "text_sensor"]
MULTI_CONF = False

CONF_FLAMEKEEPER_ID = "flamekeeper_id"
CONF_HEATER_ID = "heater_id"
CONF_ROOM_PROBE = "room_probe"
CONF_ROOM_HA = "room_home_assistant"
CONF_ALTITUDE = "altitude"
CONF_PRESSURE = "pressure"
CONF_TIME_ID = "time_id"
CONF_INTAKE_TEMPERATURE = "intake_temperature"
CONF_FAILSAFE_TIMEOUT = "failsafe_timeout"
CONF_BOARD = "board"
CONF_PIN = "pin"
CONF_PIN_EFUSE_KEY = "pin_efuse_key"
CONF_HOME_ASSISTANT = "home_assistant"

# Info byte 2 (PROTOCOL.md).
BOARDS = {"flamekeeper_rev_a2": 2}

flamekeeper_ns = cg.esphome_ns.namespace("flamekeeper")
Heater = flamekeeper_ns.class_("Heater")
FlamekeeperHub = flamekeeper_ns.class_("FlamekeeperHub", cg.Component)
HaMode = flamekeeper_ns.enum("HaMode", is_class=True)
HA_MODES = {"always_on": HaMode.HA_ALWAYS_ON, "from_app": HaMode.HA_FROM_APP}


def _pin(value):
    value = cv.string_strict(value)
    if len(value) != 6 or not value.isdigit():
        raise cv.Invalid("the PIN has exactly six digits")
    return int(value)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FlamekeeperHub),
        # The heater backend: any component implementing flamekeeper::Heater.
        cv.Required(CONF_HEATER_ID): cv.use_id(Heater),
        # Room temperature: the board's probe and a Home Assistant sensor; "Room temperature
        # source" picks one, the other is the fallback.
        cv.Optional(CONF_ROOM_PROBE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ROOM_HA): cv.use_id(sensor.Sensor),
        # Board sensors passed on to the heater (altitude fuel trim) and shown in the status.
        cv.Optional(CONF_ALTITUDE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_PRESSURE): cv.use_id(sensor.Sensor),  # hPa, shown in the apps
        cv.Optional(CONF_INTAKE_TEMPERATURE): cv.use_id(sensor.Sensor),
        # A time source (Home Assistant's) for the board's clock, next to the apps' (timers, meters).
        cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        # A run started from Home Assistant stops when Home Assistant has been unreachable this
        # long. 0s turns the failsafe off.
        cv.Optional(CONF_FAILSAFE_TIMEOUT, default="15min"): cv.positive_time_period_milliseconds,
        # Info byte 2, the board the apps are talking to: a name above, or a code for another board.
        cv.Required(CONF_BOARD): cv.Any(cv.one_of(*BOARDS, lower=True), cv.int_range(min=1, max=255)),
        # The PIN (Bluetooth passkey, PROTOCOL.md): a fixed one (from
        # secrets.yaml), or derived from the board key in this eFuse key block (the Flamekeeper
        # board, burned at production). With neither, a random PIN kept on the board.
        cv.Exclusive(CONF_PIN, "pin"): _pin,
        cv.Exclusive(CONF_PIN_EFUSE_KEY, "pin"): cv.int_range(min=0, max=5),
        # Home Assistant always on, or off until switched on from the app.
        cv.Optional(CONF_HOME_ASSISTANT, default="always_on"): cv.enum(HA_MODES, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    heater = await cg.get_variable(config[CONF_HEATER_ID])
    cg.add(var.set_heater(heater))
    await cg.register_component(var, config)
    cg.add(var.set_failsafe_timeout(config[CONF_FAILSAFE_TIMEOUT]))
    board = var.board()
    cg.add(board.set_board(BOARDS.get(config[CONF_BOARD], config[CONF_BOARD])))
    if CONF_PIN in config:
        cg.add(board.set_fixed_pin(config[CONF_PIN]))
    if CONF_PIN_EFUSE_KEY in config:
        cg.add(board.set_pin_efuse_key(config[CONF_PIN_EFUSE_KEY]))
    cg.add(board.set_ha_mode(config[CONF_HOME_ASSISTANT]))
    # The Wi-Fi sign-in (wifi_session.cpp) runs EC J-PAKE, which ESP-IDF's mbedTLS leaves out.
    from esphome.components.esp32 import add_idf_sdkconfig_option

    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECJPAKE_C", True)
    if CONF_ROOM_PROBE in config:
        cg.add(var.set_room_probe(await cg.get_variable(config[CONF_ROOM_PROBE])))
    if CONF_ROOM_HA in config:
        cg.add(var.set_room_home_assistant(await cg.get_variable(config[CONF_ROOM_HA])))
    if CONF_ALTITUDE in config:
        cg.add(var.set_altitude_sensor(await cg.get_variable(config[CONF_ALTITUDE])))
    if CONF_PRESSURE in config:
        cg.add(var.set_pressure_sensor(await cg.get_variable(config[CONF_PRESSURE])))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    if CONF_INTAKE_TEMPERATURE in config:
        cg.add(var.set_intake_sensor(await cg.get_variable(config[CONF_INTAKE_TEMPERATURE])))
