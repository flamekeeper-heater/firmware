"""The Flamekeeper board's heater backend: the link to the STM32 safety MCU over UART
(DESIGN.md; see safety_link.h).

With reset_pin and boot_pin (STM_NRST, STM_BOOT) and the safety MCU's image (mcu_firmware), the
board can update the safety MCU through its ROM bootloader ("Update heater firmware").

The image is stm32/build/safety.bin, built by stm32/build.sh (the release tools build it first). The
build refuses an image that is missing, older than its sources, has no vector table, or reports a
version other than the one expected (SAFETY_VERSION_* in stm32/core/safety.h, or "version:")."""

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
FLASH_BASE = 0x08000000
RAM_BASE, RAM_END = 0x20000000, 0x20002000  # 8 KB

# The safety MCU firmware's version, as its HELLO reports it, from this repository's source.
REPO = Path(__file__).resolve().parents[3]
SAFETY_H = REPO / "stm32" / "core" / "safety.h"
# What stm32/build.sh builds safety.bin from (the tests' folders aside), and where it puts it.
SAFETY_BIN = REPO / "stm32" / "build" / "safety.bin"
SAFETY_SOURCES = [REPO / "stm32" / d for d in ("core", "board", "hal")] + [REPO / "common"]
SAFETY_SUFFIXES = {".c", ".h", ".ld", ".s", ".S"}
# The HELLO text (stm32/core/safety.c), in the image's read-only data. The bring-up image's says
# "Flamekeeper bring-up", so it never matches.
HELLO_TEXT = re.compile(rb"Flamekeeper safety (\d{1,3}\.\d{1,3}\.\d{1,3})\0")
BUILD_IT = "build it from this checkout first: stm32/build.sh"

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


def _image_file(value):
    try:
        return cv.file_(value)
    except cv.Invalid as err:
        raise cv.Invalid(f"no safety MCU image at {value}: {BUILD_IT}") from err


def _newest_source():
    """The newest source of stm32/build/safety.bin, as (mtime, path)."""
    build_sh = REPO / "stm32" / "build.sh"
    newest = (build_sh.stat().st_mtime, build_sh)
    for top in SAFETY_SOURCES:
        for path in top.rglob("*"):
            in_tests = {"tests", "test"} & set(path.relative_to(top).parts)
            if path.suffix in SAFETY_SUFFIXES and not in_tests:
                newest = max(newest, (path.stat().st_mtime, path))
    return newest


def _check_image(config):
    """Refuses an image the safety MCU should not be given: see the module's description."""
    path = Path(config[CONF_FILE]).resolve()
    data = path.read_bytes()
    if not 8 <= len(data) <= APP_SIZE:
        raise cv.Invalid(f"{path}: {len(data)} bytes, the safety MCU takes 8 to {APP_SIZE}")
    # The vector table, as the updater checks it (mcu_update.cpp): a stack in RAM and a Thumb reset
    # handler inside the image.
    sp, pc = int.from_bytes(data[0:4], "little"), int.from_bytes(data[4:8], "little")
    if not (RAM_BASE < sp <= RAM_END and pc & 1 and FLASH_BASE + 8 <= pc < FLASH_BASE + len(data)):
        raise cv.Invalid(
            f"{path} has no STM32G030 vector table (stack 0x{sp:08x}, reset 0x{pc:08x}): "
            "is it safety.bin?"
        )
    found = {m.group(1).decode() for m in HELLO_TEXT.finditer(data)}
    if len(found) != 1:
        raise cv.Invalid(
            f"{path} does not say which safety firmware version it is: is it safety.bin (not "
            "bringup.bin)?"
        )
    built = found.pop()
    if built != config[CONF_VERSION]:
        raise cv.Invalid(
            f"{path} is version {built}, but {config[CONF_VERSION]} is expected (version:, or "
            f"SAFETY_VERSION_* in stm32/core/safety.h); {BUILD_IT}"
        )
    # This checkout's own image: not older than anything it is built from.
    if path == SAFETY_BIN:
        mtime, newest = _newest_source()
        if mtime > path.stat().st_mtime:
            raise cv.Invalid(f"{path} is older than {newest.relative_to(REPO)}: {BUILD_IT}")
    return config


MCU_FIRMWARE_SCHEMA = cv.All(
    cv.Schema(
        {
            # build/safety.bin from stm32/build.sh, and the version its HELLO reports (by default
            # SAFETY_VERSION_* in stm32/core/safety.h, the source it was built from).
            cv.Required(CONF_FILE): _image_file,
            cv.Optional(CONF_VERSION): _version,
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
        }
    ),
    _default_version,
    _check_image,
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
        data = Path(fw[CONF_FILE]).read_bytes()  # checked by _check_image
        image = cg.progmem_array(fw[CONF_RAW_DATA_ID], [HexInt(b) for b in data])
        major, minor, patch = (int(p) for p in fw[CONF_VERSION].split("."))
        cg.add(var.set_mcu_image(image, len(data), major, minor, patch))
