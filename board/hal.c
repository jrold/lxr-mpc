/*
 * hal.c: the ST HAL functions the LXR firmware calls, for VOLTKIT's board.
 * Each does what ST's HAL documentation (UM1905) says it does, on the board
 * in board.c; the clocks follow RM0410 (6.3: RCC; 32.7.4: the I2S clock).
 * HAL_GetTick() and HAL_Delay() are the board's (board.c).
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#include "board.h"

#include <string.h>

#define WEAK __attribute__((weak))

uint32_t volt_i2s_fs;            /* the DACs' sample rate, from HAL_I2S_Init */
static uint32_t pll_m = 16, pll_source_hz = HSI_VALUE;

/* ================================================================ core */

__IO uint32_t uwTick;
uint32_t uwTickPrio = (1UL << __NVIC_PRIO_BITS);
HAL_TickFreqTypeDef uwTickFreq = HAL_TICK_FREQ_DEFAULT;

WEAK void HAL_MspInit(void) { }

HAL_StatusTypeDef HAL_Init(void)
{
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
    HAL_InitTick(TICK_INT_PRIORITY);
    HAL_MspInit();
    return HAL_OK;
}

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    if (SysTick_Config(volt_hclk / (1000U / uwTickFreq)) != 0U)
        return HAL_ERROR;
    uwTickPrio = TickPriority;
    return HAL_OK;
}

void HAL_IncTick(void) { uwTick += uwTickFreq; }

void HAL_NVIC_SetPriorityGrouping(uint32_t PriorityGroup) { (void)PriorityGroup; }
void HAL_NVIC_SetPriority(IRQn_Type IRQn, uint32_t PreemptPriority, uint32_t SubPriority)
{
    (void)IRQn;
    (void)PreemptPriority;
    (void)SubPriority;
}
void HAL_NVIC_EnableIRQ(IRQn_Type IRQn) { volt_board_nvic(IRQn, 1); }
void HAL_NVIC_DisableIRQ(IRQn_Type IRQn) { volt_board_nvic(IRQn, 0); }

/* ================================================================ clocks (RM0410 5) */

HAL_StatusTypeDef HAL_PWREx_EnableOverDrive(void) { return HAL_OK; }

static uint32_t pll_hz;          /* the main PLL's P output */

HAL_StatusTypeDef HAL_RCC_OscConfig(RCC_OscInitTypeDef *osc)
{
    if (osc->PLL.PLLState == RCC_PLL_ON)
    {
        pll_source_hz = osc->PLL.PLLSource == RCC_PLLSOURCE_HSE ? HSE_VALUE : HSI_VALUE;
        pll_m = osc->PLL.PLLM;
        pll_hz = (uint32_t)((uint64_t)pll_source_hz / pll_m * osc->PLL.PLLN / osc->PLL.PLLP);
    }
    return HAL_OK;
}

static uint32_t ahb_div(uint32_t d)
{
    switch (d)
    {
    case RCC_SYSCLK_DIV2: return 2;
    case RCC_SYSCLK_DIV4: return 4;
    case RCC_SYSCLK_DIV8: return 8;
    case RCC_SYSCLK_DIV16: return 16;
    case RCC_SYSCLK_DIV64: return 64;
    case RCC_SYSCLK_DIV128: return 128;
    case RCC_SYSCLK_DIV256: return 256;
    case RCC_SYSCLK_DIV512: return 512;
    default: return 1;
    }
}

static uint32_t apb_div(uint32_t d)
{
    switch (d)
    {
    case RCC_HCLK_DIV2: return 2;
    case RCC_HCLK_DIV4: return 4;
    case RCC_HCLK_DIV8: return 8;
    case RCC_HCLK_DIV16: return 16;
    default: return 1;
    }
}

HAL_StatusTypeDef HAL_RCC_ClockConfig(RCC_ClkInitTypeDef *clk, uint32_t FLatency)
{
    (void)FLatency;
    uint32_t sys = HSI_VALUE;
    if (clk->SYSCLKSource == RCC_SYSCLKSOURCE_PLLCLK && pll_hz)
        sys = pll_hz;
    else if (clk->SYSCLKSource == RCC_SYSCLKSOURCE_HSE)
        sys = HSE_VALUE;
    volt_hclk = sys / ahb_div(clk->AHBCLKDivider);
    volt_apb1_div = apb_div(clk->APB1CLKDivider);
    volt_apb2_div = apb_div(clk->APB2CLKDivider);
    SystemCoreClock = volt_hclk;
    return HAL_InitTick(uwTickPrio);   /* as ST's: the tick follows the new clock */
}

HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *p)
{
    if (p->PeriphClockSelection & RCC_PERIPHCLK_I2S)
        volt_i2s_clk = (uint32_t)((uint64_t)pll_source_hz / pll_m * p->PLLI2S.PLLI2SN / p->PLLI2S.PLLI2SR);
    return HAL_OK;
}

uint32_t HAL_RCC_GetHCLKFreq(void) { return volt_hclk; }
uint32_t HAL_RCC_GetPCLK1Freq(void) { return volt_hclk / volt_apb1_div; }
uint32_t HAL_RCC_GetPCLK2Freq(void) { return volt_hclk / volt_apb2_div; }

/* ================================================================ GPIO */

void HAL_GPIO_Init(GPIO_TypeDef *GPIOx, GPIO_InitTypeDef *init) { (void)GPIOx; (void)init; }
void HAL_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint32_t pin) { (void)GPIOx; (void)pin; }

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t pin)
{
    volt_board_gpio_settle();
    return (GPIOx->IDR & pin) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t pin, GPIO_PinState state)
{
    if (state != GPIO_PIN_RESET)
        GPIOx->ODR |= pin;
    else
        GPIOx->ODR &= ~(uint32_t)pin;
    volt_board_gpio_settle();
}

/* ================================================================ DMA */

HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef *hdma)
{
    hdma->State = HAL_DMA_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DMA_DeInit(DMA_HandleTypeDef *hdma)
{
    hdma->State = HAL_DMA_STATE_RESET;
    return HAL_OK;
}

void HAL_DMA_IRQHandler(DMA_HandleTypeDef *hdma)
{
    if (!volt_board_dma_take_complete(hdma))
        return;
    hdma->State = HAL_DMA_STATE_READY;
    if (hdma->XferCpltCallback)
        hdma->XferCpltCallback(hdma);
}

/* ================================================================ ADC1: knobs and faders */

WEAK void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc) { (void)hadc; }

HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *hadc)
{
    if (hadc->State == HAL_ADC_STATE_RESET)
        HAL_ADC_MspInit(hadc);
    hadc->State = HAL_ADC_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *hadc, ADC_ChannelConfTypeDef *c)
{
    (void)hadc;
    (void)c;   /* the ranks are the board's wiring (board.c): pots 1..8, faders 9..14 */
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_Start_DMA(ADC_HandleTypeDef *hadc, uint32_t *pData, uint32_t Length)
{
    volt_board_adc_dma(hadc, (uint16_t *)pData, Length);   /* halfword transfers into the firmware's buffer */
    return HAL_OK;
}

/* ================================================================ I2S2, I2S3: the DACs */

WEAK void HAL_I2S_MspInit(I2S_HandleTypeDef *hi2s) { (void)hi2s; }
WEAK void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s) { (void)hi2s; }

HAL_StatusTypeDef HAL_I2S_Init(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->State == HAL_I2S_STATE_RESET)
        HAL_I2S_MspInit(hi2s);
    /* the prescaler ST's HAL picks for this clock and rate, and the rate it gives (RM0410 32.7.4) */
    const uint32_t clk = volt_i2s_clk ? volt_i2s_clk : 45200000u;
    const int mclk = hi2s->Init.MCLKOutput == I2S_MCLKOUTPUT_ENABLE;
    const uint32_t per_frame = mclk ? 256u : (hi2s->Init.DataFormat == I2S_DATAFORMAT_16B ? 32u : 64u);
    uint32_t tmp = ((clk / per_frame) * 10u / hi2s->Init.AudioFreq + 5u) / 10u;
    uint32_t odd = tmp & 1u, div = (tmp - odd) / 2u;
    if (div < 2u || div > 0xFFu)
    {
        div = 2u;
        odd = 0u;
    }
    volt_i2s_fs = clk / (per_frame * (2u * div + odd));
    hi2s->State = HAL_I2S_STATE_READY;
    return HAL_OK;
}

static void i2s_dma_complete(DMA_HandleTypeDef *hdma)
{
    I2S_HandleTypeDef *hi2s = (I2S_HandleTypeDef *)hdma->Parent;
    hi2s->TxXferCount = 0;
    hi2s->State = HAL_I2S_STATE_READY;
    HAL_I2S_TxCpltCallback(hi2s);
}

HAL_StatusTypeDef HAL_I2S_Transmit_DMA(I2S_HandleTypeDef *hi2s, uint16_t *pData, uint16_t Size)
{
    if (pData == NULL || Size == 0u)
        return HAL_ERROR;
    hi2s->pTxBuffPtr = pData;
    hi2s->TxXferSize = hi2s->TxXferCount = Size;
    hi2s->State = HAL_I2S_STATE_BUSY_TX;
    if (hi2s->hdmatx)
    {
        hi2s->hdmatx->XferCpltCallback = i2s_dma_complete;
        hi2s->hdmatx->State = HAL_DMA_STATE_BUSY;
    }
    volt_board_i2s_transmit(hi2s, pData, Size, volt_i2s_fs);
    return HAL_OK;
}

/* ================================================================ SPI1: the shift registers */

WEAK void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi) { (void)hspi; }

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi)
{
    if (hspi->State == HAL_SPI_STATE_RESET)
        HAL_SPI_MspInit(hspi);
    hspi->State = HAL_SPI_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    (void)Timeout;
    volt_board_spi_transmit(hspi->Instance, pData, Size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    (void)Timeout;
    volt_board_spi_receive(hspi->Instance, pData, Size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *pTxData, uint8_t *pRxData, uint16_t Size,
                                          uint32_t Timeout)
{
    (void)Timeout;
    volt_board_spi_transmit(hspi->Instance, pTxData, Size);
    volt_board_spi_receive(hspi->Instance, pRxData, Size);
    return HAL_OK;
}

/* ================================================================ timers */

WEAK void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim) { (void)htim; }
WEAK void HAL_TIM_OC_MspInit(TIM_HandleTypeDef *htim) { (void)htim; }
WEAK void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) { (void)htim; }

static void tim_load(TIM_HandleTypeDef *htim)
{
    htim->Instance->PSC = htim->Init.Prescaler;
    htim->Instance->ARR = htim->Init.Period;
}

HAL_StatusTypeDef HAL_TIM_Base_Init(TIM_HandleTypeDef *htim)
{
    if (htim->State == HAL_TIM_STATE_RESET)
        HAL_TIM_Base_MspInit(htim);
    tim_load(htim);
    htim->State = HAL_TIM_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_OC_Init(TIM_HandleTypeDef *htim)
{
    if (htim->State == HAL_TIM_STATE_RESET)
        HAL_TIM_OC_MspInit(htim);
    tim_load(htim);
    htim->State = HAL_TIM_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_ConfigClockSource(TIM_HandleTypeDef *htim, TIM_ClockConfigTypeDef *s)
{
    (void)htim;
    (void)s;   /* the internal clock */
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_OC_ConfigChannel(TIM_HandleTypeDef *htim, TIM_OC_InitTypeDef *s, uint32_t Channel)
{
    (void)htim;
    (void)s;
    (void)Channel;   /* no output is wired to anything the board models */
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIMEx_MasterConfigSynchronization(TIM_HandleTypeDef *htim, TIM_MasterConfigTypeDef *s)
{
    (void)htim;
    (void)s;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *htim)
{
    htim->Instance->DIER |= TIM_DIER_UIE;
    htim->Instance->CR1 |= TIM_CR1_CEN;
    volt_board_timer_started(htim->Instance);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *htim)
{
    htim->Instance->DIER &= ~TIM_DIER_UIE;
    htim->Instance->CR1 &= ~TIM_CR1_CEN;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Stop(TIM_HandleTypeDef *htim)
{
    htim->Instance->CR1 &= ~TIM_CR1_CEN;
    return HAL_OK;
}

void HAL_TIM_IRQHandler(TIM_HandleTypeDef *htim)
{
    if ((htim->Instance->SR & TIM_SR_UIF) && (htim->Instance->DIER & TIM_DIER_UIE))
    {
        htim->Instance->SR &= ~TIM_SR_UIF;
        HAL_TIM_PeriodElapsedCallback(htim);
    }
}

/* ================================================================ USART3: MIDI */

WEAK void HAL_UART_MspInit(UART_HandleTypeDef *huart) { (void)huart; }

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *huart)
{
    if (huart->gState == HAL_UART_STATE_RESET)
        HAL_UART_MspInit(huart);
    huart->Instance->CR1 |= USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
    huart->gState = huart->RxState = HAL_UART_STATE_READY;
    return HAL_OK;
}

/* ================================================================ SDMMC1: the card */

WEAK void HAL_SD_MspInit(SD_HandleTypeDef *hsd) { (void)hsd; }
WEAK void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd) { (void)hsd; }
WEAK void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hsd) { (void)hsd; }

static void sd_card_info(SD_HandleTypeDef *hsd)
{
    HAL_SD_CardInfoTypeDef *i = &hsd->SdCard;
    i->CardType = CARD_SDHC_SDXC;
    i->CardVersion = CARD_V2_X;
    i->Class = 0x5B5u;
    i->RelCardAdd = 1u;
    i->BlockNbr = i->LogBlockNbr = volt_card_sectors();
    i->BlockSize = i->LogBlockSize = 512u;
}

HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef *hsd)
{
    if (hsd->State == HAL_SD_STATE_RESET)
        HAL_SD_MspInit(hsd);
    sd_card_info(hsd);
    hsd->State = HAL_SD_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SD_GetCardInfo(SD_HandleTypeDef *hsd, HAL_SD_CardInfoTypeDef *info)
{
    sd_card_info(hsd);
    *info = hsd->SdCard;
    return HAL_OK;
}

static HAL_StatusTypeDef sd_range(uint32_t block, uint32_t n)
{
    return (uint64_t)block + n > volt_card_sectors() ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef *hsd, uint8_t *pData, uint32_t BlockAdd, uint32_t NumberOfBlocks,
                                    uint32_t Timeout)
{
    (void)hsd;
    (void)Timeout;
    if (sd_range(BlockAdd, NumberOfBlocks) != HAL_OK)
        return HAL_ERROR;
    for (uint32_t i = 0; i < NumberOfBlocks; i++)
        volt_card_read(BlockAdd + i, pData + 512u * i);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef *hsd, uint8_t *pData, uint32_t BlockAdd, uint32_t NumberOfBlocks,
                                     uint32_t Timeout)
{
    (void)hsd;
    (void)Timeout;
    if (sd_range(BlockAdd, NumberOfBlocks) != HAL_OK)
        return HAL_ERROR;
    for (uint32_t i = 0; i < NumberOfBlocks; i++)
        volt_card_write(BlockAdd + i, pData + 512u * i);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef *hsd, uint8_t *pData, uint32_t BlockAdd, uint32_t NumberOfBlocks)
{
    if (HAL_SD_ReadBlocks(hsd, pData, BlockAdd, NumberOfBlocks, 0) != HAL_OK)
        return HAL_ERROR;
    HAL_SD_RxCpltCallback(hsd);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef *hsd, uint8_t *pData, uint32_t BlockAdd, uint32_t NumberOfBlocks)
{
    if (HAL_SD_WriteBlocks(hsd, pData, BlockAdd, NumberOfBlocks, 0) != HAL_OK)
        return HAL_ERROR;
    HAL_SD_TxCpltCallback(hsd);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SD_Erase(SD_HandleTypeDef *hsd, uint32_t BlockStartAdd, uint32_t BlockEndAdd)
{
    (void)hsd;
    static const uint8_t zeros[512];
    if (BlockEndAdd < BlockStartAdd || sd_range(BlockStartAdd, BlockEndAdd - BlockStartAdd + 1u) != HAL_OK)
        return HAL_ERROR;
    for (uint32_t b = BlockStartAdd; b <= BlockEndAdd; b++)
        volt_card_write(b, zeros);
    return HAL_OK;
}

HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    return HAL_SD_CARD_TRANSFER;
}

void HAL_SD_IRQHandler(SD_HandleTypeDef *hsd) { (void)hsd; }

/* ================================================================ RNG */

WEAK void HAL_RNG_MspInit(RNG_HandleTypeDef *hrng) { (void)hrng; }

HAL_StatusTypeDef HAL_RNG_Init(RNG_HandleTypeDef *hrng)
{
    if (hrng->State == HAL_RNG_STATE_RESET)
        HAL_RNG_MspInit(hrng);
    hrng->State = HAL_RNG_STATE_READY;
    return HAL_OK;
}

uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef *hrng)
{
    (void)hrng;
    return volt_board_random();
}

HAL_StatusTypeDef HAL_RNG_GenerateRandomNumber(RNG_HandleTypeDef *hrng, uint32_t *random32bit)
{
    (void)hrng;
    *random32bit = volt_board_random();
    return HAL_OK;
}

/* ================================================================ flash: the firmware's own updater only */

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_OB_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_OB_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_OB_Launch(void) { return HAL_OK; }

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    (void)Address;
    (void)Data;
    return HAL_ERROR;   /* there is no flash to write */
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *SectorError)
{
    (void)pEraseInit;
    *SectorError = 0;
    return HAL_ERROR;
}

HAL_StatusTypeDef HAL_FLASHEx_OBProgram(FLASH_OBProgramInitTypeDef *pOBInit)
{
    (void)pOBInit;
    return HAL_ERROR;
}

void HAL_FLASHEx_OBGetConfig(FLASH_OBProgramInitTypeDef *pOBInit) { memset(pOBInit, 0, sizeof *pOBInit); }

/* ================================================================ USB: no host is ever attached */

WEAK void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd) { (void)hpcd; }
WEAK void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd) { (void)hpcd; }

HAL_StatusTypeDef HAL_PCD_Init(PCD_HandleTypeDef *hpcd)
{
    if (hpcd->State == HAL_PCD_STATE_RESET)
        HAL_PCD_MspInit(hpcd);
    hpcd->State = HAL_PCD_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_DeInit(PCD_HandleTypeDef *hpcd)
{
    HAL_PCD_MspDeInit(hpcd);
    hpcd->State = HAL_PCD_STATE_RESET;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_Start(PCD_HandleTypeDef *hpcd) { (void)hpcd; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_Stop(PCD_HandleTypeDef *hpcd) { (void)hpcd; return HAL_OK; }
void HAL_PCD_IRQHandler(PCD_HandleTypeDef *hpcd) { (void)hpcd; }
HAL_StatusTypeDef HAL_PCD_SetAddress(PCD_HandleTypeDef *hpcd, uint8_t address) { (void)hpcd; (void)address; return HAL_OK; }

HAL_StatusTypeDef HAL_PCD_EP_Open(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint16_t ep_mps, uint8_t ep_type)
{
    (void)hpcd; (void)ep_addr; (void)ep_mps; (void)ep_type;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_Close(PCD_HandleTypeDef *hpcd, uint8_t ep_addr) { (void)hpcd; (void)ep_addr; return HAL_OK; }

HAL_StatusTypeDef HAL_PCD_EP_Receive(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint8_t *pBuf, uint32_t len)
{
    (void)hpcd; (void)ep_addr; (void)pBuf; (void)len;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_Transmit(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint8_t *pBuf, uint32_t len)
{
    (void)hpcd; (void)ep_addr; (void)pBuf; (void)len;
    return HAL_OK;
}

uint32_t HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef *hpcd, uint8_t ep_addr) { (void)hpcd; (void)ep_addr; return 0; }
HAL_StatusTypeDef HAL_PCD_EP_SetStall(PCD_HandleTypeDef *hpcd, uint8_t ep_addr) { (void)hpcd; (void)ep_addr; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_ClrStall(PCD_HandleTypeDef *hpcd, uint8_t ep_addr) { (void)hpcd; (void)ep_addr; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_Flush(PCD_HandleTypeDef *hpcd, uint8_t ep_addr) { (void)hpcd; (void)ep_addr; return HAL_OK; }
HAL_StatusTypeDef HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef *hpcd, uint16_t size) { (void)hpcd; (void)size; return HAL_OK; }

HAL_StatusTypeDef HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef *hpcd, uint8_t fifo, uint16_t size)
{
    (void)hpcd; (void)fifo; (void)size;
    return HAL_OK;
}
