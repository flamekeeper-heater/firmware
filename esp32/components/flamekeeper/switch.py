import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

CONF_PARAMETER = "parameter"

# An on/off parameter by id, the heater's or the board's own (1 on, 0 off). Not restored: it shows
# what the heater (or the stored tuning) holds.
ParamSwitch = flamekeeper_ns.class_("ParamSwitch", switch.Switch, cg.Component)

_HUB = {cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)}

CONFIG_SCHEMA = cv.typed_schema(
    {
        "parameter": switch.switch_schema(
            ParamSwitch, entity_category=ENTITY_CATEGORY_CONFIG, default_restore_mode="DISABLED"
        )
        .extend(_HUB)
        .extend({cv.Required(CONF_PARAMETER): cv.int_range(min=1, max=0xFFFF)})
        .extend(cv.COMPONENT_SCHEMA),
    },
    key=CONF_TYPE,
    default_type="parameter",
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
    cg.add(var.set_param(config[CONF_PARAMETER]))
