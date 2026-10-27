/*
 * stm32f7xx.h for VOLTKIT's board: ST's own device headers, then every
 * peripheral instance made a board accessor (volt_periphs.h), then the HAL.
 *
 * ST's stm32f7xx.h includes the HAL at its end; the HAL's macros and inline
 * functions must see the accessors, so the HAL is held back until they are
 * in place.
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#ifndef VOLT_STM32F7XX_H
#define VOLT_STM32F7XX_H

#ifdef USE_HAL_DRIVER
#define VOLT_USE_HAL_DRIVER
#undef USE_HAL_DRIVER
#endif

#include_next <stm32f7xx.h>
#include "volt_periphs.h"

#ifdef VOLT_USE_HAL_DRIVER
#define USE_HAL_DRIVER
#include "stm32f7xx_hal.h"
#endif

#endif
