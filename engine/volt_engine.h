// VOLTKIT engine: the LXR V2 firmware's sound engine (voices, mixer and FX
// bus, compiled from firmware/), behind a small C interface.
//
// The firmware keeps all of its state in global variables, so one loaded
// copy of the engine is one drum machine. The MPC plug-in loads a separate
// copy of this library for every instance (see mpc/EngineLoader.*); inside
// one copy, every call comes from one thread at a time.
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only, like the firmware it wraps.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOLT_ENGINE_API_VERSION 1

enum
{
    VOLT_SAMPLE_RATE = 44100,
    VOLT_VOICES = 7,        // drum 1-3, snare, cymbal, closed hat, open hat
    VOLT_PARAMS = 247,      // the firmware's sound parameters (Parameters.h)
    VOLT_KIT_BYTES = 8 + 247
};

typedef struct VoltEngineApi
{
    int version;

    // Starts the engine with the firmware's init kit.
    void (*init)(void);

    // One sound parameter, numbered as in the firmware's Parameters.h, with
    // the value the LXR's own menu would store (0..127 for most, wider for a
    // few such as modulation targets). Applied exactly as the firmware
    // applies a kit value.
    void (*set_param)(int param, int value);
    int (*get_param)(int param);

    // Loads a kit in the LXR's .SND layout: an 8-byte name, then one byte
    // per sound parameter (shorter files leave the rest at 0).
    void (*load_kit)(const uint8_t* data, int size);

    // Plays a voice (0..6, see VOLT_VOICES) at velocity 1..127. `note` is
    // the MIDI note; the firmware's default note 60 plays the kit's pitch.
    void (*trigger)(int voice, int note, int velocity);

    // Silences every voice and the effects' tails.
    void (*all_off)(void);

    // The host tempo and transport, for tempo-synced LFOs and delay.
    void (*set_tempo)(float bpm, int running);

    // Writes `frames` of stereo output.
    void (*render)(float* left, float* right, int frames);

    // The modulation target (an index into the firmware's modTargets) that
    // matches `target` on another voice (1..6), as the LXR's menu picks it
    // when an LFO changes voice; 0 when that voice has no such target.
    int (*retarget)(int target, int voice);
} VoltEngineApi;

// The one exported symbol of the engine library.
const VoltEngineApi* volt_engine_api(void);
typedef const VoltEngineApi* (*VoltEngineApiFn)(void);

#ifdef __cplusplus
}
#endif
