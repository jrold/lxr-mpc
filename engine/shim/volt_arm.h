// VOLTKIT: portable C versions of the Cortex-M instructions the LXR engine uses.
#pragma once
#include <stdint.h>

// Signed saturation to `bits` bits.
static inline int32_t __SSAT(int32_t v, uint32_t bits)
{
    const int32_t hi = (int32_t)((1u << (bits - 1)) - 1);
    const int32_t lo = -hi - 1;
    return v > hi ? hi : (v < lo ? lo : v);
}

// Saturating 32-bit add.
static inline int32_t __QADD(int32_t a, int32_t b)
{
    const int64_t s = (int64_t)a + (int64_t)b;
    return s > INT32_MAX ? INT32_MAX : (s < INT32_MIN ? INT32_MIN : (int32_t)s);
}

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

// Two saturating 16-bit adds packed in one 32-bit word.
static inline int32_t __QADD16(int32_t a, int32_t b)
{
    const int32_t lo = __SSAT((int16_t)(a & 0xffff) + (int16_t)(b & 0xffff), 16);
    const int32_t hi = __SSAT((int16_t)((uint32_t)a >> 16) + (int16_t)((uint32_t)b >> 16), 16);
    return (int32_t)(((uint32_t)hi << 16) | ((uint32_t)lo & 0xffff));
}

// The hardware random number generator, replaced by a software one (stubs.c).
typedef struct { int unused; } RNG_HandleTypeDef;
uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef* h);

// Pack the bottom half of `a` with the top half of `b << shift`.
static inline uint32_t __PKHBT(uint32_t a, uint32_t b, uint32_t shift)
{
    return (a & 0x0000ffffu) | ((b << shift) & 0xffff0000u);
}

// Two saturating 16-bit subtracts packed in one 32-bit word.
static inline int32_t __QSUB16(int32_t a, int32_t b)
{
    const int32_t lo = __SSAT((int16_t)(a & 0xffff) - (int16_t)(b & 0xffff), 16);
    const int32_t hi = __SSAT((int16_t)((uint32_t)a >> 16) - (int16_t)((uint32_t)b >> 16), 16);
    return (int32_t)(((uint32_t)hi << 16) | ((uint32_t)lo & 0xffff));
}

// Two wrapping 16-bit adds packed in one 32-bit word.
static inline int32_t __SADD16(int32_t a, int32_t b)
{
    const uint32_t lo = (uint32_t)((int16_t)(a & 0xffff) + (int16_t)(b & 0xffff)) & 0xffffu;
    const uint32_t hi = (uint32_t)((int16_t)((uint32_t)a >> 16) + (int16_t)((uint32_t)b >> 16)) & 0xffffu;
    return (int32_t)((hi << 16) | lo);
}

// Count leading zeros (32 for 0, as on the Cortex-M).
static inline uint8_t __CLZ(uint32_t v)
{
    return v ? (uint8_t)__builtin_clz(v) : 32;
}
#include "volt_hal_stubs.h"

// Float to unsigned as the Cortex-M7's VCVT.U32.F32 does it: negative and
// NaN give 0, too large gives the maximum (C leaves both undefined).
static inline uint32_t volt_f2u(float x)
{
    if (!(x > 0.0f))
        return 0;
    if (x >= 4294967296.0f)
        return 0xffffffffu;
    return (uint32_t)x;
}
