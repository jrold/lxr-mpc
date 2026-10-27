// VOLTKIT: types and calls from the STM32 HAL that the engine's headers
// mention but the sound engine never uses on the MPC.
#pragma once
#include <stdint.h>

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __IO
#define __IO volatile
#endif

typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { int unused; } UART_HandleTypeDef;
typedef struct { int unused; } TIM_HandleTypeDef;
typedef struct { int unused; } ADC_HandleTypeDef;
typedef struct { int unused; } I2S_HandleTypeDef;
typedef struct { int unused; } DMA_HandleTypeDef;
typedef struct { int unused; } GPIO_TypeDef;
typedef struct { volatile uint32_t CYCCNT; } VoltDwt;
extern VoltDwt volt_dwt;
#define DWT (&volt_dwt)
static inline uint32_t HAL_RCC_GetHCLKFreq(void) { return 216000000u; }
static inline uint32_t HAL_GetTick(void) { return 0; }

// GPIO, only so the mixer can read its output-jack switches. VOLTKIT
// reports the main outputs plugged in and the second pair empty, so the
// firmware folds every voice onto one stereo pair (stubs.c).
typedef struct { volatile uint32_t IDR; } VoltGpio;
extern VoltGpio volt_gpio_b, volt_gpio_d;
#define GPIOB (&volt_gpio_b)
#define GPIOD (&volt_gpio_d)
#define GPIO_PIN_4 0x0010u
#define GPIO_PIN_6 0x0040u
#define GPIO_PIN_7 0x0080u
