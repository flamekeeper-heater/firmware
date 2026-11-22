/* The STM32G030K8 facts both firmware projects rely on: the flash map the safety MCU's firmware is
 * linked for (stm32/hal/stm32g030k8.ld) and the ESP32's updater writes through the ROM bootloader,
 * the chip id, and the option bytes (RM0454, FLASH_OPTR). Header-only, plain C99 and C++.
 *
 * The ESP32 includes it through a symlink in esp32/components/flamekeeper/. */
#ifndef FLAMEKEEPER_G030_MAP_H
#define FLAMEKEEPER_G030_MAP_H

#include <stdint.h>

/* ---------------------------------------------------------------- memory */

#define G030_FLASH_BASE 0x08000000u
#define G030_PAGE_SIZE 2048u
#define G030_PAGES 32u      /* 64 KB */
#define G030_APP_PAGES 30u  /* the application: pages 0 to 29 */
/* Pages 30 and 31: the saved parameters and the plug record. Never erased or written by an update. */
#define G030_SAVED_PAGE 30u
#define G030_APP_END (G030_FLASH_BASE + G030_APP_PAGES * G030_PAGE_SIZE) /* 0x0800F000 */
#define G030_RAM_BASE 0x20000000u
#define G030_RAM_END 0x20002000u /* 8 KB */
#define G030_CHIP_ID 0x466u      /* the product id (DEV_ID; the bootloader's Get ID) */

/* ---------------------------------------------------------------- option bytes */

/* Double words from here: the value in the low word, its complement in the high word. The first
 * is FLASH_OPTR. */
#define G030_OPTION_BASE 0x1FFF7800u

/* FLASH_OPTR */
#define G030_OPTR_RDP 0xFFu         /* read-out protection: */
#define G030_RDP_LEVEL_0 0xAAu      /* ...0xAA is level 0, none */
#define G030_OPTR_NRST_STOP (1u << 13)   /* 0: entering stop mode resets */
#define G030_OPTR_NRST_STDBY (1u << 14)  /* 0: entering standby resets */
#define G030_OPTR_IWDG_SW (1u << 16)     /* 0: the IWDG started by hardware at reset */
#define G030_OPTR_IWDG_STOP (1u << 17)   /* 0: the IWDG frozen in stop mode */
#define G030_OPTR_IWDG_STDBY (1u << 18)  /* 0: the IWDG frozen in standby */
#define G030_OPTR_WWDG_SW (1u << 19)     /* 0: the WWDG started by hardware at reset */
#define G030_OPTR_NBOOT_SEL (1u << 24)   /* 0: BOOT0 from the pin, 1: from nBOOT0 */
#define G030_OPTR_NBOOT1 (1u << 25)      /* with BOOT0 high, 1: the system memory (bootloader) */
#define G030_OPTR_NBOOT0 (1u << 26)      /* BOOT0 when nBOOT_SEL is 1 (inverted) */

/* WDG-1: the IWDG in hardware and running in stop and standby, BOOT0 from the pin with main flash
 * by default (the ESP32 can still enter the bootloader). The updater clears and sets these bits and
 * keeps every other one (RDP included). */
#define G030_OPTR_WDG1_CLEAR (G030_OPTR_IWDG_SW | G030_OPTR_NBOOT_SEL)
#define G030_OPTR_WDG1_SET (G030_OPTR_IWDG_STOP | G030_OPTR_IWDG_STDBY | G030_OPTR_NBOOT1 | G030_OPTR_NBOOT0)

static inline uint32_t g030_optr_required(uint32_t optr) {
  return (optr & ~(uint32_t) G030_OPTR_WDG1_CLEAR) | (uint32_t) G030_OPTR_WDG1_SET;
}

#endif /* FLAMEKEEPER_G030_MAP_H */
