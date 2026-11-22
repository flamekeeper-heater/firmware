import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate

from . import CONF_FLAMEKEEPER_ID, FlamekeeperHub, flamekeeper_ns

DEPENDENCIES = ["flamekeeper"]

FlamekeeperClimate = flamekeeper_ns.class_("FlamekeeperClimate", climate.Climate, cg.Component)

CONFIG_SCHEMA = climate.climate_schema(FlamekeeperClimate).extend(
    {cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub)}
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
