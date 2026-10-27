/*
 * vboard.c: the virtual LXR-02 board under the firmware, for VOLTKIT.
 *
 * Adapted from lxr02-recomp's 2.engine/platform/board.c (Greg Sadetsky,
 * GPL-3.0-only; platform/RECOMP_COMMIT): the same model of the hardware
 * (virtual time, the interrupt sources, the GPIO pins, the HD44780 LCD on
 * its pins, the shift-register buttons and LEDs, the pot/fader ADC buffer,
 * the MIDI UART, the two I2S DACs, the encoder), with its file and scenario
 * I/O replaced by the host interface of engine/volt_machine.h: inputs from
 * the host, the SD card in memory, the DACs into a FIFO the host reads.
 *
 * The firmware runs as a coroutine on its own stack. render() resumes it;
 * it yields back from the DMA-complete event once the FIFO holds the frames
 * the host asked for, so it never runs ahead by more than one 64-frame
 * block and needs no thread. Firmware code paths are never replaced:
 * interrupts are delivered by calling the firmware's own handlers.
 *
 * Copyright (C) 2026 Greg Sadetsky, Jason Rold. SPDX-License-Identifier: GPL-3.0-only
 */
#define _XOPEN_SOURCE 600   /* ucontext */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include "board.h"
#include "main.h"
#include "stm32f7xx_it.h"
#include "Soundengine/globals.h"
#include "volt_machine.h"

void USART3_IRQHandler(void);                      /* Core/Src/Soundengine/MIDI/Uart.c */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *h); /* Core/Src/Soundengine/cs4344_cs5343.c */
int lxr_firmware_main(void);                       /* Core/Src/main.c's main(), renamed by the build */

uint64_t lxr_now;
static int in_isr;                    /* no nested interrupts are modelled */
/* exception entry/return as the cortex-m7 fpu does it: the handler runs with FPSCR's
 * control bits taken from FPDSCR, the interrupted code's FPSCR comes back on return. */
static uint32_t isr_fpscr_saved;
static void isr_enter(void)
{
    in_isr = 1;
    isr_fpscr_saved = __get_FPSCR();
    __set_FPSCR(FPU->FPDSCR & 0x07C00000u);
}
static void isr_exit(void)
{
    __set_FPSCR(isr_fpscr_saved);
    in_isr = 0;
}

/* --------------------------------------------------------------- coroutine */
#define FW_STACK_BYTES (1u << 20)
static ucontext_t host_ctx, fw_ctx;
static char *fw_stack;
static enum { FW_IDLE, FW_RUNNING, FW_STOPPED } fw_state;
static int fw_want;                   /* frames the host is waiting for */

/* the dacs' output, both summed, as float frames */
#define FIFO_FRAMES 1024u
static float fifo_l[FIFO_FRAMES], fifo_r[FIFO_FRAMES];
static uint32_t fifo_head, fifo_count;

static void firmware_entry(void)
{
    lxr_firmware_main();             /* returns only if the firmware gives up */
    fw_state = FW_STOPPED;
    swapcontext(&fw_ctx, &host_ctx);
}

/* -------------------------------------------------------------- the inputs
 * queued by the host while the firmware is paused, applied at the next dma round
 * (once per 64 frames), as lxr02-recomp's live input lines are */
typedef enum { IN_BUTTON, IN_ADC, IN_ENCODER, IN_ENC_BUTTON, IN_MIDI } input_kind;
typedef struct { input_kind kind; int32_t a, b; uint8_t bytes[16]; uint8_t n; } input;
#define INPUTS 1024u
static input inputs[INPUTS];
static uint32_t in_head, in_tail;
static void queue_input(const input *e)
{
    if (in_head - in_tail >= INPUTS) return;   /* full: drop (the host sends far less) */
    inputs[in_head++ % INPUTS] = *e;
}

/* --------------------------------------------------------------------- lcd
 * hd44780 in 4-bit mode on GPIOE: D4..D7 = PE7..PE10, E = PE11, RS = PE12.
 * the controller latches a nibble on the falling edge of E. */
static uint8_t lcd_ddram[128], lcd_cgram[64];
static uint8_t lcd_addr, lcd_cg_mode, lcd_mode8 = 1, lcd_have_hi, lcd_hi, lcd_inc = 1;
static uint32_t lcd_last_pins, lcd_rev;

static void lcd_byte(uint8_t rs, uint8_t v)
{
    if (rs) {
        if (lcd_cg_mode) lcd_cgram[lcd_addr & 63] = v; else lcd_ddram[lcd_addr & 127] = v;
        lcd_addr = (uint8_t)(lcd_addr + (lcd_inc ? 1 : -1));
        lcd_rev++;
        return;
    }
    if (v & 0x80) { lcd_cg_mode = 0; lcd_addr = v & 0x7F; }
    else if (v & 0x40) { lcd_cg_mode = 1; lcd_addr = v & 0x3F; }
    else if (v & 0x20) { if (!(v & 0x10)) lcd_mode8 = 0; }        /* function set */
    else if (v & 0x04 && !(v & 0x08) && !(v & 0x10)) { lcd_inc = (v >> 1) & 1; }   /* entry mode */
    else if (v == 0x01) { memset(lcd_ddram, ' ', sizeof lcd_ddram); lcd_addr = 0; lcd_cg_mode = 0; lcd_rev++; }
    else if ((v & 0xFE) == 0x02) { lcd_addr = 0; lcd_cg_mode = 0; }
}

static void lcd_pins(uint32_t pins)
{
    const uint32_t E = GPIO_PIN_11;
    if ((lcd_last_pins & E) && !(pins & E)) {          /* falling edge: latch */
        const uint8_t nib = (uint8_t)((lcd_last_pins >> 7) & 0xF);
        const uint8_t rs = (lcd_last_pins & GPIO_PIN_12) ? 1 : 0;
        if (lcd_mode8) {
            lcd_byte(rs, (uint8_t)(nib << 4));
        } else if (!lcd_have_hi) {
            lcd_hi = nib; lcd_have_hi = 1;
        } else {
            lcd_have_hi = 0;
            lcd_byte(rs, (uint8_t)((lcd_hi << 4) | nib));
        }
    }
    lcd_last_pins = pins;
}

/* --------------------------------------------------------------------- gpio */
GPIO_TypeDef *lxr_gpio_access(GPIO_TypeDef *p)
{
    const uint32_t bsrr = p->BSRR;
    if (bsrr) {
        /* set has priority over reset for the same pin (rm0410 gpio bsrr) */
        p->ODR = (p->ODR & ~(bsrr >> 16)) | (bsrr & 0xFFFFu);
        p->BSRR = 0;
    }
    if (p == &lxr_periph_GPIOE_BASE) lcd_pins(p->ODR);
    return p;
}

static void gpio_fold_all(void)
{
    (void)lxr_gpio_access(&lxr_periph_GPIOA_BASE); (void)lxr_gpio_access(&lxr_periph_GPIOB_BASE);
    (void)lxr_gpio_access(&lxr_periph_GPIOC_BASE); (void)lxr_gpio_access(&lxr_periph_GPIOD_BASE);
    (void)lxr_gpio_access(&lxr_periph_GPIOE_BASE);
}

/* ---------------------------------------------------------- shift registers */
static uint8_t buttons[5];      /* 40 inputs, 1 = pressed */
static uint8_t leds[6], leds_last[6];   /* 5 shift-register bytes + page down */
static uint32_t led_rev;

void lxr_board_spi_receive(uint8_t *data, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) data[i] = i < 5 ? buttons[i] : 0;
}
void lxr_board_spi_transmit(const uint8_t *data, uint16_t n)
{
    for (uint16_t i = 0; i < n && i < 5; i++) leds[i] = data[i];
}

/* ---------------------------------------------------------------------- adc */
static uint16_t *adc_dma; static uint32_t adc_len;
static uint16_t adc_values[16];
void lxr_board_adc_started(uint16_t *buf, uint32_t len)
{
    adc_dma = buf; adc_len = len;
    for (uint32_t i = 0; i < len && i < 16; i++) adc_dma[i] = adc_values[i];
}

/* ---------------------------------------------------------------------- rng */
static uint32_t rng_state = 0x12345678u;
uint32_t lxr_board_rng(void)
{   /* xorshift32. the device has a true rng. */
    uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng_state = x;
}

/* ------------------------------------------------------------------ sd card
 * in memory, a sector at a time: unwritten sectors read as zeros */
static uint8_t *card[VOLT_CARD_SECTORS];
static uint32_t card_rev;
static void card_put(uint32_t s, const uint8_t *d)
{
    if (s >= VOLT_CARD_SECTORS) return;
    if (!card[s]) {
        int any = 0;
        for (uint32_t i = 0; i < VOLT_SECTOR_BYTES && !any; i++) any = d[i] != 0;
        if (!any) return;
        if (!(card[s] = malloc(VOLT_SECTOR_BYTES))) return;
    }
    memcpy(card[s], d, VOLT_SECTOR_BYTES);
}
static void card_get(uint32_t s, uint8_t *d)
{
    if (s < VOLT_CARD_SECTORS && card[s]) memcpy(d, card[s], VOLT_SECTOR_BYTES);
    else memset(d, 0, VOLT_SECTOR_BYTES);
}
int lxr_board_sd_read(uint8_t *buf, uint32_t block, uint32_t n)
{
    if (block + n > VOLT_CARD_SECTORS) return -1;
    for (uint32_t i = 0; i < n; i++) card_get(block + i, buf + i * VOLT_SECTOR_BYTES);
    return 0;
}
int lxr_board_sd_write(const uint8_t *buf, uint32_t block, uint32_t n)
{
    if (block + n > VOLT_CARD_SECTORS) return -1;
    for (uint32_t i = 0; i < n; i++) card_put(block + i, buf + i * VOLT_SECTOR_BYTES);
    card_rev++;
    return 0;
}
uint32_t lxr_board_sd_blocks(void) { return VOLT_CARD_SECTORS; }

/* ------------------------------------------------------------------- timers
 * apb1 timers (TIM2, TIM14) run at 108 MHz = hclk/2: one timer tick is
 * 2*(PSC+1) cpu cycles. update event when the counter passes ARR. */
typedef struct { TIM_TypeDef *t; uint64_t base_cycle; uint32_t base_cnt; uint32_t last_pub; int bits32; uint64_t due; } vtimer;
static vtimer vt2, vt14;

static uint64_t timer_due(vtimer *v)
{
    TIM_TypeDef *t = v->t;
    if (!(t->CR1 & TIM_CR1_CEN)) return UINT64_MAX;
    const uint64_t tick = 2ull * (t->PSC + 1u);
    const uint32_t arr = t->ARR;
    if (v->base_cnt > arr) return UINT64_MAX;
    return v->base_cycle + (uint64_t)(arr + 1u - v->base_cnt) * tick;
}
static void timer_sync(vtimer *v)
{   /* pick up a counter written by the firmware since the last sync */
    if (v->t->CNT != v->last_pub) { v->base_cnt = v->t->CNT; v->base_cycle = lxr_now; }
    v->due = timer_due(v);
}
static void timer_publish(vtimer *v)
{
    TIM_TypeDef *t = v->t;
    uint32_t cnt = v->base_cnt;
    if (t->CR1 & TIM_CR1_CEN) {
        const uint64_t tick = 2ull * (t->PSC + 1u);
        cnt = v->base_cnt + (uint32_t)((lxr_now - v->base_cycle) / tick);
    }
    t->CNT = v->bits32 ? cnt : (cnt & 0xFFFFu);
    v->last_pub = t->CNT;
}
void lxr_board_timer_changed(TIM_TypeDef *tim)
{
    vtimer *v = tim == &lxr_periph_TIM2_BASE ? &vt2 : tim == &lxr_periph_TIM14_BASE ? &vt14 : NULL;
    if (!v) return;
    v->base_cnt = tim->CNT; v->base_cycle = lxr_now; v->last_pub = tim->CNT;
    v->due = timer_due(v);
}

/* ----------------------------------------------------------------- systick */
static int systick_on; static uint64_t systick_due;
void lxr_board_systick_started(void) { systick_on = 1; systick_due = lxr_now + LXR_HCLK / 1000u; }

/* ---------------------------------------------------------------- i2s dacs
 * cs4344 dacs at fs = 22.6 MHz / 512 = 44140.625 Hz (plli2s n=113, r=5).
 * both streams transfer 64 stereo frames per dma round. */
#define I2S_FS_NUM 706250ull   /* fs = 706250/16 */
static const uint16_t *i2s_buf[2]; static uint16_t i2s_size[2];
static uint16_t i2s_snap[2][256];
static uint64_t dma_rounds;
static uint64_t dma_t0 = UINT64_MAX;
static uint64_t dma_due_cycle(uint64_t round) { return dma_t0 + ((round + 1u) * 64u * (uint64_t)LXR_HCLK * 16u) / I2S_FS_NUM; }

void lxr_board_i2s_started(I2S_HandleTypeDef *h, const uint16_t *buf, uint16_t size)
{
    const int k = h->Instance == SPI2 ? 0 : 1;
    if (dma_t0 == UINT64_MAX) dma_t0 = lxr_now;
    i2s_buf[k] = buf; i2s_size[k] = size > 256 ? 256 : size;
    /* the round's samples, taken at its start (see lxr02-recomp's board.c) */
    memcpy(i2s_snap[k], buf, (size_t)i2s_size[k] * 2u);
}

/* -------------------------------------------------------------------- uart
 * din midi on USART3, 31250 baud: one byte every 320 us. */
#define UART_BYTE_CYCLES (LXR_HCLK / 3125u)
static uint8_t rxq[4096]; static uint32_t rxq_head, rxq_tail; static uint64_t rx_due = UINT64_MAX;
static uint64_t tx_due = UINT64_MAX;

static void uart_rx_event(void)
{
    USART_TypeDef *u = USART3;
    if (rxq_head == rxq_tail) { rx_due = UINT64_MAX; return; }
    if (u->ISR & USART_ISR_RXNE) u->ISR |= USART_ISR_ORE;        /* previous byte never read */
    u->RDR = rxq[rxq_tail++ & 4095u];
    u->ISR |= USART_ISR_RXNE;
    if (u->CR1 & USART_CR1_RXNEIE) { isr_enter(); USART3_IRQHandler(); isr_exit(); }
    u->ISR &= ~USART_ISR_RXNE;                                    /* the handler read RDR */
    rx_due = rxq_head != rxq_tail ? lxr_now + UART_BYTE_CYCLES : UINT64_MAX;
}
static void uart_tx_event(void)
{   /* the din midi output: sent nowhere */
    USART_TypeDef *u = USART3;
    if (!(u->CR1 & USART_CR1_TXEIE)) { tx_due = UINT64_MAX; return; }
    u->TDR = 0xFFFFu;                                             /* sentinel: write detection */
    u->ISR |= USART_ISR_TXE;
    isr_enter(); USART3_IRQHandler(); isr_exit();
    if (u->TDR != 0xFFFFu) {
        u->ISR &= ~USART_ISR_TXE;
        tx_due = lxr_now + UART_BYTE_CYCLES;
    } else {
        tx_due = (u->CR1 & USART_CR1_TXEIE) ? lxr_now + UART_BYTE_CYCLES : UINT64_MAX;
    }
}

/* ------------------------------------------------------------------- encoder */
static int enc_pending;           /* detents still to turn (+/-) */
static int enc_phase;             /* 0..3 in the gray sequence 00 01 11 10 */
static uint64_t enc_due = UINT64_MAX;
static void enc_set_pins(void)
{
    static const uint8_t gray[4] = { 0, 1, 3, 2 };   /* A<<1 | B, forward = 00->01->11->10 */
    const uint8_t s = gray[enc_phase & 3];
    GPIO_TypeDef *e = &lxr_periph_GPIOE_BASE;
    e->IDR = (e->IDR & ~(uint32_t)(GPIO_PIN_13 | GPIO_PIN_14)) | ((s & 2) ? GPIO_PIN_13 : 0) | ((s & 1) ? GPIO_PIN_14 : 0);
}
static void enc_event(void)
{   /* one quarter step every 2 ms, sampled by the 1 ms SysTick */
    if (enc_pending == 0) { enc_due = UINT64_MAX; return; }
    enc_phase += enc_pending > 0 ? 1 : -1;
    if ((enc_phase & 3) == 0) enc_pending += enc_pending > 0 ? -1 : 1;
    enc_set_pins();
    enc_due = enc_pending ? lxr_now + 2u * (LXR_HCLK / 1000u) : UINT64_MAX;
}

/* ------------------------------------------------------------ apply inputs */
static void apply_input(const input *e)
{
    switch (e->kind) {
    case IN_MIDI:
        for (int i = 0; i < e->n; i++) rxq[rxq_head++ & 4095u] = e->bytes[i];
        if (rx_due == UINT64_MAX) rx_due = lxr_now;
        break;
    case IN_BUTTON:
        if (e->a >= 0 && e->a < 40) {
            if (e->b) buttons[e->a / 8] |= (uint8_t)(1u << (e->a % 8));
            else buttons[e->a / 8] &= (uint8_t)~(1u << (e->a % 8));
        } else if (e->a == 40) {   /* BUT_PAGE_DOWN: lone switch on SW_43 (PB7), high = pressed (DIO.c) */
            if (e->b) lxr_periph_GPIOB_BASE.IDR |= GPIO_PIN_7; else lxr_periph_GPIOB_BASE.IDR &= ~(uint32_t)GPIO_PIN_7;
        }
        break;
    case IN_ENCODER:
        enc_pending += e->a;
        if (enc_due == UINT64_MAX) enc_due = lxr_now;
        break;
    case IN_ENC_BUTTON:   /* pulled up: high = released */
        if (e->b) lxr_periph_GPIOE_BASE.IDR &= ~(uint32_t)GPIO_PIN_15; else lxr_periph_GPIOE_BASE.IDR |= GPIO_PIN_15;
        break;
    case IN_ADC:
        if (e->a >= 0 && e->a < 16) {
            adc_values[e->a] = (uint16_t)e->b;
            if (adc_dma && (uint32_t)e->a < adc_len) adc_dma[e->a] = (uint16_t)e->b;
        }
        break;
    }
}

/* -------------------------------------------------------------- dispatcher */
static void dma_event(void)
{
    while (in_tail != in_head) apply_input(&inputs[in_tail++ % INPUTS]);
    /* the round that just finished played what the dma was pointed at: into the fifo */
    if (i2s_buf[0] || i2s_buf[1]) {
        const uint32_t frames = (i2s_size[0] > i2s_size[1] ? i2s_size[0] : i2s_size[1]) / 2u;
        for (uint32_t f = 0; f < frames && fifo_count < FIFO_FRAMES; f++) {
            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < 2; k++)
                if (i2s_buf[k] && f * 2u + 1u < i2s_size[k]) {
                    l += (int16_t)i2s_snap[k][f * 2u] * (1.0f / 32768.0f);
                    r += (int16_t)i2s_snap[k][f * 2u + 1u] * (1.0f / 32768.0f);
                }
            const uint32_t at = (fifo_head + fifo_count) % FIFO_FRAMES;
            fifo_l[at] = l; fifo_r[at] = r;
            fifo_count++;
        }
    }
    dma_rounds++;
    /* enough for the host: pause here, until render() wants more */
    if (fw_state == FW_RUNNING && (int)fifo_count >= fw_want) swapcontext(&fw_ctx, &host_ctx);
    /* transfer complete: SPI2 then SPI3, through the firmware's own callback */
    isr_enter();
    if (i2s_buf[0]) HAL_I2S_TxCpltCallback(&hi2s2);
    if (i2s_buf[1]) HAL_I2S_TxCpltCallback(&hi2s3);
    isr_exit();
}

enum { SRC_TIM14, SRC_SYSTICK, SRC_DMA, SRC_TIM2, SRC_UART_RX, SRC_UART_TX, SRC_ENC, SRC_N };

static uint64_t due_of(int s)
{
    switch (s) {
    case SRC_TIM14: return (vt14.t->DIER & TIM_DIER_UIE) ? vt14.due : UINT64_MAX;
    case SRC_SYSTICK: return systick_on ? systick_due : UINT64_MAX;
    case SRC_DMA: return (i2s_buf[0] || i2s_buf[1]) ? dma_due_cycle(dma_rounds) : UINT64_MAX;
    case SRC_TIM2: return (vt2.t->DIER & TIM_DIER_UIE) ? vt2.due : UINT64_MAX;
    case SRC_UART_RX: return rx_due;
    case SRC_UART_TX: return (USART3->CR1 & USART_CR1_TXEIE) ? (tx_due == UINT64_MAX ? lxr_now : tx_due) : UINT64_MAX;
    case SRC_ENC: return enc_due;
    }
    return UINT64_MAX;
}

static void fire(int s)
{
    switch (s) {
    case SRC_TIM14:
        vt14.base_cnt = 0; vt14.base_cycle = vt14.due; vt14.due = timer_due(&vt14);
        timer_publish(&vt14);
        isr_enter(); TIM8_TRG_COM_TIM14_IRQHandler(); isr_exit();
        break;
    case SRC_SYSTICK:
        systick_due += LXR_HCLK / 1000u;
        isr_enter(); SysTick_Handler(); isr_exit();
        break;
    case SRC_DMA: dma_event(); break;
    case SRC_TIM2:
        vt2.base_cnt = 0; vt2.base_cycle = vt2.due; vt2.due = timer_due(&vt2);
        timer_publish(&vt2);
        isr_enter(); TIM2_IRQHandler(); isr_exit();
        break;
    case SRC_UART_RX: uart_rx_event(); break;
    case SRC_UART_TX: uart_tx_event(); break;
    case SRC_ENC: enc_event(); break;
    }
    timer_sync(&vt2); timer_sync(&vt14);
}

static uint8_t render_flag_last;

void lxr_advance(uint64_t cycles)
{
    /* a block rendered since the last call costs cpu time too */
    if (bCurrentSampleValid == SAMPLE_VALID && render_flag_last != SAMPLE_VALID) cycles += LXR_COST_RENDER;
    render_flag_last = bCurrentSampleValid;

    const uint64_t target = lxr_now + cycles;
    timer_sync(&vt2); timer_sync(&vt14);
    gpio_fold_all();
    if (in_isr || __get_PRIMASK()) { lxr_now = target; timer_publish(&vt2); timer_publish(&vt14); return; }
    for (;;) {
        int best = -1; uint64_t bd = UINT64_MAX;
        for (int s = 0; s < SRC_N; s++) { const uint64_t d = due_of(s); if (d < bd) { bd = d; best = s; } }
        if (best < 0 || bd > target) break;
        if (bd > lxr_now) lxr_now = bd;
        timer_publish(&vt2); timer_publish(&vt14);
        fire(best);
        render_flag_last = bCurrentSampleValid;
    }
    lxr_now = target;
    timer_publish(&vt2); timer_publish(&vt14);
    /* LED_PAGE_DOWN is not in the shift register: LED_43 on PB8 (ledHandler.c) */
    leds[5] = (lxr_periph_GPIOB_BASE.ODR & GPIO_PIN_8) ? 1 : 0;
    if (memcmp(leds, leds_last, 6)) { memcpy(leds_last, leds, 6); led_rev++; }
}

void lxr_board_hook(void) { lxr_advance(LXR_COST_HAL_CALL); }

/* --------------------------------------------- delay.h replacements (dwt) */
static uint64_t nb_target;
uint32_t DWT_Delay_Init(void) { return 0; }      /* the cycle counter always runs here */
void DWT_Delay_us(volatile uint32_t us) { lxr_advance((uint64_t)us * (LXR_HCLK / 1000000u)); }
void DWT_init_nonblock(volatile uint32_t us) { lxr_advance(0); nb_target = lxr_now + (uint64_t)us * (LXR_HCLK / 1000000u); }
uint8_t DWT_check_nonblock(void) { lxr_advance(LXR_COST_POLL); return lxr_now >= nb_target; }

/* ----------------------------------------------------------------- startup */
void lxr_board_init(void)
{   /* called once when the library loads (platform/hal.c) */
    vt2.t = TIM2; vt2.bits32 = 1; vt14.t = TIM14; vt14.bits32 = 0;
    /* input pins at rest. jack detect: 1 = cable in (all outputs plugged).
     * sd detect PD1: 0 = card present. encoder switch PE15 pulled up = released.
     * SW_43 PB7: 0 = released. */
    lxr_periph_GPIOB_BASE.IDR = GPIO_PIN_4 | GPIO_PIN_6;
    lxr_periph_GPIOD_BASE.IDR = GPIO_PIN_6 | GPIO_PIN_7;
    lxr_periph_GPIOE_BASE.IDR = GPIO_PIN_15;
    enc_set_pins();
    /* pots: both wipers mid; faders 8..13 fully up */
    for (int i = 0; i < 8; i++) adc_values[i] = 0x8000u;
    for (int i = 8; i < 14; i++) adc_values[i] = 0xFFF0u;
    memset(lcd_ddram, ' ', sizeof lcd_ddram);
}

/* ------------------------------------------------------------ the host side */
static void m_card_write(uint32_t s, const uint8_t *d) { card_put(s, d); }
static void m_card_read(uint32_t s, uint8_t *d) { card_get(s, d); }
static uint32_t m_card_next(uint32_t from)
{
    while (from < VOLT_CARD_SECTORS && !card[from]) from++;
    return from;
}
static uint32_t m_card_version(void) { return card_rev; }

static int m_boot(void)
{
    if (fw_state != FW_IDLE) return fw_state == FW_RUNNING ? 0 : -1;
    if (!(fw_stack = malloc(FW_STACK_BYTES))) return -1;
    getcontext(&fw_ctx);
    fw_ctx.uc_stack.ss_sp = fw_stack;
    fw_ctx.uc_stack.ss_size = FW_STACK_BYTES;
    fw_ctx.uc_link = NULL;
    makecontext(&fw_ctx, firmware_entry, 0);
    fw_state = FW_RUNNING;
    fw_want = 1;                       /* until the first block: the boot */
    swapcontext(&host_ctx, &fw_ctx);
    fifo_head = (fifo_head + fifo_count) % FIFO_FRAMES;   /* the boot's block: not heard */
    fifo_count = 0;
    return fw_state == FW_RUNNING ? 0 : -1;
}

static void m_render(float *left, float *right, int frames)
{
    if (frames <= 0) return;
    if (fw_state == FW_RUNNING && frames <= (int)FIFO_FRAMES - 64) {
        fw_want = frames;
        while ((int)fifo_count < frames && fw_state == FW_RUNNING) swapcontext(&host_ctx, &fw_ctx);
    }
    for (int i = 0; i < frames; i++) {
        if (fifo_count) {
            left[i] = fifo_l[fifo_head]; right[i] = fifo_r[fifo_head];
            fifo_head = (fifo_head + 1u) % FIFO_FRAMES; fifo_count--;
        } else {
            left[i] = right[i] = 0.0f;
        }
    }
}

static void m_button(int index, int down) { input e = { IN_BUTTON, index, down != 0, { 0 }, 0 }; queue_input(&e); }
static void m_adc(int index, int value) { input e = { IN_ADC, index, value, { 0 }, 0 }; queue_input(&e); }
static void m_encoder(int detents) { if (detents) { input e = { IN_ENCODER, detents, 0, { 0 }, 0 }; queue_input(&e); } }
static void m_encoder_button(int down) { input e = { IN_ENC_BUTTON, 0, down != 0, { 0 }, 0 }; queue_input(&e); }
static void m_midi(const uint8_t *bytes, int count)
{
    while (count > 0) {
        input e = { IN_MIDI, 0, 0, { 0 }, 0 };
        e.n = (uint8_t)(count > 16 ? 16 : count);
        memcpy(e.bytes, bytes, e.n);
        queue_input(&e);
        bytes += e.n; count -= e.n;
    }
}
static const uint8_t *m_lcd(void) { return lcd_ddram; }
static const uint8_t *m_lcd_glyphs(void) { return lcd_cgram; }
static uint32_t m_lcd_version(void) { return lcd_rev; }
static const uint8_t *m_leds(void) { return leds_last; }
static uint32_t m_led_version(void) { return led_rev; }

static const VoltMachineApi api = {
    VOLT_MACHINE_API_VERSION,
    m_card_write, m_card_read, m_card_next, m_card_version,
    m_boot, m_render,
    m_button, m_adc, m_encoder, m_encoder_button, m_midi,
    m_lcd, m_lcd_glyphs, m_lcd_version, m_leds, m_led_version,
};

__attribute__((visibility("default"))) const VoltMachineApi *volt_machine_api(void) { return &api; }
