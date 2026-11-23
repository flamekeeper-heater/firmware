#!/bin/sh
# Builds the safety MCU's firmware for the STM32G030K8 with arm-none-eabi-gcc: build/safety.elf,
# build/safety.bin, and the flash and RAM use. No vendor HAL, no C library.
#
# `build.sh bringup` builds the bring-up image instead (bringup/: the same firmware with the text
# console and the bench actions): build/bringup.elf and .bin.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out="$here/build"
mkdir -p "$out"
cflags="-mcpu=cortex-m0plus -mthumb -Os -std=c99 -ffreestanding -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -I$here/hal/include"
core="$here/core/burn.c $here/core/glow.c $here/core/fan.c $here/core/checks.c $here/core/pump.c \
  $here/core/params.c $here/core/plug.c $here/core/monitor.c $here/core/safety.c $here/../common/link/link.c \
  $here/../common/co/co_alarm.c \
  $here/board/analog.c $here/board/board.c $here/board/pump_oc.c"
chip="$here/hal/startup.c $here/hal/libc_min.c"
ld="-nostdlib -Wl,--gc-sections -Wl,-T,$here/hal/stm32g030k8.ld -lgcc"

if [ "${1:-}" = bringup ]; then
  # The driver's UART read, watchdog refresh, glow readings, pump cut arming and CO heater pin go
  # to the console's versions, which watch them (console.h).
  arm-none-eabi-gcc $cflags -DSAFETY_BENCH -Dhal_uart_getc=bringup_chip_uart_getc \
    -Dhal_iwdg_refresh=bringup_chip_iwdg_refresh -Dhal_glow_period=bringup_chip_glow_period \
    -Dhal_pump_oc_arm=bringup_chip_pump_oc_arm -Dhal_co_heater=bringup_chip_co_heater \
    -c "$here/hal/hal_stm32g0.c" -o "$out/bringup_hal.o"
  arm-none-eabi-gcc $cflags -DSAFETY_BENCH $core $chip "$out/bringup_hal.o" "$here/bringup/bench.c" \
    "$here/bringup/console.c" "$here/bringup/main.c" $ld -Wl,-Map,"$out/bringup.map" -o "$out/bringup.elf"
  arm-none-eabi-objcopy -O binary "$out/bringup.elf" "$out/bringup.bin"
  arm-none-eabi-size "$out/bringup.elf"
  exit 0
fi

arm-none-eabi-gcc $cflags $core $chip $here/hal/hal_stm32g0.c $here/hal/main.c $ld \
  -Wl,-Map,"$out/safety.map" -o "$out/safety.elf"
arm-none-eabi-objcopy -O binary "$out/safety.elf" "$out/safety.bin"
arm-none-eabi-size "$out/safety.elf"
