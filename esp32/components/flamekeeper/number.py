import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_STEP,
    CONF_TYPE,
    ENTITY_CATEGORY_CONFIG,
    UNIT_PERCENT,
)

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

CONF_PARAMETER = "parameter"
CONF_MULTIPLY = "multiply"

# "Power" 0-100 %: the fixed power demand (0 % = the low end of the heater's range, 100 % = its
# maximum), and the fan speed in fan-only mode.
PowerNumber = flamekeeper_ns.class_("PowerNumber", number.Number, cg.Component)
# A parameter by id: the heater's (through the link; staged until Apply / Save) or the board's own
# (tuning.h, ids from 0x100; applied at once). The value shown is the raw value / multiply.
ParamNumber = flamekeeper_ns.class_("ParamNumber", number.Number, cg.Component)

_HUB = {cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)}

CONFIG_SCHEMA = cv.typed_schema(
    {
        "power": number.number_schema(PowerNumber, icon="mdi:fire", unit_of_measurement=UNIT_PERCENT)
        .extend(_HUB)
        .extend(cv.COMPONENT_SCHEMA),
        "parameter": number.number_schema(ParamNumber, entity_category=ENTITY_CATEGORY_CONFIG)
        .extend(_HUB)
        .extend(
            {
                cv.Required(CONF_PARAMETER): cv.int_range(min=1, max=0xFFFF),
                cv.Optional(CONF_MULTIPLY, default=1.0): cv.positive_float,
                cv.Required(CONF_MIN_VALUE): cv.float_,
                cv.Required(CONF_MAX_VALUE): cv.float_,
                cv.Optional(CONF_STEP, default=1.0): cv.positive_float,
            }
        )
        .extend(cv.COMPONENT_SCHEMA),
    },
    key=CONF_TYPE,
    default_type="power",
)


async def to_code(config):
    if config[CONF_TYPE] == "power":
        var = await number.new_number(config, min_value=0, max_value=100, step=1)
    else:
        var = await number.new_number(
            config, min_value=config[CONF_MIN_VALUE], max_value=config[CONF_MAX_VALUE], step=config[CONF_STEP]
        )
        cg.add(var.set_param(config[CONF_PARAMETER], config[CONF_MULTIPLY]))
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
