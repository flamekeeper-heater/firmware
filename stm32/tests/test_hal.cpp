// The STM32 driver's host-testable decisions (hal/hal_stm32g0.h): the option-byte check (WDG-1)
// against the ESP32 updater's own rule for the option bytes it writes (stm32_boot.h).
#include <cstdio>

// The ESP32's header first: the register macros in ST's device header would shadow its names.
#include "../../esp32/components/flamekeeper/stm32_boot.h"
#include "../hal/hal_stm32g0.h"
#include "../../common/test/check.h"

namespace g030 = esphome::flamekeeper::g030;


// The bits match the map both sides share (common/g030/g030_map.h).
static void test_optr_bits() {
  CHECK(FLASH_OPTR_IWDG_SW == G030_OPTR_IWDG_SW);
  CHECK(FLASH_OPTR_IWDG_STOP == G030_OPTR_IWDG_STOP);
  CHECK(FLASH_OPTR_IWDG_STDBY == G030_OPTR_IWDG_STDBY);
  CHECK(FLASH_OPTR_nBOOT_SEL == G030_OPTR_NBOOT_SEL);
  CHECK(FLASH_OPTR_nBOOT1 == G030_OPTR_NBOOT1);
  CHECK(FLASH_OPTR_nBOOT0 == G030_OPTR_NBOOT0);
}

// The MCU accepts exactly what the updater would leave alone, over every combination of the six
// bits and a few values of the rest; OPTVERR fails it whatever the bits.
static void test_optr_matches_updater() {
  const uint32_t six = FLASH_OPTR_IWDG_SW | FLASH_OPTR_IWDG_STOP | FLASH_OPTR_IWDG_STDBY | FLASH_OPTR_nBOOT_SEL |
                       FLASH_OPTR_nBOOT1 | FLASH_OPTR_nBOOT0;
  const uint32_t rest[] = {0x00000000u, 0xDFFFE1AAu & ~six, 0xFFFFFFFFu & ~six, 0x000000BBu};
  int accepted = 0;
  for (uint32_t r : rest) {
    for (uint32_t k = 0; k < 64; k++) {
      uint32_t optr = r;
      for (int b = 0, bit = 0; b < 32; b++)
        if (six & (1u << b))
          optr |= ((k >> bit++) & 1u) << b;
      bool as_required = g030::optr_required(optr) == optr;
      CHECK(hal_optr_wdg1_ok(optr, 0) == as_required);
      CHECK(!hal_optr_wdg1_ok(optr, FLASH_SR_OPTVERR));
      accepted += hal_optr_wdg1_ok(optr, 0);
    }
  }
  CHECK(accepted == 4);  // one combination of the six, whatever the rest
}

// The bring-up tests' values: the factory option bytes (nBOOT_SEL 1, IWDG_SW 1) fail, the
// updater's result from them passes, and any one of the six bits wrong fails.
static void test_optr_examples() {
  const uint32_t factory = 0xDFFFE1AAu, ok = 0xDEFEE1AAu;
  CHECK(!hal_optr_wdg1_ok(factory, 0));
  CHECK(hal_optr_wdg1_ok(ok, 0));
  CHECK(g030::optr_required(factory) == ok);
  CHECK(!hal_optr_wdg1_ok(factory & ~FLASH_OPTR_nBOOT_SEL, 0));  // IWDG_SW still 1
  CHECK(!hal_optr_wdg1_ok(factory & ~FLASH_OPTR_IWDG_SW, 0));    // nBOOT_SEL still 1
  CHECK(!hal_optr_wdg1_ok(ok & ~FLASH_OPTR_nBOOT1, 0));          // BOOT0 high would boot SRAM
  CHECK(!hal_optr_wdg1_ok(ok & ~FLASH_OPTR_nBOOT0, 0));
  CHECK(!hal_optr_wdg1_ok(ok & ~FLASH_OPTR_IWDG_STOP, 0));
  CHECK(!hal_optr_wdg1_ok(ok & ~FLASH_OPTR_IWDG_STDBY, 0));
  CHECK(hal_optr_wdg1_ok(ok, FLASH_SR_EOP));  // other FLASH_SR flags do not matter
}

int main() {
  test_optr_bits();
  test_optr_matches_updater();
  test_optr_examples();
  return check_report("hal");
}
