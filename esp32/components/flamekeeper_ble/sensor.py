import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FLAMEKEEPER_BLE_ID, FlamekeeperBLE

DEPENDENCIES = ["flamekeeper_ble"]

CONF_PAIRED_PHONES = "paired_phones"
CONF_PAIRING_TIME_LEFT = "pairing_time_left"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FLAMEKEEPER_BLE_ID): cv.use_id(FlamekeeperBLE),
        cv.Optional(CONF_PAIRED_PHONES): sensor.sensor_schema(
            icon="mdi:cellphone-link", accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_PAIRING_TIME_LEFT): sensor.sensor_schema(
            icon="mdi:bluetooth-connect",
            unit_of_measurement="s",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FLAMEKEEPER_BLE_ID])
    if CONF_PAIRED_PHONES in config:
        cg.add(parent.set_bonds_sensor(await sensor.new_sensor(config[CONF_PAIRED_PHONES])))
    if CONF_PAIRING_TIME_LEFT in config:
        cg.add(parent.set_window_sensor(await sensor.new_sensor(config[CONF_PAIRING_TIME_LEFT])))
