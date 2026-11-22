import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_FLAMEKEEPER_BLE_ID, FlamekeeperBLE, flamekeeper_ble_ns

DEPENDENCIES = ["flamekeeper_ble"]

FlamekeeperBLEButton = flamekeeper_ble_ns.class_("FlamekeeperBLEButton", button.Button, cg.Component)

# Forgetting every phone is the board's factory reset (BOOT held 10 s), not a button.
TYPES = {
    "allow_pairing": "mdi:bluetooth-connect",
}


def _schema(icon):
    return (
        button.button_schema(FlamekeeperBLEButton, icon=icon, entity_category=ENTITY_CATEGORY_CONFIG)
        .extend({cv.GenerateID(CONF_FLAMEKEEPER_BLE_ID): cv.use_id(FlamekeeperBLE)})
        .extend(cv.COMPONENT_SCHEMA)
    )


CONFIG_SCHEMA = cv.typed_schema({k: _schema(icon) for k, icon in TYPES.items()}, key=CONF_TYPE)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(await cg.get_variable(config[CONF_FLAMEKEEPER_BLE_ID])))
