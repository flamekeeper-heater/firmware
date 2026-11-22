"""The Flamekeeper board's heater backend: the link to the STM32 safety MCU over UART
(DESIGN.md; see safety_link.h).

With reset_pin and boot_pin (STM_NRST, STM_BOOT) and the safety MCU's image (mcu_firmware), the
board can update the safety MCU through its ROM bootloader ("Update heater firmware")."""

from pathlib import Path
import re

from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.components.flamekeeper import Heater
from esphome.const import CONF_FILE, CONF_ID, CONF_RAW_DATA_ID, CONF_RESET_PIN, CONF_VERSION
from esphome.core import HexInt

CODEOWNERS = []
DEPENDENCIES = ["uart", "flamekeeper"]

CONF_BOOT_PIN = "boot_pin"
CONF_MCU_FIRMWARE = "mcu_firmware"

# The STM32G030K8's application flash: pages 30 and 31 hold its saved data.
APP_SIZE = 30 * 2048

# The safety MCU firmware's version, as its HELLO reports it, from this repository's source.
SAFETY_H = Path(__file__).resolve().parents[3] / "stm32" / "core" / "safety.h"

safety_link_ns = cg.esphome_ns.namespace("safety_link")
SafetyLink = safety_link_ns.class_("SafetyLink", cg.Component, uart.UARTDevice, Heater)


def _version(value):
    value = cv.version_number(value)
    parts = [int(p) for p in value.split(".")]
    if len(parts) != 3 or any(p > 255 for p in parts):
        raise cv.Invalid("the version is major.minor.patch, each 0-255")
    return value


def _source_version():
    try:
        text = SAFETY_H.read_text(encoding="utf-8")
    except OSError as err:
        raise cv.Invalid(f"no version given, and stm32/core/safety.h cannot be read ({err})") from err
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"^#define SAFETY_VERSION_{name}\s+(\d+)\s*$", text, re.MULTILINE)
        if m is None:
            raise cv.Invalid(f"no version given, and stm32/core/safety.h has no SAFETY_VERSION_{name}")
        parts.append(m.group(1))
    return _version(".".join(parts))


def _default_version(config):
    if CONF_VERSION not in config:
        config = config.copy()
        config[CONF_VERSION] = _source_version()
    return config


MCU_FIRMWARE_SCHEMA = cv.All(
    cv.Schema(
        {
            # build/safety.bin from stm32/build.sh, and the version its HELLO reports (by default
            # SAFETY_VERSION_* in stm32/core/safety.h, the source it was built from).
            cv.Required(CONF_FILE): cv.file_,
            cv.Optional(CONF_VERSION): _version,
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
        }
    ),
    _default_version,
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SafetyLink),
            # Inputs, no pull, until an update drives them (only after UPDATE_PREPARE).
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_BOOT_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_MCU_FIRMWARE): MCU_FIRMWARE_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    cv.has_none_or_all_keys(CONF_RESET_PIN, CONF_BOOT_PIN),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(config[CONF_RESET_PIN])))
        cg.add(var.set_boot_pin(await cg.gpio_pin_expression(config[CONF_BOOT_PIN])))
    if fw := config.get(CONF_MCU_FIRMWARE):
        with open(fw[CONF_FILE], "rb") as f:
            data = f.read()
        if not 8 <= len(data) <= APP_SIZE:
            raise cv.Invalid(f"{fw[CONF_FILE]}: {len(data)} bytes, the safety MCU takes 8 to {APP_SIZE}")
        image = cg.progmem_array(fw[CONF_RAW_DATA_ID], [HexInt(b) for b in data])
        major, minor, patch = (int(p) for p in fw[CONF_VERSION].split("."))
        cg.add(var.set_mcu_image(image, len(data), major, minor, patch))
