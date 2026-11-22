#!/bin/sh
# Builds the safety MCU's firmware for the STM32G030K8 with arm-none-eabi-gcc: build/safety.elf,
# build/safety.bin, and the flash and RAM use. No vendor HAL, no C library.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out="$here/build"
mkdir -p "$out"
cflags="-mcpu=cortex-m0plus -mthumb -Os -std=c99 -ffreestanding -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -I$here/hal/include"
srcs="$here/core/burn.c $here/core/glow.c $here/core/fan.c $here/core/checks.c $here/core/pump.c \
  $here/core/params.c $here/core/plug.c $here/core/safety.c $here/../common/link/link.c \
  $here/board/analog.c $here/board/board.c $here/board/pump_oc.c \
  $here/hal/hal_stm32g0.c $here/hal/startup.c $here/hal/libc_min.c $here/hal/main.c"
arm-none-eabi-gcc $cflags $srcs -nostdlib -Wl,--gc-sections -Wl,-T,"$here/hal/stm32g030k8.ld" \
  -Wl,-Map,"$out/safety.map" -lgcc -o "$out/safety.elf"
arm-none-eabi-objcopy -O binary "$out/safety.elf" "$out/safety.bin"
arm-none-eabi-size "$out/safety.elf"
