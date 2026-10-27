/*
 * board.h: what VOLTKIT's HAL (board/hal.c) and board model (board/board.c)
 * share. Not part of the firmware's view of the world.
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#ifndef VOLT_BOARD_H
#define VOLT_BOARD_H

#include "stm32f7xx_hal.h"

#include <stdint.h>

/* ---- clocks (set by HAL_RCC_ClockConfig from the firmware's own settings) */
extern uint32_t volt_hclk;         /* core clock, Hz */
extern uint32_t volt_apb1_div;     /* APB1 and APB2 prescalers */
extern uint32_t volt_apb2_div;
extern uint32_t volt_i2s_clk;      /* the I2S clock from PLLI2S, Hz */
extern uint32_t volt_i2s_fs;       /* the DACs' sample rate (HAL_I2S_Init) */

/* ---- time: the core's cycles since reset */
extern uint64_t volt_now;
void volt_board_spend(uint64_t cycles);          /* main-context time passes (HAL_Delay, polling) */
uint64_t volt_cycles_per_ms(void);

/* ---- what the HAL tells the board */
void volt_board_nvic(IRQn_Type irq, int enabled);
void volt_board_timer_started(TIM_TypeDef *tim);
void volt_board_i2s_transmit(I2S_HandleTypeDef *hi2s, const uint16_t *data, uint16_t size, uint32_t fs);
void volt_board_adc_dma(ADC_HandleTypeDef *hadc, uint16_t *buffer, uint32_t length);
void volt_board_spi_receive(SPI_TypeDef *spi, uint8_t *data, uint16_t size);
void volt_board_spi_transmit(SPI_TypeDef *spi, const uint8_t *data, uint16_t size);
int volt_board_dma_take_complete(DMA_HandleTypeDef *hdma);
void volt_board_gpio_settle(void);
uint32_t volt_board_random(void);

/* ---- the SD card */
uint32_t volt_card_sectors(void);
void volt_card_read(uint32_t sector, uint8_t *data);
void volt_card_write(uint32_t sector, const uint8_t *data);

#endif
