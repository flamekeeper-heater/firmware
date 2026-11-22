import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

FlamekeeperSelect = flamekeeper_ns.class_("FlamekeeperSelect", select.Select, cg.Component)
SelectKind = flamekeeper_ns.enum("SelectKind", is_class=True)

# Option order = the protocol's codes (heat mode 0 power level / 1 temperature; room source 0 board
# probe / 1 Home Assistant / 2 a Bluetooth thermometer, refused without flamekeeper_thermo).
TYPES = {
    "heat_mode": (SelectKind.HEAT_MODE, ["Power level", "Temperature"], "mdi:tune-variant", None),
    "room_source": (
        SelectKind.ROOM_SOURCE,
        ["Board probe", "Home Assistant", "Bluetooth thermometer"],
        "mdi:home-thermometer",
        ENTITY_CATEGORY_CONFIG,
    ),
}


def _schema(kind):
    _, _, icon, category = TYPES[kind]
    kw = {"icon": icon}
    if category:
        kw["entity_category"] = category
    return (
        select.select_schema(FlamekeeperSelect, **kw)
        .extend({cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)})
        .extend(cv.COMPONENT_SCHEMA)
    )


CONFIG_SCHEMA = cv.typed_schema({k: _schema(k) for k in TYPES}, key=CONF_TYPE)


async def to_code(config):
    kind, options, _, _ = TYPES[config[CONF_TYPE]]
    var = await select.new_select(config, options=options)
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
    cg.add(var.set_kind(kind))
