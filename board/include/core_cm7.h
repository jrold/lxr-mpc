/*
 * core_cm7.h for VOLTKIT's board: the Cortex-M7 core as the LXR firmware and
 * ST's HAL headers use it, on a host computer. Found before CMSIS's own
 * core_cm7.h (board/include comes first on the include path), which reaches
 * the core's registers at fixed addresses and its instructions through ARM
 * assembly.
 *
 * - The core peripherals (SCB, SysTick, NVIC, DWT, CoreDebug, FPU) are
 *   register blocks handed out by volt_periph(), as the device's peripherals
 *   are (volt_periphs.h): the board sees each access.
 * - The instructions the firmware uses through CMSIS intrinsics are written
 *   in C after their definitions in the ARMv7-M Architecture Reference
 *   Manual (A7.7). PRIMASK is the board's: it holds back interrupts.
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#ifndef VOLT_CORE_CM7_H
#define VOLT_CORE_CM7_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- qualifiers and attributes */
#define __I     volatile const
#define __O     volatile
#define __IO    volatile
#define __IM    volatile const
#define __OM    volatile
#define __IOM   volatile

#define __ASM            __asm
#define __INLINE         inline
#define __STATIC_INLINE  static inline
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#define __NO_RETURN      __attribute__((__noreturn__))
#define __USED           __attribute__((used))
#define __WEAK           __attribute__((weak))
#define __PACKED         __attribute__((packed, aligned(1)))
#define __PACKED_STRUCT  struct __attribute__((packed, aligned(1)))
#define __ALIGNED(x)     __attribute__((aligned(x)))
#define __RESTRICT       __restrict
#define __COMPILER_BARRIER() __asm volatile("" ::: "memory")

void *volt_periph(int id, unsigned size);

/* Ids of the core's register blocks (the device's are in volt_periphs.h). */
enum VoltCorePeriph
{
    VP_CORE_FIRST = 1000,
    VP_SCB = VP_CORE_FIRST,
    VP_SYSTICK,
    VP_NVIC,
    VP_DWT,
    VP_COREDEBUG,
    VP_FPU,
    VP_CORE_END
};

/* ---- register blocks: the registers this firmware and the HAL headers name */
typedef struct
{
    __IOM uint32_t ISER[8];
    uint32_t RESERVED0[24];
    __IOM uint32_t ICER[8];
    uint32_t RESERVED1[24];
    __IOM uint32_t ISPR[8];
    uint32_t RESERVED2[24];
    __IOM uint32_t ICPR[8];
    uint32_t RESERVED3[24];
    __IOM uint32_t IABR[8];
    uint32_t RESERVED4[56];
    __IOM uint8_t IP[240];
    uint32_t RESERVED5[644];
    __OM uint32_t STIR;
} NVIC_Type;

typedef struct
{
    __IM uint32_t CPUID;
    __IOM uint32_t ICSR;
    __IOM uint32_t VTOR;
    __IOM uint32_t AIRCR;
    __IOM uint32_t SCR;
    __IOM uint32_t CCR;
    __IOM uint8_t SHPR[12];
    __IOM uint32_t SHCSR;
    __IOM uint32_t CFSR;
    __IOM uint32_t HFSR;
    __IOM uint32_t DFSR;
    __IOM uint32_t MMFAR;
    __IOM uint32_t BFAR;
    __IOM uint32_t AFSR;
    __IOM uint32_t CPACR;
} SCB_Type;

typedef struct
{
    __IOM uint32_t CTRL;
    __IOM uint32_t LOAD;
    __IOM uint32_t VAL;
    __IM uint32_t CALIB;
} SysTick_Type;

typedef struct
{
    __IOM uint32_t CTRL;
    __IOM uint32_t CYCCNT;
    __IOM uint32_t CPICNT;
    __IOM uint32_t EXCCNT;
    __IOM uint32_t SLEEPCNT;
    __IOM uint32_t LSUCNT;
    __IOM uint32_t FOLDCNT;
    __IM uint32_t PCSR;
    __OM uint32_t LAR;
    __IM uint32_t LSR;
} DWT_Type;

typedef struct
{
    __IOM uint32_t DHCSR;
    __OM uint32_t DCRSR;
    __IOM uint32_t DCRDR;
    __IOM uint32_t DEMCR;
} CoreDebug_Type;

typedef struct
{
    __IOM uint32_t FPCCR;
    __IOM uint32_t FPCAR;
    __IOM uint32_t FPDSCR;
    __IM uint32_t MVFR0;
    __IM uint32_t MVFR1;
    __IM uint32_t MVFR2;
} FPU_Type;

#define SCB        ((SCB_Type *)volt_periph(VP_SCB, sizeof(SCB_Type)))
#define SysTick    ((SysTick_Type *)volt_periph(VP_SYSTICK, sizeof(SysTick_Type)))
#define NVIC       ((NVIC_Type *)volt_periph(VP_NVIC, sizeof(NVIC_Type)))
#define DWT        ((DWT_Type *)volt_periph(VP_DWT, sizeof(DWT_Type)))
#define CoreDebug  ((CoreDebug_Type *)volt_periph(VP_COREDEBUG, sizeof(CoreDebug_Type)))
#define FPU        ((FPU_Type *)volt_periph(VP_FPU, sizeof(FPU_Type)))

/* ---- bit fields (ARMv7-M ARM, B3) */
#define SCB_SCR_SEVONPEND_Pos        4U
#define SCB_SCR_SEVONPEND_Msk        (1UL << SCB_SCR_SEVONPEND_Pos)
#define SCB_SCR_SLEEPDEEP_Pos        2U
#define SCB_SCR_SLEEPDEEP_Msk        (1UL << SCB_SCR_SLEEPDEEP_Pos)
#define SCB_SCR_SLEEPONEXIT_Pos      1U
#define SCB_SCR_SLEEPONEXIT_Msk      (1UL << SCB_SCR_SLEEPONEXIT_Pos)
#define SCB_AIRCR_PRIGROUP_Pos       8U
#define SCB_AIRCR_PRIGROUP_Msk       (7UL << SCB_AIRCR_PRIGROUP_Pos)
#define SysTick_CTRL_COUNTFLAG_Pos   16U
#define SysTick_CTRL_COUNTFLAG_Msk   (1UL << SysTick_CTRL_COUNTFLAG_Pos)
#define SysTick_CTRL_CLKSOURCE_Pos   2U
#define SysTick_CTRL_CLKSOURCE_Msk   (1UL << SysTick_CTRL_CLKSOURCE_Pos)
#define SysTick_CTRL_TICKINT_Pos     1U
#define SysTick_CTRL_TICKINT_Msk     (1UL << SysTick_CTRL_TICKINT_Pos)
#define SysTick_CTRL_ENABLE_Pos      0U
#define SysTick_CTRL_ENABLE_Msk      (1UL << SysTick_CTRL_ENABLE_Pos)
#define SysTick_LOAD_RELOAD_Msk      0xFFFFFFUL
#define DWT_CTRL_CYCCNTENA_Pos       0U
#define DWT_CTRL_CYCCNTENA_Msk       (1UL << DWT_CTRL_CYCCNTENA_Pos)
#define CoreDebug_DEMCR_TRCENA_Pos   24U
#define CoreDebug_DEMCR_TRCENA_Msk   (1UL << CoreDebug_DEMCR_TRCENA_Pos)
#define FPU_FPDSCR_AHP_Pos           26U
#define FPU_FPDSCR_AHP_Msk           (1UL << FPU_FPDSCR_AHP_Pos)
#define FPU_FPDSCR_DN_Pos            25U
#define FPU_FPDSCR_DN_Msk            (1UL << FPU_FPDSCR_DN_Pos)
#define FPU_FPDSCR_FZ_Pos            24U
#define FPU_FPDSCR_FZ_Msk            (1UL << FPU_FPDSCR_FZ_Pos)

/* ---- interrupt masking: the board delivers no interrupt while PRIMASK is set */
extern volatile uint32_t volt_primask;

static inline uint32_t __get_PRIMASK(void) { return volt_primask; }
static inline void __set_PRIMASK(uint32_t v) { volt_primask = v & 1u; }
static inline void __disable_irq(void) { volt_primask = 1u; }
static inline void __enable_irq(void) { volt_primask = 0u; }

static inline void __NOP(void) { }
static inline void __WFI(void) { }
static inline void __WFE(void) { }
static inline void __SEV(void) { }
static inline void __ISB(void) { __COMPILER_BARRIER(); }
static inline void __DSB(void) { __COMPILER_BARRIER(); }
static inline void __DMB(void) { __COMPILER_BARRIER(); }

/* ---- data processing (ARMv7-M ARM A7.7) */

/* SSAT: signed saturation to `sat` bits, 1..32. */
static inline int32_t __SSAT(int32_t val, uint32_t sat)
{
    if (sat >= 1u && sat <= 32u)
    {
        const int64_t hi = ((int64_t)1 << (sat - 1u)) - 1, lo = -((int64_t)1 << (sat - 1u));
        if (val > hi) return (int32_t)hi;
        if (val < lo) return (int32_t)lo;
    }
    return val;
}

/* USAT: unsigned saturation to `sat` bits, 0..31. */
static inline uint32_t __USAT(int32_t val, uint32_t sat)
{
    if (sat <= 31u)
    {
        const int64_t hi = ((int64_t)1 << sat) - 1;
        if (val > hi) return (uint32_t)hi;
        if (val < 0) return 0u;
    }
    return (uint32_t)val;
}

static inline int32_t volt_sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static inline int16_t volt_sat16(int32_t v)
{
    return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

/* QADD, QSUB: 32-bit saturating add and subtract. */
static inline int32_t __QADD(int32_t a, int32_t b) { return volt_sat32((int64_t)a + b); }
static inline int32_t __QSUB(int32_t a, int32_t b) { return volt_sat32((int64_t)a - b); }

/* QADD16: two signed halfword additions, each saturated. */
static inline uint32_t __QADD16(uint32_t a, uint32_t b)
{
    const int16_t lo = volt_sat16((int32_t)(int16_t)a + (int16_t)b);
    const int16_t hi = volt_sat16((int32_t)(int16_t)(a >> 16) + (int16_t)(b >> 16));
    return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
}

/* QSUB16: two signed halfword subtractions, each saturated. */
static inline uint32_t __QSUB16(uint32_t a, uint32_t b)
{
    const int16_t lo = volt_sat16((int32_t)(int16_t)a - (int16_t)b);
    const int16_t hi = volt_sat16((int32_t)(int16_t)(a >> 16) - (int16_t)(b >> 16));
    return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
}

/* SADD16: two signed halfword additions, modulo 2^16 (the GE flags are not kept). */
static inline uint32_t __SADD16(uint32_t a, uint32_t b)
{
    const uint16_t lo = (uint16_t)((uint16_t)a + (uint16_t)b);
    const uint16_t hi = (uint16_t)((uint16_t)(a >> 16) + (uint16_t)(b >> 16));
    return (uint32_t)lo | ((uint32_t)hi << 16);
}

/* PKHBT: bottom halfword of a, top halfword of b shifted left. */
#define __PKHBT(a, b, shift) \
    ((((uint32_t)(a)) & 0x0000FFFFUL) | ((((uint32_t)(b)) << (shift)) & 0xFFFF0000UL))
#define __PKHTB(a, b, shift) \
    ((((uint32_t)(a)) & 0xFFFF0000UL) | ((((uint32_t)(b)) >> (shift)) & 0x0000FFFFUL))

/* SMLALD: both signed halfword products added to a 64-bit accumulator. */
static inline uint64_t __SMLALD(uint32_t a, uint32_t b, uint64_t acc)
{
    const int64_t p = (int64_t)(int16_t)a * (int16_t)b + (int64_t)(int16_t)(a >> 16) * (int16_t)(b >> 16);
    return (uint64_t)((int64_t)acc + p);
}

/* CLZ: leading zeros; 32 for 0. */
static inline uint8_t __CLZ(uint32_t v) { return v ? (uint8_t)__builtin_clz(v) : 32u; }
static inline uint32_t __REV(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t __RBIT(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 32; i++, v >>= 1) r = (r << 1) | (v & 1u);
    return r;
}

/* ---- the core functions the firmware calls (the board does what they mean) */
void volt_core_system_reset(void);
uint32_t volt_core_systick_config(uint32_t ticks);

static inline void SCB_EnableICache(void) { }
static inline void SCB_EnableDCache(void) { }
static inline void SCB_DisableDCache(void) { }
static inline void SCB_CleanDCache(void) { }
static inline void SCB_InvalidateDCache(void) { }
static inline void SCB_CleanInvalidateDCache(void) { }
static inline void SCB_CleanDCache_by_Addr(uint32_t *a, int32_t n) { (void)a; (void)n; }
static inline void SCB_InvalidateDCache_by_Addr(uint32_t *a, int32_t n) { (void)a; (void)n; }
static inline void SCB_CleanInvalidateDCache_by_Addr(uint32_t *a, int32_t n) { (void)a; (void)n; }
static inline uint32_t SysTick_Config(uint32_t ticks) { return volt_core_systick_config(ticks); }
static inline void NVIC_SystemReset(void) { volt_core_system_reset(); }

#ifdef __cplusplus
}
#endif

#endif
