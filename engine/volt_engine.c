// VOLTKIT engine: drives the LXR V2 firmware's sound engine the way the
// firmware's own main loop and preset manager do (see volt_engine.h).
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

#include "volt_engine.h"

#include <string.h>

#include "Parameters.h"
#include "Preset/InitKit.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/DSPAudio/mixer.h"
#include "Soundengine/DSPAudio/modulationNode.h"
#include "Soundengine/DSPAudio/random.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/MidiVoiceControl.h"
#include "Soundengine/MIDI/ParameterArray.h"
#include "Soundengine/config.h"
#include "menu/menu.h"
#include "param_limits.h"

extern uint8_t activeParameters[NUM_PARAMS];
extern uint8_t parameterOrder[END_OF_SOUND_PARAMETERS];
uint8_t getNumModTargets(void);
uint8_t getModTargetGapIndex(uint8_t modTargetIdx);
uint8_t getModTargetIdxFromGapIdx(uint8_t v, uint8_t gapidx);

// Set by set_tempo, read by the firmware through seq_getBpm / seq_isRunning
// (firmware_stubs.c).
float volt_bpm = 120.0f;
int volt_running = 0;

_Static_assert(END_OF_SOUND_PARAMETERS == VOLT_PARAMS, "Parameters.h changed: update VOLT_PARAMS");

// The firmware renders blocks of OUTPUT_DMA_SIZE stereo frames, as 16-bit
// interleaved samples for each of its two output pairs.
static int16_t blockMain[OUTPUT_DMA_SIZE * 2];
static int16_t blockSecond[OUTPUT_DMA_SIZE * 2];
static int blockPos = OUTPUT_DMA_SIZE;   // frames of the current block already handed out

// A short fade for all_off, so cutting everything doesn't click.
enum { kPanicFadeFrames = 220 };   // 5 ms
static int panicFade = 0;

// What the menu and the preset manager do between a stored kit value and the
// engine: modulation targets are stored as indices into modTargets and sent
// as parameter numbers; everything else goes through the MIDI CC handler
// with the firmware's +1 offset below 128.
static void sendParam(const int param, const uint8_t value)
{
    const uint8_t numTargets = getNumModTargets();
    if (param >= PAR_VEL_DEST_1 && param <= PAR_VEL_DEST_6)
    {
        const uint8_t idx = value < numTargets ? value : 0;
        modNode_setDestination(&velocityModulators[param - PAR_VEL_DEST_1], modTargets[idx].param);
        return;
    }
    if (param >= PAR_TARGET_LFO1 && param <= PAR_TARGET_LFO6)
    {
        const uint8_t idx = value < numTargets ? value : 0;
        const uint16_t dest = modTargets[idx].param;
        switch (param - PAR_TARGET_LFO1)
        {
        case 0:
        case 1:
        case 2: modNode_setDestination(&voiceArray[param - PAR_TARGET_LFO1].lfo.modTarget, dest); break;
        case 3: modNode_setDestination(&snareVoice.lfo.modTarget, dest); break;
        case 4: modNode_setDestination(&cymbalVoice.lfo.modTarget, dest); break;
        default: modNode_setDestination(&hatVoice.lfo.modTarget, dest); break;
        }
        return;
    }
    // The LFO's target voice only picks where the menu looks for targets.
    if (param >= PAR_VOICE_LFO1 && param <= PAR_VOICE_LFO6)
        return;

    MidiMsg msg = { 0 };
    if (param < 128)
    {
        msg.status = MIDI_CC;
        msg.data1 = (uint8_t)((param + 1) & 0x7f);
    }
    else
    {
        msg.status = MIDI_CC2;
        msg.data1 = (uint8_t)(param - 128);
    }
    msg.data2 = value;
    midiParser_ccHandler(msg, 1, 0, 0.0f);
}

// A value as the LXR's menu would let it through (param_limits.h).
static uint8_t clampParam(const int param, const int value)
{
    const int lo = voltParamMin[param], hi = voltParamMax[param];
    if (param >= PAR_VOICE_LFO1 && param <= PAR_VOICE_LFO6 && (value < lo || value > hi))
        return 1;
    return (uint8_t)(value < lo ? lo : (value > hi ? hi : value));
}

// Slots in the parameter list that are MIDI plumbing, not sound: NRPN
// number and data entry (a stray value there makes the next CC write
// anywhere) and bank change.
static int isPlumbing(const int param)
{
    return param == PAR_NRPN_DATA_ENTRY_COARSE || param == PAR_NRPN_FINE || param == PAR_NRPN_COARSE
        || param == PAR_BANK_CHANGE;
}

static void setParam(const int param, const int value)
{
    if (param <= PAR_NONE || param >= END_OF_SOUND_PARAMETERS || isPlumbing(param))
        return;
    const uint8_t v = clampParam(param, value);
    activeParameters[param] = v;
    sendParam(param, v);
}

static int getParam(const int param)
{
    if (param <= PAR_NONE || param >= END_OF_SOUND_PARAMETERS)
        return 0;
    return activeParameters[param];
}

// Every parameter to the engine, in the firmware's order (bass drum first,
// to keep a kit change quiet).
static void sendAll(void)
{
    for (int i = 0; i < END_OF_SOUND_PARAMETERS; ++i)
    {
        const int p = parameterOrder[i];
        if (p > PAR_NONE && p < END_OF_SOUND_PARAMETERS && !isPlumbing(p))
            sendParam(p, activeParameters[p]);
    }
}

static void loadKit(const uint8_t* data, const int size)
{
    // As preset_readDrumsetData: the bytes after the name are the
    // parameters from index 0, anything missing is 0.
    uint8_t params[END_OF_SOUND_PARAMETERS];
    memset(params, 0, sizeof params);
    const int n = size - 8;
    if (data && n > 0)
        memcpy(params, data + 8, (size_t)(n < END_OF_SOUND_PARAMETERS ? n : END_OF_SOUND_PARAMETERS));

    for (int p = PAR_NONE + 1; p < END_OF_SOUND_PARAMETERS; ++p)
        params[p] = clampParam(p, params[p]);
    const uint8_t numTargets = getNumModTargets();
    for (int i = 0; i < 6; ++i)
    {
        if (params[PAR_VEL_DEST_1 + i] >= numTargets)
            params[PAR_VEL_DEST_1 + i] = 0;
        if (params[PAR_TARGET_LFO1 + i] >= numTargets)
            params[PAR_TARGET_LFO1 + i] = 0;
        if (params[PAR_VOICE_LFO1 + i] < 1 || params[PAR_VOICE_LFO1 + i] > 6)
            params[PAR_VOICE_LFO1 + i] = 1;
    }
    memcpy(activeParameters, params, sizeof params);
    sendAll();
}

// The parts of soundengine_init() that make sound.
static void startEngine(void)
{
    initRng();
    mixer_init();
    parameterArray_init();
    for (int i = 0; i < 6; ++i)
        modNode_init(&velocityModulators[i]);
    memset(midi_MidiChannels, 0, 8);
    initDrumVoice();
    Snare_init();
    HiHat_init();
    Cymbal_init();
    blockPos = OUTPUT_DMA_SIZE;
}

static void init(void)
{
    startEngine();
    loadKit(initKit, INIT_KIT_SIZE);
    panicFade = 0;
}

static void trigger(const int voice, const int note, const int velocity)
{
    if (voice < 0 || voice >= VOLT_VOICES || velocity <= 0)
        return;
    panicFade = 0;
    const int n = note < 0 ? 0 : (note > 127 ? 127 : note);
    const int v = velocity > 127 ? 127 : velocity;
    voiceControl_noteOn((uint8_t)voice, (uint8_t)n, (uint8_t)v);
}

static void allOff(void)
{
    if (panicFade == 0)
        panicFade = kPanicFadeFrames;
}

static void setTempo(const float bpm, const int running)
{
    if (bpm > 0.0f)
        volt_bpm = bpm;
    volt_running = running;
}

static void render(float* left, float* right, const int frames)
{
    const float scale = 1.0f / 32768.0f;
    for (int i = 0; i < frames; ++i)
    {
        if (blockPos >= OUTPUT_DMA_SIZE)
        {
            mixer_calcNextSampleBlock(blockMain, blockSecond);
            blockPos = 0;
        }
        // Both output pairs: everything is routed to the first while the
        // second reads as unplugged, but the sum costs nothing.
        float l = (blockMain[blockPos * 2] + blockSecond[blockPos * 2]) * scale;
        float r = (blockMain[blockPos * 2 + 1] + blockSecond[blockPos * 2 + 1]) * scale;
        ++blockPos;

        if (panicFade > 0)
        {
            const float g = (float)(panicFade - 1) / kPanicFadeFrames;
            l *= g;
            r *= g;
            if (--panicFade == 0)
            {
                // Faded out: restart the engine with the same kit, which
                // ends every voice and clears the effects' memory.
                startEngine();
                sendAll();
                for (int j = i + 1; j < frames; ++j)
                    left[j] = right[j] = 0.0f;
                left[i] = l;
                right[i] = r;
                return;
            }
        }
        left[i] = l;
        right[i] = r;
    }
}

static int retarget(const int target, const int voice)
{
    if (target <= 0 || target >= getNumModTargets() || voice < 1 || voice > 6)
        return 0;
    return getModTargetIdxFromGapIdx((uint8_t)(voice - 1), getModTargetGapIndex((uint8_t)target));
}

static const VoltEngineApi api = {
    VOLT_ENGINE_API_VERSION,
    init,
    setParam,
    getParam,
    loadKit,
    trigger,
    allOff,
    setTempo,
    render,
    retarget,
};

__attribute__((visibility("default"))) const VoltEngineApi* volt_engine_api(void)
{
    return &api;
}
