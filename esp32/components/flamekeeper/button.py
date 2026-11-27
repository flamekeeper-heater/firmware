import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

FlamekeeperButton = flamekeeper_ns.class_("FlamekeeperButton", button.Button, cg.Component)
ButtonKind = flamekeeper_ns.enum("ButtonKind", is_class=True)

TYPES = {
    # Fuel priming with the heater off. Use it only when you are at the heater.
    "prime": (ButtonKind.PRIME, "mdi:gas-station", None),
    "stop_prime": (ButtonKind.STOP_PRIME, "mdi:gas-station-off", None),
    # Heater parameters: staged by the app or the set_parameter action, sent on Apply / Save.
    "save_settings": (ButtonKind.SAVE_SETTINGS, "mdi:content-save", ENTITY_CATEGORY_CONFIG),
    "discard_settings": (ButtonKind.DISCARD_SETTINGS, "mdi:undo", ENTITY_CATEGORY_CONFIG),
    # The heater's own firmware (the safety MCU). Refused unless it is off.
    "firmware_update": (ButtonKind.FIRMWARE_UPDATE, "mdi:chip", ENTITY_CATEGORY_CONFIG),
    # Clears a fault the safety MCU latched until a user reset, once its cause is gone.
    "reset_fault": (ButtonKind.RESET_FAULT, "mdi:restore-alert", None),
    "tank_filled": (ButtonKind.TANK_FILLED, "mdi:gas-station", None),
    "rearm_automatic": (ButtonKind.REARM_AUTOMATIC, "mdi:timer-refresh", None),
    "clear_alerts": (ButtonKind.CLEAR_ALERTS, "mdi:bell-cancel", None),
    "carbon_burnoff": (ButtonKind.CARBON_BURNOFF, "mdi:fire-alert", None),
    "glow_new_plug": (ButtonKind.GLOW_NEW_PLUG, "mdi:lightbulb-on-outline", ENTITY_CATEGORY_CONFIG),
    # Empties the heater's fault log (the safety MCU's).
    "clear_heater_fault_log": (ButtonKind.CLEAR_HEATER_FAULT_LOG, "mdi:delete-sweep-outline", ENTITY_CATEGORY_CONFIG),
}


def _schema(kind):
    _, icon, category = TYPES[kind]
    kw = {"icon": icon}
    if category:
        kw["entity_category"] = category
    return (
        button.button_schema(FlamekeeperButton, **kw)
        .extend({cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)})
        .extend(cv.COMPONENT_SCHEMA)
    )


CONFIG_SCHEMA = cv.typed_schema({k: _schema(k) for k in TYPES}, key=CONF_TYPE)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
    cg.add(var.set_kind(TYPES[config[CONF_TYPE]][0]))
