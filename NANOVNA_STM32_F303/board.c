/*
    ChibiOS - Copyright (C) 2006..2015 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "hal.h"

#if HAL_USE_PAL || defined(__DOXYGEN__)
/**
 * @brief   PAL setup.
 * @details Digital I/O ports static configuration as defined in @p board.h.
 *          This variable is used by the HAL when initializing the PAL driver.
 */
const PALConfig pal_default_config = {
#if STM32_HAS_GPIOA
  {VAL_GPIOA_MODER, VAL_GPIOA_OTYPER, VAL_GPIOA_OSPEEDR, VAL_GPIOA_PUPDR,
   VAL_GPIOA_ODR,   VAL_GPIOA_AFRL,   VAL_GPIOA_AFRH},
#endif
#if STM32_HAS_GPIOB
  {VAL_GPIOB_MODER, VAL_GPIOB_OTYPER, VAL_GPIOB_OSPEEDR, VAL_GPIOB_PUPDR,
   VAL_GPIOB_ODR,   VAL_GPIOB_AFRL,   VAL_GPIOB_AFRH},
#endif
#if STM32_HAS_GPIOC
  {VAL_GPIOC_MODER, VAL_GPIOC_OTYPER, VAL_GPIOC_OSPEEDR, VAL_GPIOC_PUPDR,
   VAL_GPIOC_ODR,   VAL_GPIOC_AFRL,   VAL_GPIOC_AFRH},
#endif
#if STM32_HAS_GPIOD
  {VAL_GPIOD_MODER, VAL_GPIOD_OTYPER, VAL_GPIOD_OSPEEDR, VAL_GPIOD_PUPDR,
   VAL_GPIOD_ODR,   VAL_GPIOD_AFRL,   VAL_GPIOD_AFRH},
#endif
#if STM32_HAS_GPIOE
  {VAL_GPIOE_MODER, VAL_GPIOE_OTYPER, VAL_GPIOE_OSPEEDR, VAL_GPIOE_PUPDR,
   VAL_GPIOE_ODR,   VAL_GPIOE_AFRL,   VAL_GPIOE_AFRH},
#endif
#if STM32_HAS_GPIOF
  {VAL_GPIOF_MODER, VAL_GPIOF_OTYPER, VAL_GPIOF_OSPEEDR, VAL_GPIOF_PUPDR,
   VAL_GPIOF_ODR,   VAL_GPIOF_AFRL,   VAL_GPIOF_AFRH},
#endif
#if STM32_HAS_GPIOG
  {VAL_GPIOG_MODER, VAL_GPIOG_OTYPER, VAL_GPIOG_OSPEEDR, VAL_GPIOG_PUPDR,
   VAL_GPIOG_ODR,   VAL_GPIOG_AFRL,   VAL_GPIOG_AFRH},
#endif
#if STM32_HAS_GPIOH
  {VAL_GPIOH_MODER, VAL_GPIOH_OTYPER, VAL_GPIOH_OSPEEDR, VAL_GPIOH_PUPDR,
   VAL_GPIOH_ODR,   VAL_GPIOH_AFRL,   VAL_GPIOH_AFRH},
#endif
#if STM32_HAS_GPIOI
  {VAL_GPIOI_MODER, VAL_GPIOI_OTYPER, VAL_GPIOI_OSPEEDR, VAL_GPIOI_PUPDR,
   VAL_GPIOI_ODR,   VAL_GPIOI_AFRL,   VAL_GPIOI_AFRH}
#endif
};
#endif

/*
 * Enter the ROM (system memory) USB DFU bootloader from the running firmware.
 *
 * Jumping straight from the running firmware leaves PLL clocks, SysTick, the USB
 * peripheral and its NVIC interrupt, the app vector table and the process stack in
 * place, and the host never sees a detach (D+ has a fixed 1.5k pull-up on this board),
 * so the ROM DFU never enumerates. Instead: set a magic word in CCM RAM (not cleared
 * by the startup code), pull D+ low so the host sees a disconnect, reset the MCU, and
 * jump to the bootloader from __early_init() with the chip in its reset state.
 */
#define DFU_MAGIC 0xDF0B007AU
static volatile uint32_t dfu_request __attribute__((section(".ram4")));

void boardDFUEnter(void) {
  osalThreadSleepMilliseconds(20);         // let the last shell/UI output go out
  dfu_request = DFU_MAGIC;
  // USB detach: drive D+ low (same as usb_lld_disconnect_bus on this board)
  GPIOA->BSRR  = 1U << (GPIOA_USB_DP + 16);
  GPIOA->MODER = (GPIOA->MODER & ~(3U << (GPIOA_USB_DP * 2))) | (1U << (GPIOA_USB_DP * 2));
  osalThreadSleepMilliseconds(50);
  NVIC_SystemReset();
}

static void __attribute__((noreturn)) dfu_jump(void) {
  uint32_t sp = *((volatile uint32_t *)(STM32F303xC_SYSTEM_MEMORY));
  uint32_t pc = *((volatile uint32_t *)(STM32F303xC_SYSTEM_MEMORY + 4));
  // Map system memory at 0x0 (bootloader vectors), main stack, thread mode on MSP (crt0 runs on PSP)
  RCC->APB2ENR|= RCC_APB2ENR_SYSCFGEN;
  SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE) | SYSCFG_CFGR1_MEM_MODE_0;
  __asm volatile ("msr msp, %0   \n"
                  "msr control, %2\n"
                  "isb           \n"
                  "bx  %1        \n" :: "r"(sp), "r"(pc), "r"(0) : "memory");
  while (1);
}

/*
 * Early initialization code.
 * This initialization must be performed just after stack setup and before
 * any other initialization.
 */
void __early_init(void) {
  if (dfu_request == DFU_MAGIC) {
    dfu_request = 0;
    // Keep the LCD backlight on (DAC1 ch2, H4) so the DFU message stays readable
    RCC->APB1ENR|= RCC_APB1ENR_DAC1EN;
    DAC->DHR12R2 = 3000;
    DAC->CR|= DAC_CR_EN2;
    dfu_jump();
  }
  stm32_clock_init();
}

/*
 * Board-specific initialization code.
 */
void boardInit(void) {
  // Speedup flash latency
  FLASH->ACR= FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_0;
}
