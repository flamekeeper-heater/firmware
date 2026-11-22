import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

TextKey = flamekeeper_ns.enum("TextKey", is_class=True)
DIAG = ENTITY_CATEGORY_DIAGNOSTIC

TEXTS = {
    "run_state": (TextKey.RUN_STATE, "mdi:fire-circle", None),
    "controller_status": (TextKey.CONTROLLER_STATUS, "mdi:remote", None),
    "error": (TextKey.ERROR, "mdi:alert-circle-outline", None),
    "run_started_by": (TextKey.RUN_STARTED_BY, "mdi:account-arrow-right", None),
    "alerts": (TextKey.ALERTS, "mdi:bell-alert", None),
    "heater_firmware": (TextKey.HEATER_FIRMWARE, "mdi:chip", DIAG),
    "settings_status": (TextKey.SETTINGS_STATUS, "mdi:playlist-check", DIAG),
    "firmware_update_status": (TextKey.FIRMWARE_UPDATE_STATUS, "mdi:progress-upload", DIAG),
    "relay": (TextKey.RELAY, "mdi:cloud-outline", DIAG),
}


def _schema(icon, category):
    kw = {"icon": icon}
    if category:
        kw["entity_category"] = category
    return text_sensor.text_sensor_schema(**kw)


CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)}
    | {cv.Optional(key): _schema(icon, cat) for key, (_, icon, cat) in TEXTS.items()}
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FLAMEKEEPER_ID])
    for key, (enum, _, _) in TEXTS.items():
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(hub.add_text_sensor(enum, s))
