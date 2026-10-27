/* hal_unused.c: hal functions the firmware references but never calls on the
 * virtual board (flash programming, usb device teardown, dma sd transfers),
 * and the cpu state the portable intrinsics keep in memory on non-arm hosts.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "board.h"
volatile uint32_t lxr_primask;
volatile uint32_t lxr_fpscr;
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e, uint32_t *err) { (void)e; if (err) *err = 0; return HAL_ERROR; }
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t t, uint32_t a, uint64_t d) { (void)t; (void)a; (void)d; return HAL_ERROR; }
HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_ERROR; }
void HAL_GPIO_DeInit(GPIO_TypeDef *p, uint32_t pin) { (void)p; (void)pin; }
HAL_StatusTypeDef HAL_PCD_DeInit(PCD_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_ClrStall(PCD_HandleTypeDef *h, uint8_t a) { (void)h; (void)a; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_Flush(PCD_HandleTypeDef *h, uint8_t a) { (void)h; (void)a; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_SetAddress(PCD_HandleTypeDef *h, uint8_t a) { (void)h; (void)a; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_Stop(PCD_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_SD_Erase(SD_HandleTypeDef *h, uint32_t a, uint32_t b) { (void)h; (void)a; (void)b; return HAL_ERROR; }
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef *h, uint8_t *d, uint32_t b, uint32_t n) { (void)h; (void)d; (void)b; (void)n; return HAL_ERROR; }
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef *h, uint8_t *d, uint32_t b, uint32_t n) { (void)h; (void)d; (void)b; (void)n; return HAL_ERROR; }
