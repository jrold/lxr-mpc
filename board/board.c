/*
 * board.c: VOLTKIT's virtual LXR-02 board, written from ST's RM0410
 * (STM32F76xxx reference manual), the HD44780 datasheet and the LXR firmware
 * itself (firmware/: what each part of the panel is wired to, main.h).
 *
 * Time is the core's cycle count. The firmware runs in its own context (a
 * coroutine): render() resumes it, and it hands back at the end of a main
 * loop pass, or inside HAL_Delay, once the time render() asked for has
 * passed. Interrupts are delivered at their exact times whenever time
 * passes in the main context (between passes, in HAL_Delay and polls of
 * HAL_GetTick), never inside another interrupt or while PRIMASK is set.
 *
 * The peripherals are register blocks in memory (volt_periph()); the board
 * sees every access a firmware source line makes and settles what the last
 * one did: GPIO set/reset writes reach the output register, the display's
 * enable line is watched, the cycle counter moves on, the timers count.
 *
 * The panel, as the firmware's drivers use it:
 *   keys       40 behind shift registers on SPI1 (DIO.c), PAGE DOWN on PB7
 *   LEDs       40 on the same chain's outputs, PAGE DOWN's on PB8
 *   display    HD44780, 4-bit bus PE7..PE10, E on PE11, RS on PE12 (lcd.c)
 *   encoder    phases on PE13/PE14, push on PE15 (active low; encoder.c)
 *   knobs      4 endless pots, two wipers each, ADC1 ranks 1..8 (adcPots.c)
 *   faders     6, ADC1 ranks 9..14
 *   MIDI in    USART3 at 31250 baud
 *   audio      I2S2 and I2S3, each fed blocks by DMA1 streams 4 and 5
 *   SD card    SDMMC1, card detect on PD1 (low: present)
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#define _XOPEN_SOURCE 600
#include "board.h"
#include "volt_machine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* ---- the firmware: entry, interrupt handlers, panel helpers (engine/volt_panel.c) */
extern void SystemInit(void);
extern int lxr_firmware_main(void);
extern void SysTick_Handler(void);
extern void TIM8_TRG_COM_TIM14_IRQHandler(void);
extern void TIM2_IRQHandler(void);
extern void DMA1_Stream4_IRQHandler(void);
extern void DMA1_Stream5_IRQHandler(void);
extern void USART3_IRQHandler(void);
int volt_panel_knob(int index, VoltKnob *out);

/* ---- clocks and time */
volatile uint32_t volt_primask;
uint32_t volt_hclk = 16000000u;   /* HSI until the firmware configures the PLL */
uint32_t volt_apb1_div = 1, volt_apb2_div = 1;
uint32_t volt_i2s_clk;
uint64_t volt_now;

#define PASS_US        100u   /* a main-loop pass, as time on the board */
#define PASS_BUSY_US   20u    /* ... while a knob is still turning */
#define DWT_POLL_CYCLES 32u   /* each look at the cycle counter */
#define TICK_POLL_US   1u     /* each HAL_GetTick() */
#define BOOT_LIMIT_S   60u
#define MIDI_BYTE_US   320u   /* 10 bits at 31250 baud */

uint64_t volt_cycles_per_ms(void) { return volt_hclk / 1000u; }
static uint64_t us(uint64_t n) { return n * (volt_hclk / 1000000u); }

/* ---- register blocks */
static uint8_t *dev_regs[VP_DEVICE_COUNT];
static uint8_t *core_regs[VP_CORE_END - VP_CORE_FIRST];

static void *regs(int id, unsigned size)
{
    uint8_t **slot = id >= VP_CORE_FIRST ? &core_regs[id - VP_CORE_FIRST] : &dev_regs[id];
    if (!*slot)
        *slot = calloc(1, size + 64u);
    return *slot;
}
#define REG(id, T) ((T *)regs(VP_##id, sizeof(T)))

static void on_access(int id);

void *volt_periph(int id, unsigned size)
{
    void *r = regs(id, size);
    on_access(id);
    return r;
}

/* ---- the firmware's context */
static ucontext_t host_ctx, fw_ctx;
static void *fw_stack;
static int fw_alive, fw_running, booting, booted, boot_failed, in_isr;
static uint64_t run_until, boot_limit;

static void fw_entry(void)
{
    SystemInit();
    lxr_firmware_main();
    fw_alive = 0;                 /* main() returned: the machine stops */
    fw_running = 0;
    setcontext(&host_ctx);
}

static void yield_to_host(void)
{
    fw_running = 0;
    swapcontext(&fw_ctx, &host_ctx);
    fw_running = 1;
}

static void resume_firmware(void)
{
    if (!fw_alive)
        return;
    fw_running = 1;
    swapcontext(&host_ctx, &fw_ctx);
    fw_running = 0;
}

/* ================================================================ display: HD44780 */

static struct
{
    uint8_t ddram[128];
    uint8_t cgram[64];
    uint8_t ac;          /* address counter */
    uint8_t cg;          /* 1: the counter points into CGRAM */
    uint8_t dec;         /* entry mode: 1 decrements */
    uint8_t four_bit;
    uint8_t half;        /* 4-bit mode: the high nibble came */
    uint8_t high;
    uint8_t e;           /* enable line as last seen */
} lcd;
static uint32_t lcd_version;

static uint8_t ddram_next(uint8_t a, int dec)
{
    /* two lines: 0x00..0x27 and 0x40..0x67, each wrapping into the other */
    if (!dec)
        return a == 0x27 ? 0x40 : a == 0x67 ? 0x00 : (uint8_t)(a + 1);
    return a == 0x00 ? 0x67 : a == 0x40 ? 0x27 : (uint8_t)(a - 1);
}

static void lcd_byte(int rs, uint8_t b)
{
    if (rs)
    {
        if (lcd.cg)
        {
            lcd.cgram[lcd.ac & 0x3F] = b & 0x1F;
            lcd.ac = (uint8_t)((lcd.ac + (lcd.dec ? -1 : 1)) & 0x3F);
        }
        else
        {
            lcd.ddram[lcd.ac & 0x7F] = b;
            lcd.ac = ddram_next(lcd.ac & 0x7F, lcd.dec);
        }
        lcd_version++;
        return;
    }
    if (b & 0x80)        /* set DDRAM address */
    {
        lcd.ac = b & 0x7F;
        lcd.cg = 0;
    }
    else if (b & 0x40)   /* set CGRAM address */
    {
        lcd.ac = b & 0x3F;
        lcd.cg = 1;
    }
    else if (b & 0x20)   /* function set: DL (bit 4) chooses the bus width */
    {
        lcd.four_bit = !(b & 0x10);
        lcd.half = 0;
    }
    else if (b & 0x10)   /* cursor or display shift (the OLED's power bits): no change to the text */
    {
    }
    else if (b & 0x08)   /* display on/off control */
    {
    }
    else if (b & 0x04)   /* entry mode set */
        lcd.dec = !(b & 0x02);
    else if (b & 0x02)   /* return home */
    {
        lcd.ac = 0;
        lcd.cg = 0;
    }
    else if (b & 0x01)   /* clear display */
    {
        memset(lcd.ddram, ' ', sizeof lcd.ddram);
        lcd.ac = 0;
        lcd.cg = 0;
        lcd.dec = 0;
        lcd_version++;
    }
}

/* E rising: the bus is read (it is held through the pulse, so this is what
 * the falling edge latches). */
static void lcd_strobe(int rs, uint8_t nibble)
{
    if (!lcd.four_bit)
    {
        lcd_byte(rs, (uint8_t)(nibble << 4));
        return;
    }
    if (!lcd.half)
    {
        lcd.high = nibble;
        lcd.half = 1;
        return;
    }
    lcd.half = 0;
    lcd_byte(rs, (uint8_t)(lcd.high << 4 | nibble));
}

/* ================================================================ panel state */

static uint8_t keys[5];           /* shift-register inputs: bit = key down */
static uint8_t page_down_key;
static struct { uint8_t key, down; } key_queue[256];
static unsigned key_head, key_tail;

static uint8_t leds[6];
static uint32_t led_version;

static int enc_pending;           /* detents still to turn */
static int enc_phase;             /* 0..3 along 00 -> 01 -> 11 -> 10 */
static uint64_t enc_push_until;

static uint16_t adc_value[14];    /* 12-bit readings by rank */
static uint16_t *adc_dma;
static uint32_t adc_dma_len, adc_left_aligned;

static double pot_angle[VOLT_KNOBS];
static int knob_target[VOLT_KNOBS], knob_active[VOLT_KNOBS], knob_last[VOLT_KNOBS];
static int knob_stall[VOLT_KNOBS], knob_dir[VOLT_KNOBS];

static uint8_t midi_queue[4096];
static unsigned midi_head, midi_tail;
static uint64_t midi_next;

/* ================================================================ GPIO */

static GPIO_TypeDef *port(int id) { return (GPIO_TypeDef *)regs(id, sizeof(GPIO_TypeDef)); }

static const int gpio_ports[] = { VP_GPIOA, VP_GPIOB, VP_GPIOC, VP_GPIOD, VP_GPIOE, VP_GPIOF,
                                  VP_GPIOG, VP_GPIOH, VP_GPIOI, VP_GPIOJ, VP_GPIOK };

void volt_board_gpio_settle(void)
{
    for (unsigned i = 0; i < sizeof gpio_ports / sizeof gpio_ports[0]; i++)
    {
        GPIO_TypeDef *g = port(gpio_ports[i]);
        const uint32_t bsrr = g->BSRR;
        if (bsrr)   /* RM0410 6.4.7: set bits win over reset bits */
        {
            g->ODR = (g->ODR & ~(bsrr >> 16)) | (bsrr & 0xFFFFu);
            g->BSRR = 0;
        }
    }

    /* inputs */
    static const uint8_t phases[4] = { 0, 1, 3, 2 };   /* A<<1 | B */
    const uint8_t ph = phases[enc_phase & 3];
    port(VP_GPIOE)->IDR = (uint32_t)((ph >> 1) & 1) << 13 | (uint32_t)(ph & 1) << 14
                          | (volt_now < enc_push_until ? 0u : 1u << 15);
    port(VP_GPIOD)->IDR = 0;                                      /* PD1 low: card present */
    port(VP_GPIOB)->IDR = (uint32_t)page_down_key << 7;

    /* the display's bus */
    const uint32_t e_odr = port(VP_GPIOE)->ODR;
    const uint8_t e = (e_odr >> 11) & 1;
    if (e && !lcd.e)
        lcd_strobe((int)((e_odr >> 12) & 1), (uint8_t)((e_odr >> 7) & 0x0F));
    lcd.e = e;

    /* PAGE DOWN's LED */
    const uint8_t pd = (port(VP_GPIOB)->ODR >> 8) & 1;
    if (pd != leds[5])
    {
        leds[5] = pd;
        led_version++;
    }
}

/* ================================================================ timers (RM0410 26-29) */

typedef struct
{
    int id;
    IRQn_Type irq;
    void (*isr)(void);
    uint32_t max;            /* the counter's top: 16 or 32 bits */
    int running;
    uint64_t base_time;      /* counter == base_cnt at base_time */
    uint32_t base_cnt;
    uint32_t cnt_shown;      /* what the board last wrote to CNT */
    uint64_t next;           /* the next update event */
} Timer;

static Timer timers[2];      /* TIM14 (the firmware's 4 kHz tick), TIM2 (samples while saving) */
static uint8_t nvic_on[128];

static TIM_TypeDef *tim_regs(const Timer *t) { return (TIM_TypeDef *)regs(t->id, sizeof(TIM_TypeDef)); }

/* Core cycles per counter tick: APB1 timers run at twice PCLK1 when APB1 is divided. */
static uint64_t tim_cycles_per_tick(const Timer *t)
{
    const uint64_t pre = tim_regs(t)->PSC + 1u;
    return volt_apb1_div == 1 ? pre : pre * volt_apb1_div / 2u;
}

static uint32_t tim_count(const Timer *t)
{
    const uint64_t ticks = (volt_now - t->base_time) / tim_cycles_per_tick(t);
    const uint32_t arr = tim_regs(t)->ARR;
    if (t->base_cnt <= arr)
        return (uint32_t)((t->base_cnt + ticks) % ((uint64_t)arr + 1u));
    /* above the reload value: up to the top, then from 0 */
    const uint64_t to_top = (uint64_t)t->max - t->base_cnt + 1u;
    return ticks < to_top ? (uint32_t)(t->base_cnt + ticks) : (uint32_t)((ticks - to_top) % ((uint64_t)arr + 1u));
}

static void tim_schedule(Timer *t)
{
    const uint32_t arr = tim_regs(t)->ARR, c = t->base_cnt;
    const uint64_t ticks = c <= arr ? (uint64_t)arr - c + 1u : (uint64_t)t->max - c + 1u;
    t->next = t->base_time + ticks * tim_cycles_per_tick(t);
}

/* Before the firmware looks at a timer: show the live count; a count the
 * firmware wrote since starts it counting from there. */
static void tim_sync(Timer *t)
{
    TIM_TypeDef *r = tim_regs(t);
    if (r->CNT != t->cnt_shown)
    {
        t->base_time = volt_now;
        t->base_cnt = r->CNT;
        tim_schedule(t);
    }
    r->CNT = t->cnt_shown = tim_count(t);
}

void volt_board_timer_started(TIM_TypeDef *tim)
{
    for (int i = 0; i < 2; i++)
        if (tim_regs(&timers[i]) == tim)
        {
            Timer *t = &timers[i];
            t->running = 1;
            t->base_time = volt_now;
            t->base_cnt = t->cnt_shown = tim->CNT;
            tim_schedule(t);
        }
}

static void tim_fire(Timer *t)
{
    TIM_TypeDef *r = tim_regs(t);
    if (r->CNT != t->cnt_shown)   /* the firmware moved the counter: no update yet */
    {
        t->base_time = volt_now;
        t->base_cnt = t->cnt_shown = r->CNT;
        tim_schedule(t);
        return;
    }
    t->base_time = t->next;
    t->base_cnt = 0;
    r->CNT = t->cnt_shown = 0;
    tim_schedule(t);
    if (!(r->CR1 & TIM_CR1_CEN))
    {
        t->running = 0;
        return;
    }
    r->SR |= TIM_SR_UIF;
    if ((r->DIER & TIM_DIER_UIE) && nvic_on[t->irq])
        t->isr();
}

void volt_board_nvic(IRQn_Type irq, int enabled)
{
    if ((int)irq >= 0 && (int)irq < 128)
        nvic_on[irq] = (uint8_t)enabled;
}

/* ================================================================ SysTick */

static uint64_t systick_next;

uint32_t volt_core_systick_config(uint32_t ticks)
{
    SysTick_Type *s = (SysTick_Type *)regs(VP_SYSTICK, sizeof(SysTick_Type));
    if (ticks == 0 || ticks - 1u > SysTick_LOAD_RELOAD_Msk)
        return 1u;
    s->LOAD = ticks - 1u;
    s->VAL = 0;
    s->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
    systick_next = volt_now + ticks;
    return 0u;
}

void volt_core_system_reset(void)
{
    fw_alive = 0;    /* the firmware asked for a reset: this machine stops */
    yield_to_host();
}

static int systick_on(void)
{
    const SysTick_Type *s = (SysTick_Type *)regs(VP_SYSTICK, sizeof(SysTick_Type));
    return (s->CTRL & SysTick_CTRL_ENABLE_Msk) && (s->CTRL & SysTick_CTRL_TICKINT_Msk);
}

static void systick_fire(void)
{
    const SysTick_Type *s = (SysTick_Type *)regs(VP_SYSTICK, sizeof(SysTick_Type));
    systick_next += s->LOAD + 1u;
    /* the encoder moves a quarter step between the firmware's 1 ms samples */
    if (enc_pending > 0)
    {
        enc_phase = (enc_phase + 1) & 3;
        if (enc_phase == 0)
            enc_pending--;
    }
    else if (enc_pending < 0)
    {
        enc_phase = (enc_phase + 3) & 3;
        if (enc_phase == 0)
            enc_pending++;
    }
    volt_board_gpio_settle();
    SysTick_Handler();
}

/* ================================================================ audio: I2S by DMA */

#define FIFO_FRAMES 8192u

typedef struct
{
    I2S_HandleTypeDef *h;
    int active;
    int completing;          /* inside this DAC's completion: the next block starts where it ended */
    uint64_t end;            /* when the block being sent finishes */
    uint64_t end_frac;       /* ... and the fraction of a cycle (32 bits) */
    float fifo[FIFO_FRAMES][2];
    unsigned head, count;
} Dac;

static Dac dacs[2];          /* I2S2 (the main output), I2S3 (output 2) */
static uint8_t dma_done[2];

void volt_board_i2s_transmit(I2S_HandleTypeDef *hi2s, const uint16_t *data, uint16_t size, uint32_t fs)
{
    Dac *d = &dacs[hi2s->Instance == SPI2 ? 0 : 1];
    const unsigned frames = size / 2u;
    d->h = hi2s;
    if (!d->completing || !d->active)
    {
        d->end = volt_now;
        d->end_frac = 0;
    }
    /* the block's samples, as the DMA will send them */
    for (unsigned i = 0; i < frames; i++)
    {
        const unsigned at = (d->head + d->count) % FIFO_FRAMES;
        d->fifo[at][0] = (int16_t)data[2 * i] / 32768.0f;
        d->fifo[at][1] = (int16_t)data[2 * i + 1] / 32768.0f;
        if (d->count < FIFO_FRAMES)
            d->count++;
        else
            d->head = (d->head + 1) % FIFO_FRAMES;
    }
    const uint64_t q = ((uint64_t)volt_hclk << 32) / (fs ? fs : 44100u) * frames + d->end_frac;
    d->end += q >> 32;
    d->end_frac = q & 0xFFFFFFFFu;
    d->active = 1;
}

int volt_board_dma_take_complete(DMA_HandleTypeDef *hdma)
{
    for (int i = 0; i < 2; i++)
        if (dacs[i].h && dacs[i].h->hdmatx == hdma && dma_done[i])
        {
            dma_done[i] = 0;
            return 1;
        }
    return 0;
}

static void dac_fire(Dac *d)
{
    const int i = (int)(d - dacs);
    d->active = 0;
    dma_done[i] = 1;
    d->completing = 1;
    const DMA_Stream_TypeDef *stream = d->h->hdmatx ? d->h->hdmatx->Instance : NULL;
    if (stream == (DMA_Stream_TypeDef *)regs(VP_DMA1_Stream4, sizeof(DMA_Stream_TypeDef)) && nvic_on[DMA1_Stream4_IRQn])
        DMA1_Stream4_IRQHandler();
    else if (stream == (DMA_Stream_TypeDef *)regs(VP_DMA1_Stream5, sizeof(DMA_Stream_TypeDef)) && nvic_on[DMA1_Stream5_IRQn])
        DMA1_Stream5_IRQHandler();
    d->completing = 0;
}

/* ================================================================ MIDI in: USART3 */

static void midi_fire(void)
{
    USART_TypeDef *u = (USART_TypeDef *)regs(VP_USART3, sizeof(USART_TypeDef));
    const uint8_t b = midi_queue[midi_head];
    midi_head = (midi_head + 1) % sizeof midi_queue;
    midi_next = volt_now + us(MIDI_BYTE_US);
    if (!(u->CR1 & USART_CR1_UE) || !(u->CR1 & USART_CR1_RXNEIE) || !nvic_on[USART3_IRQn])
        return;   /* nothing listening: the byte is lost, as on the wire */
    u->RDR = b;
    u->ISR |= USART_ISR_RXNE;
    USART3_IRQHandler();
    u->ISR &= ~USART_ISR_RXNE;
}

/* ================================================================ events */

static uint64_t next_event(void)
{
    uint64_t t = UINT64_MAX;
    if (systick_on() && systick_next < t)
        t = systick_next;
    for (int i = 0; i < 2; i++)
    {
        if (timers[i].running && timers[i].next < t)
            t = timers[i].next;
        if (dacs[i].active && dacs[i].end < t)
            t = dacs[i].end;
    }
    if (midi_head != midi_tail && midi_next < t)
        t = midi_next;
    return t;
}

static void deliver_due(void)
{
    if (in_isr || volt_primask)
        return;
    for (;;)
    {
        const uint64_t t = next_event();
        if (t > volt_now)
            return;
        in_isr = 1;
        if (systick_on() && systick_next == t)
            systick_fire();
        else if (midi_head != midi_tail && midi_next == t)
            midi_fire();
        else
        {
            int done = 0;
            for (int i = 0; i < 2 && !done; i++)
                if (dacs[i].active && dacs[i].end == t)
                {
                    dac_fire(&dacs[i]);
                    done = 1;
                }
            for (int i = 0; i < 2 && !done; i++)
                if (timers[i].running && timers[i].next == t)
                {
                    tim_fire(&timers[i]);
                    done = 1;
                }
        }
        in_isr = 0;
    }
}

void volt_board_spend(uint64_t cycles)
{
    if (in_isr)
    {
        volt_now += cycles;
        return;
    }
    const uint64_t target = volt_now + cycles;
    while (volt_now < target)
    {
        const uint64_t t = next_event();
        volt_now = t < target ? (t > volt_now ? t : volt_now) : target;
        deliver_due();
        if (booting && volt_now > boot_limit && fw_running)
        {
            boot_failed = 1;
            yield_to_host();
        }
        if (!booting && fw_running && volt_now >= run_until)
            yield_to_host();
    }
    deliver_due();
}

/* ================================================================ per access */

static void on_access(int id)
{
    if ((id >= VP_GPIOA && id <= VP_GPIOK) || id == VP_DWT)
        volt_board_gpio_settle();
    if (id == VP_DWT)
    {
        DWT_Type *d = (DWT_Type *)regs(VP_DWT, sizeof(DWT_Type));
        volt_now += DWT_POLL_CYCLES;
        d->CYCCNT = (uint32_t)volt_now;
    }
    for (int i = 0; i < 2; i++)
        if (id == timers[i].id && timers[i].running)
            tim_sync(&timers[i]);
}

/* ================================================================ SPI1: keys and LEDs */

void volt_board_spi_receive(SPI_TypeDef *spi, uint8_t *data, uint16_t size)
{
    if (spi == SPI1)
        for (uint16_t i = 0; i < size; i++)
            data[i] = i < sizeof keys ? keys[i] : 0;
    else
        memset(data, 0, size);
}

void volt_board_spi_transmit(SPI_TypeDef *spi, const uint8_t *data, uint16_t size)
{
    if (spi != SPI1)
        return;
    for (uint16_t i = 0; i < size && i < 5; i++)
        if (leds[i] != data[i])
        {
            leds[i] = data[i];
            led_version++;
        }
}

/* ================================================================ ADC1: knobs and faders */

void volt_board_adc_dma(ADC_HandleTypeDef *hadc, uint16_t *buffer, uint32_t length)
{
    adc_dma = buffer;
    adc_dma_len = length;
    adc_left_aligned = hadc->Init.DataAlign == ADC_DATAALIGN_LEFT;
}

/* An endless pot's wipers at angle th: two triangles 90 degrees apart, as
 * adcPots.c reads them back (calcPosition(): wiper A above half scale says
 * which half of the turn wiper B is in). */
static void pot_wipers(double th, uint16_t *a, uint16_t *b)
{
    th = fmod(fmod(th, 360.0) + 360.0, 360.0);
    const double wa = th <= 270.0 ? 180.0 - fabs(th - 90.0) : th - 270.0;
    const double wb = th <= 180.0 ? th : 360.0 - th;
    *a = (uint16_t)lround(wa / 180.0 * 4095.0);
    *b = (uint16_t)lround(wb / 180.0 * 4095.0);
}

static void set_pot(int k)
{
    pot_wipers(pot_angle[k], &adc_value[2 * k], &adc_value[2 * k + 1]);
}

static void adc_write(void)
{
    if (!adc_dma)
        return;
    for (uint32_t i = 0; i < adc_dma_len && i < 14; i++)
        adc_dma[i] = adc_left_aligned ? (uint16_t)(adc_value[i] << 4) : adc_value[i];
}

#define KNOB_STEP_DEG 12.0

/* One step of a knob that is going somewhere: watch its parameter and turn
 * the pot a little more toward the target (one step registers per pass;
 * a turn across the top or bottom of wiper A's triangle is cut there, so
 * the firmware sees A move). */
static int knob_step(int k)
{
    VoltKnob info;
    if (!knob_active[k])
        return 0;
    if (!volt_panel_knob(k, &info) || info.value == knob_target[k])
    {
        knob_active[k] = 0;
        return 0;
    }
    if (info.value == knob_last[k])
    {
        if (++knob_stall[k] > 12)   /* it won't go further (an end, or a menu that stops) */
        {
            knob_active[k] = 0;
            return 0;
        }
    }
    else
    {
        /* moved the wrong way: this knob counts the other way round */
        if ((info.value - knob_last[k] > 0) != (knob_target[k] - knob_last[k] > 0))
            knob_dir[k] = -knob_dir[k];
        knob_stall[k] = 0;
    }
    knob_last[k] = info.value;
    const double sign = (knob_target[k] > info.value ? 1.0 : -1.0) * knob_dir[k];
    const double from = pot_angle[k];
    double to = from + sign * KNOB_STEP_DEG;
    const double peaks[] = { 90.0, 270.0, 450.0, -90.0 };
    for (int i = 0; i < 4; i++)
        if ((from < peaks[i] && to > peaks[i]) || (from > peaks[i] && to < peaks[i]))
            to = peaks[i];
    pot_angle[k] = fmod(to + 360.0, 360.0);
    set_pot(k);
    return 1;
}

/* ================================================================ each main-loop pass */

static void apply_keys(void)
{
    uint8_t touched[VOLT_BUTTONS] = { 0 };
    while (key_head != key_tail)
    {
        const unsigned k = key_queue[key_head].key, down = key_queue[key_head].down;
        if (touched[k])
            break;   /* the firmware's next scan sees the next change of this key */
        touched[k] = 1;
        key_head = (key_head + 1) % (sizeof key_queue / sizeof key_queue[0]);
        if (k == 40)
            page_down_key = (uint8_t)down;
        else if (down)
            keys[k / 8] |= (uint8_t)(1u << (k % 8));
        else
            keys[k / 8] &= (uint8_t)~(1u << (k % 8));
    }
}

void volt_board_main_pass(void)
{
    if (!booted)
    {
        booted = 1;
        yield_to_host();   /* boot() returns: the firmware is at its main loop */
    }
    apply_keys();
    int busy = 0;
    for (int k = 0; k < VOLT_KNOBS; k++)
        busy |= knob_step(k);
    adc_write();
    volt_board_gpio_settle();
    volt_board_spend(us(busy ? PASS_BUSY_US : PASS_US));
}

/* ================================================================ HAL helpers */

uint32_t volt_board_random(void)
{
    static uint32_t x = 0x2545F491u;   /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

uint32_t HAL_GetTick(void)
{
    if (fw_running && !in_isr)
        volt_board_spend(us(TICK_POLL_US));   /* a poll takes time, so a wait for the tick ends */
    return uwTick;
}

void HAL_Delay(uint32_t ms)
{
    volt_board_spend((uint64_t)ms * volt_cycles_per_ms());
}

/* ================================================================ the card */

static uint8_t *card[VOLT_CARD_SECTORS];
static uint32_t card_ver;

uint32_t volt_card_sectors(void) { return VOLT_CARD_SECTORS; }

void volt_card_read(uint32_t s, uint8_t *data)
{
    if (s < VOLT_CARD_SECTORS && card[s])
        memcpy(data, card[s], VOLT_SECTOR_BYTES);
    else
        memset(data, 0, VOLT_SECTOR_BYTES);
}

void volt_card_write(uint32_t s, const uint8_t *data)
{
    if (s >= VOLT_CARD_SECTORS)
        return;
    int zero = 1;
    for (unsigned i = 0; i < VOLT_SECTOR_BYTES && zero; i++)
        zero = data[i] == 0;
    if (zero)
    {
        free(card[s]);
        card[s] = NULL;
    }
    else
    {
        if (!card[s])
            card[s] = malloc(VOLT_SECTOR_BYTES);
        memcpy(card[s], data, VOLT_SECTOR_BYTES);
    }
    card_ver++;
}

/* ================================================================ the machine */

static void api_card_write(uint32_t s, const uint8_t *data) { volt_card_write(s, data); }
static void api_card_read(uint32_t s, uint8_t *data) { volt_card_read(s, data); }
static uint32_t api_card_version(void) { return card_ver; }

static uint32_t api_card_next(uint32_t from)
{
    while (from < VOLT_CARD_SECTORS && !card[from])
        from++;
    return from;
}

static int api_boot(void)
{
    if (fw_stack)
        return booted && fw_alive ? 0 : -1;
    for (int k = 0; k < VOLT_KNOBS; k++)
    {
        pot_angle[k] = 180.0;
        knob_dir[k] = 1;
        set_pot(k);
    }
    for (int f = 0; f < VOLT_FADERS; f++)
        adc_value[8 + f] = 4095;
    memset(lcd.ddram, ' ', sizeof lcd.ddram);
    timers[0] = (Timer){ .id = VP_TIM14, .irq = TIM8_TRG_COM_TIM14_IRQn, .isr = TIM8_TRG_COM_TIM14_IRQHandler,
                         .max = 0xFFFFu };
    timers[1] = (Timer){ .id = VP_TIM2, .irq = TIM2_IRQn, .isr = TIM2_IRQHandler, .max = 0xFFFFFFFFu };

    const size_t stack = 1u << 20;
    fw_stack = malloc(stack);
    getcontext(&fw_ctx);
    fw_ctx.uc_stack.ss_sp = fw_stack;
    fw_ctx.uc_stack.ss_size = stack;
    fw_ctx.uc_link = NULL;
    makecontext(&fw_ctx, fw_entry, 0);
    fw_alive = 1;
    booting = 1;
    boot_limit = (uint64_t)BOOT_LIMIT_S * 216000000u;
    resume_firmware();
    booting = 0;
    if (boot_failed || !booted || !fw_alive)
    {
        fw_alive = 0;
        return -1;
    }
    for (int i = 0; i < 2; i++)   /* what played while it booted */
        dacs[i].count = 0;
    run_until = volt_now;
    return 0;
}

static void api_render(float *left, float *right, int frames)
{
    if (fw_alive && booted)
    {
        /* frames of the DACs' own clock: the I2S rate the firmware set up */
        const uint64_t q = ((uint64_t)volt_hclk << 32) / (volt_i2s_fs ? volt_i2s_fs : 44100u) * (uint64_t)frames;
        static uint64_t frac;
        const uint64_t step = q + frac;
        frac = step & 0xFFFFFFFFu;
        run_until += step >> 32;   /* the host's clock: a pass that runs over is not counted twice */
        while (fw_alive && volt_now < run_until)
            resume_firmware();
    }
    for (int i = 0; i < frames; i++)
    {
        float l = 0.0f, r = 0.0f;
        for (int d = 0; d < 2; d++)
            if (dacs[d].count)
            {
                l += dacs[d].fifo[dacs[d].head][0];
                r += dacs[d].fifo[dacs[d].head][1];
                dacs[d].head = (dacs[d].head + 1) % FIFO_FRAMES;
                dacs[d].count--;
            }
        left[i] = l;
        right[i] = r;
    }
}

static void api_key(int index, int down)
{
    if (index < 0 || index >= VOLT_BUTTONS)
        return;
    const unsigned next = (key_tail + 1) % (sizeof key_queue / sizeof key_queue[0]);
    if (next == key_head)
        return;
    key_queue[key_tail].key = (uint8_t)index;
    key_queue[key_tail].down = (uint8_t)(down != 0);
    key_tail = next;
}

static void api_tap(int index)
{
    api_key(index, 1);
    api_key(index, 0);
}

static void api_encoder(int detents)
{
    enc_pending += detents;
    if (enc_pending > 64)
        enc_pending = 64;
    if (enc_pending < -64)
        enc_pending = -64;
}

static void api_encoder_push(void)
{
    /* sampled every 10 ms, accepted after 4 equal samples (encoder.c) */
    enc_push_until = volt_now + 60u * volt_cycles_per_ms();
}

static void api_fader(int index, int value)
{
    if (index >= 0 && index < VOLT_FADERS)
        adc_value[8 + index] = (uint16_t)(value < 0 ? 0 : value > 4095 ? 4095 : value);
}

static int api_knob(int index, VoltKnob *out)
{
    if (index < 0 || index >= VOLT_KNOBS || !booted || !fw_alive)
        return 0;
    return volt_panel_knob(index, out);
}

static void api_knob_to(int index, int value)
{
    VoltKnob info;
    if (!api_knob(index, &info))
        return;
    if (value < info.min)
        value = info.min;
    if (value > info.max)
        value = info.max;
    knob_target[index] = value;
    knob_last[index] = info.value;
    knob_stall[index] = 0;
    knob_active[index] = value != info.value;
}

static void api_midi(const uint8_t *bytes, int count)
{
    if (midi_head == midi_tail && midi_next < volt_now)
        midi_next = volt_now;
    for (int i = 0; i < count; i++)
    {
        const unsigned next = (midi_tail + 1) % sizeof midi_queue;
        if (next == midi_head)
            return;
        midi_queue[midi_tail] = bytes[i];
        midi_tail = next;
    }
}

static const uint8_t *api_lcd(void) { return lcd.ddram; }
static const uint8_t *api_lcd_glyphs(void) { return lcd.cgram; }
static uint32_t api_lcd_version(void) { return lcd_version; }
static const uint8_t *api_leds(void) { return leds; }
static uint32_t api_led_version(void) { return led_version; }

__attribute__((visibility("default"))) const VoltMachineApi *volt_machine_api(void)
{
    static const VoltMachineApi api = {
        VOLT_MACHINE_API_VERSION,
        api_card_write, api_card_read, api_card_next, api_card_version,
        api_boot, api_render,
        api_key, api_tap, api_encoder, api_encoder_push, api_fader, api_knob, api_knob_to, api_midi,
        api_lcd, api_lcd_glyphs, api_lcd_version, api_leds, api_led_version,
    };
    return &api;
}
