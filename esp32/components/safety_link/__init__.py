"""The Flamekeeper board's heater backend: the link to the STM32 safety MCU over UART
(DESIGN.md; see safety_link.h)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.components.flamekeeper import Heater
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["uart", "flamekeeper"]

safety_link_ns = cg.esphome_ns.namespace("safety_link")
SafetyLink = safety_link_ns.class_("SafetyLink", cg.Component, uart.UARTDevice, Heater)

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(SafetyLink)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
