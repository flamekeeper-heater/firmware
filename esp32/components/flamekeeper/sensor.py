import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_CARBON_MONOXIDE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    DEVICE_CLASS_VOLUME,
)

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

SensorKey = flamekeeper_ns.enum("SensorKey", is_class=True)
DIAG = ENTITY_CATEGORY_DIAGNOSTIC
MEAS = STATE_CLASS_MEASUREMENT


def _s(unit=None, decimals=0, device_class=None, icon=None, state_class=MEAS, category=None):
    kw = {"accuracy_decimals": decimals}
    if state_class:
        kw["state_class"] = state_class
    if unit:
        kw["unit_of_measurement"] = unit
    if device_class:
        kw["device_class"] = device_class
    if icon:
        kw["icon"] = icon
    if category:
        kw["entity_category"] = category
    return sensor.sensor_schema(**kw)


# key in YAML: (SensorKey, schema). A value the heater does not report stays unknown.
SENSORS = {
    "room_temperature": (SensorKey.ROOM_TEMPERATURE, _s("°C", 1, DEVICE_CLASS_TEMPERATURE)),
    "heat_exchanger_temperature": (SensorKey.HEAT_EXCHANGER_TEMPERATURE, _s("°C", 0, DEVICE_CLASS_TEMPERATURE)),
    "supply_voltage": (SensorKey.SUPPLY_VOLTAGE, _s("V", 1, DEVICE_CLASS_VOLTAGE)),
    "fan_speed": (SensorKey.FAN_SPEED, _s("rpm", 0, icon="mdi:fan")),
    "pump_frequency": (SensorKey.PUMP_FREQUENCY, _s("Hz", 1, icon="mdi:pump")),
    "glow_plug_power": (SensorKey.GLOW_PLUG_POWER, _s("W", 0, DEVICE_CLASS_POWER)),
    "glow_plug_voltage": (SensorKey.GLOW_PLUG_VOLTAGE, _s("V", 1, DEVICE_CLASS_VOLTAGE)),
    "fan_voltage": (SensorKey.FAN_VOLTAGE, _s("V", 1, DEVICE_CLASS_VOLTAGE)),
    "glow_plug_current": (SensorKey.GLOW_PLUG_CURRENT, _s("A", 1, DEVICE_CLASS_CURRENT)),
    "power_demand": (SensorKey.POWER_DEMAND, _s("%", 0, icon="mdi:fire")),
    "altitude": (SensorKey.ALTITUDE, _s("m", 0, icon="mdi:image-filter-hdr")),
    "running_time": (
        SensorKey.RUNNING_TIME,
        _s("h", 1, DEVICE_CLASS_DURATION, state_class=STATE_CLASS_TOTAL_INCREASING),
    ),
    "priming_time_left": (SensorKey.PRIMING_TIME_LEFT, _s("s", 0, icon="mdi:timer-sand")),
    "intake_temperature": (SensorKey.INTAKE_TEMPERATURE, _s("°C", 1, DEVICE_CLASS_TEMPERATURE)),
    "carbon_monoxide": (SensorKey.CO, _s("ppm", 0, DEVICE_CLASS_CARBON_MONOXIDE)),
    "settings_pending": (SensorKey.SETTINGS_PENDING, _s(None, 0, icon="mdi:playlist-edit", category=DIAG)),
    "firmware_update_progress": (
        SensorKey.FIRMWARE_UPDATE_PROGRESS,
        _s("%", 0, icon="mdi:progress-upload", category=DIAG),
    ),
    "failsafe_time_left": (SensorKey.FAILSAFE_TIME_LEFT, _s("s", 0, icon="mdi:timer-alert", category=DIAG)),
    # The board's fuel and hour meters (an estimate from the pump's strokes).
    "fuel_left": (SensorKey.FUEL_LEFT, _s("%", 0, icon="mdi:gauge")),
    "fuel_used": (
        SensorKey.FUEL_USED,
        _s("L", 2, DEVICE_CLASS_VOLUME, state_class=STATE_CLASS_TOTAL_INCREASING),
    ),
    "burner_hours": (
        SensorKey.BURNER_HOURS,
        _s("h", 1, DEVICE_CLASS_DURATION, state_class=STATE_CLASS_TOTAL_INCREASING, category=DIAG),
    ),
    "glow_plug_hours": (
        SensorKey.GLOW_PLUG_HOURS,
        _s("h", 2, DEVICE_CLASS_DURATION, state_class=STATE_CLASS_TOTAL_INCREASING, category=DIAG),
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)}
    | {cv.Optional(key): schema for key, (_, schema) in SENSORS.items()}
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FLAMEKEEPER_ID])
    for key, (enum, _) in SENSORS.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.add_sensor(enum, s))
