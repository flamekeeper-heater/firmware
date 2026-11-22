import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FLAMEKEEPER_BLE_ID, FlamekeeperBLE

DEPENDENCIES = ["flamekeeper_ble"]

CONF_PAIRING_OPEN = "pairing_open"

# On while a new phone may pair. Also Improv's authorizer (step 3): Wi-Fi credentials are accepted
# over BLE only inside the same window.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FLAMEKEEPER_BLE_ID): cv.use_id(FlamekeeperBLE),
        cv.Optional(CONF_PAIRING_OPEN): binary_sensor.binary_sensor_schema(
            icon="mdi:bluetooth-connect", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FLAMEKEEPER_BLE_ID])
    if CONF_PAIRING_OPEN in config:
        cg.add(parent.set_pairing_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_PAIRING_OPEN])))
