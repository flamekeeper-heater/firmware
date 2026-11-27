# ST's and ARM's headers for the STM32G030

Copied unchanged, so the driver's registers and bit names are ST's own rather than a hand-written
map. Each file keeps its copyright and licence notice; both projects are Apache-2.0
(`LICENSE-ST.md`, `LICENSE-CMSIS.txt`), which the firmware's GPL-3.0 can include.

| File | From | Version | SHA-256 |
|---|---|---|---|
| `stm32g030xx.h` | [STMicroelectronics/cmsis-device-g0](https://github.com/STMicroelectronics/cmsis-device-g0) `Include/` | v1.4.5 (f576c24) | `9af2b7c64ebe6cbe486f8a5672c7e6f2f08c23cd5d24962c138205b1a49b833b` |
| `system_stm32g0xx.h` | same | v1.4.5 | `20ee999e4431472a7c351e2412537aa0ef7f5f5e499a21a92c1d4cc21f84ebf1` |
| `LICENSE-ST.md` | same, `LICENSE.md` | v1.4.5 | |
| `core_cm0plus.h` | [ARM-software/CMSIS_5](https://github.com/ARM-software/CMSIS_5) `CMSIS/Core/Include/` | 5.9.0 (2b7495b) | `a09fa9c6b8b21d1920bce15b98aa56182b027e9278cca0b154812bac2aea3cfa` |
| `cmsis_compiler.h` | same | 5.9.0 | `b51963d271c1571ca3463654c76aa7ea50c4eac5d0a2710df10414ac64d9e0f8` |
| `cmsis_gcc.h` | same | 5.9.0 | `43bfd1fe69fbbc2ca70aaf7d43cc3e20f16e0c7f66e1bdef9b51af6dc14b2617` |
| `cmsis_version.h` | same | 5.9.0 | `184c19fd3ee73632edf35a0b4d49cd48be75fbf49e6ccb19d9db05fa83bea4b3` |
| `mpu_armv7.h` | same | 5.9.0 | `29206b52ee02290ed6f5a5415ebd4187de802cf176d9b3cb844390d8e5571372` |
| `LICENSE-CMSIS.txt` | same, `LICENSE.txt` | 5.9.0 | |

To update: copy the newer files over these, rebuild (`stm32/build.sh`, `stm32/build.sh bringup`)
and run `stm32/tests/run.sh`. Only `stm32g030xx.h` and the core header shape the code; what they
leave out is in `../g030_fields.h`.
