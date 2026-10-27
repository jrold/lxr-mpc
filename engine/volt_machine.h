/*
 * volt_machine.h: the whole LXR-02, as a library for VOLTKIT.
 *
 * The LXR V2 firmware (firmware/, Erica Synths, GPL-3.0) runs unmodified but
 * for the marked VOLTKIT changes, on VOLTKIT's virtual board (board/): ST's
 * HAL and the STM32F765's peripherals as the firmware uses them, and the
 * LXR-02's panel hardware (the shift-register keys and LEDs, the HD44780
 * display, the endless knobs and faders on the ADC, the encoder, the MIDI
 * UART, the two I2S DACs, the SD card).
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

#define VOLT_MACHINE_API_VERSION 4
#define VOLT_SAMPLE_RATE 44100      /* the DACs run at 44140.6 Hz (PLLI2S); the host's rate is close enough */
#define VOLT_CARD_SECTORS 131072u   /* a 64 MB card */
#define VOLT_SECTOR_BYTES 512u
#define VOLT_BUTTONS 41             /* the firmware's button numbers: 0..39 shift registers, 40 PAGE DOWN */
#define VOLT_KNOBS 4
#define VOLT_FADERS 6

/* What sits under one of the four knobs: the parameter the display shows
 * above it, as the firmware keeps it. */
typedef struct VoltKnob
{
    int param;   /* the firmware's parameter number; -1: nothing there */
    int value;
    int min, max;
    int name;    /* the display's name for it (a firmware text id) */
} VoltKnob;

typedef struct VoltMachineApi
{
    int version;

    /* The card. Before boot: load it sector by sector (unwritten sectors
     * read as zeros). Any time: read it back for a save. A blank card is
     * formatted at boot with the firmware's Init project; boot again to use
     * it (card_version tells). */
    void (*card_write)(uint32_t sector, const uint8_t *data);
    void (*card_read)(uint32_t sector, uint8_t *data);
    uint32_t (*card_next)(uint32_t from);   /* the next sector >= from holding data, or VOLT_CARD_SECTORS */
    uint32_t (*card_version)(void);         /* changes whenever the firmware writes the card */
    void (*flush)(void);                    /* the pattern and kit being played to the card (the firmware's autosave) */

    /* Starts the firmware and runs it to its main loop. 0 when it runs. */
    int (*boot)(void);

    /* Runs the machine for `frames` frames and writes both DACs' sum. */
    void (*render)(float *left, float *right, int frames);

    /* The panel. Each takes effect at the firmware's next main-loop pass. */
    void (*key)(int index, int down);       /* hold a key down, or let it go */
    void (*tap)(int index);                 /* down for one scan, up on the next */
    void (*encoder)(int detents);           /* + clockwise; 4 ms a detent, as the firmware samples it */
    void (*encoder_push)(void);             /* a press the firmware's debounce accepts */
    void (*fader)(int index, int value);    /* 0..4095 */
    int (*knob)(int index, VoltKnob *out);  /* what is under a knob now; 0 if nothing */
    void (*knob_to)(int index, int value);  /* turn a knob until its parameter reads value */
    int (*voice)(void);                     /* the voice whose page is shown (0..6); -1 on any other page */
    int (*voice_knob)(int voice, int index, VoltKnob *out);   /* what that knob would be on that voice's
                                                                  same page and screen; 0 if nothing, or
                                                                  not a sound parameter */
    void (*set_param)(int param, int value);   /* a sound parameter set as its knob would set it */
    void (*midi)(const uint8_t *bytes, int count);   /* the DIN MIDI input, at 31250 baud */
    void (*midi_at)(const uint8_t *bytes, int count, int frame);   /* ... arriving `frame` frames into the next render() */

    /* What it shows. */
    const uint8_t *(*lcd)(void);            /* 128 bytes of display RAM: line 1 at 0, line 2 at 0x40 */
    const uint8_t *(*lcd_glyphs)(void);     /* 64 bytes: the 8 custom 5x8 characters */
    uint32_t (*lcd_version)(void);          /* changes when either does */
    const uint8_t *(*leds)(void);           /* 6 bytes: the 5 shift-register bytes (bit = key), PAGE DOWN */
    uint32_t (*led_version)(void);
} VoltMachineApi;

const VoltMachineApi *volt_machine_api(void);

#ifdef __cplusplus
}
#endif

#endif
