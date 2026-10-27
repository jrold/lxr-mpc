/*
 * volt_machine.h: the whole LXR-02, as a library for VOLTKIT.
 *
 * The LXR V2 firmware (firmware/, Erica Synths, GPL-3.0) runs unmodified but
 * for the marked VOLTKIT changes, on a virtual board (platform/vboard.c,
 * after lxr02-recomp's): its sequencer, menus, kits and patterns, and its
 * sound engine. The host feeds the board's inputs (the 40 panel buttons and
 * PAGE DOWN, the 4 knobs and 6 faders, the encoder, the DIN MIDI input) and
 * reads its outputs (the 2x16 LCD, the button LEDs, both stereo DACs). The SD
 * card lives in memory, sector by sector.
 *
 * The firmware keeps its state in globals: a process holds one machine per
 * loaded copy of this library (mpc/EngineLoader.h). Not thread-safe: one call
 * at a time; render() runs the firmware itself.
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#ifndef VOLT_MACHINE_H
#define VOLT_MACHINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOLT_MACHINE_API_VERSION 1
#define VOLT_SAMPLE_RATE 44100      /* the LXR's DACs run at 44140.6 Hz; the host's rate is close enough */
#define VOLT_CARD_SECTORS 131072u   /* a 64 MB card */
#define VOLT_SECTOR_BYTES 512u
#define VOLT_BUTTONS 41             /* shift-register buttons 0..39, 40 = PAGE DOWN */
#define VOLT_ADC_KNOBS 8            /* adc 0..7: four endless knobs, two wipers each */
#define VOLT_ADC_FADERS 6           /* adc 8..13: the faders */

typedef struct VoltMachineApi
{
    int version;

    /* The card. Before boot: load it sector by sector (unwritten sectors
     * read as zeros). Any time: read it back for a save. */
    void (*card_write)(uint32_t sector, const uint8_t *data);
    void (*card_read)(uint32_t sector, uint8_t *data);
    uint32_t (*card_next)(uint32_t from);   /* the next sector >= from holding data, or VOLT_CARD_SECTORS */
    uint32_t (*card_version)(void);         /* changes whenever the firmware writes the card */

    /* Starts the firmware and runs it until it plays (about 2 s of virtual
     * time: the boot screen and the project load). 0 when it is running. */
    int (*boot)(void);

    /* Runs the firmware for `frames` frames and writes both DACs' sum. */
    void (*render)(float *left, float *right, int frames);

    /* Inputs, applied at the start of the firmware's next 64-frame block. */
    void (*button)(int index, int down);    /* 0..40 */
    void (*adc)(int index, int value);      /* 0..13, the raw 16-bit reading */
    void (*encoder)(int detents);           /* + clockwise */
    void (*encoder_button)(int down);
    void (*midi)(const uint8_t *bytes, int count);   /* the DIN MIDI input */

    /* Outputs. */
    const uint8_t *(*lcd)(void);            /* 128 bytes of display RAM: line 1 at 0, line 2 at 0x40 */
    const uint8_t *(*lcd_glyphs)(void);     /* 64 bytes: the 8 custom 5x8 characters */
    uint32_t (*lcd_version)(void);          /* changes when either does */
    const uint8_t *(*leds)(void);           /* 6 bytes: the 5 shift-register bytes (bit = button), PAGE DOWN */
    uint32_t (*led_version)(void);
} VoltMachineApi;

const VoltMachineApi *volt_machine_api(void);

#ifdef __cplusplus
}
#endif

#endif
