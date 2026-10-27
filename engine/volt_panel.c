/*
 * volt_panel.c: what the LXR firmware has under each of its four knobs, read
 * the way its own knob handler finds it (adcPots.c, menu_parseKnobValue()):
 * the parameter of the display's column, its value, and the range the
 * firmware's limitPotValue() keeps it in.
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#include "volt_machine.h"

#include "menu/menu.h"
#include "buttonHandler.h"
#include "ff.h"
#include "Preset/PresetManager.h"
#include "frontPanelParser_ui.h"
#include "Soundengine/Soundengine.h"

uint8_t limitPotValue(int8_t diff, uint16_t paramNr);   /* menu.c (VOLTKIT: not static) */
extern uint8_t preset_patDirty, preset_kitDirty;        /* presetManager.c */
FRESULT preset_saveTmpPat(void);
FRESULT preset_saveTmpKit(void);
extern uint8_t saveScreenPots[4];

int volt_panel_knob(int index, VoltKnob *out)
{
    if (index < 0 || index >= VOLT_KNOBS)
        return 0;
    const uint8_t menuIndex = getMenuIndx();
    const uint8_t activePage = (menuIndex & MASK_PAGE) >> PAGE_SHIFT;
    const uint8_t isOn2ndPage = ((menuIndex & MASK_PARAMETER) > 3) * 4;
    const uint16_t paramNr = *(&menuPages[menu_activePage][activePage].bot1 + index + isOn2ndPage);

    if (menu_activePage == SAVE_PAGE || menu_activePage == LOAD_PAGE)
    {
        /* the name and slot pickers keep their own knob positions */
        out->param = -1 - index;
        out->value = saveScreenPots[index];
        out->min = 0;
        out->max = 127;
        return 1;
    }
    if (paramNr == PAR_NONE || paramNr >= NUM_PARAMS)
        return 0;
    out->param = paramNr;
    out->name = *(&menuPages[menu_activePage][activePage].top1 + index + isOn2ndPage);
    out->value = activeParameters[paramNr];
    out->min = limitPotValue(-128, paramNr);
    out->max = (parameter_dtypes[paramNr] & 0xFF) == DTYPE_0B255 ? 255 : limitPotValue(127, paramNr);
    if (out->max < out->min)
        out->max = out->min;
    return 1;
}

/* What preset_processTimer() does once PRESET_SAVE_TIME has passed since a
 * change: the pattern and kit being played to the card's tmp folder, which
 * boot loads. Always, not only when marked changed: the firmware drops that
 * mark for the pattern whenever the sequencer runs (PRESET_STOP_PAT_TIMER),
 * and with the MPC's transport it mostly does. */
void volt_panel_flush(void)
{
    preset_patDirty = 0;
    preset_saveTmpPat();
    preset_kitDirty = 0;
    preset_saveTmpKit();
}

/* The voice whose page is shown, or -1. */
int volt_panel_voice(void)
{
    return menu_activePage <= VOICE7_PAGE ? menu_activePage : -1;
}

/* A plain sound parameter: one menu_parseKnobValue() sends as a CC (not a
   modulation target, which it translates first, nor a global). */
static int plain_sound_param(uint16_t paramNr)
{
    if (paramNr == PAR_NONE || paramNr >= END_OF_SOUND_PARAMETERS)
        return 0;
    switch (parameter_dtypes[paramNr] & 0xFF)
    {
    case DTYPE_TARGET_SELECTION_VELO:
    case DTYPE_VOICE_LFO:
    case DTYPE_TARGET_SELECTION_LFO:
        return 0;
    default:
        return 1;
    }
}

/* What knob `index` would be on `voice`'s page, on the sub page and screen
   shown now (a voice page's): that voice's parameter in the same place. */
int volt_panel_voice_knob(int voice, int index, VoltKnob *out)
{
    if (voice < 0 || voice > VOICE7_PAGE || index < 0 || index >= VOLT_KNOBS || volt_panel_voice() < 0)
        return 0;
    const uint8_t menuIndex = getMenuIndx();
    const uint8_t activePage = (menuIndex & MASK_PAGE) >> PAGE_SHIFT;
    const uint8_t isOn2ndPage = ((menuIndex & MASK_PARAMETER) > 3) * 4;
    const uint16_t paramNr = *(&menuPages[voice][activePage].bot1 + index + isOn2ndPage);
    if (!plain_sound_param(paramNr))
        return 0;
    out->param = paramNr;
    out->name = *(&menuPages[voice][activePage].top1 + index + isOn2ndPage);
    out->value = activeParameters[paramNr];
    out->min = limitPotValue(-128, paramNr);
    out->max = (parameter_dtypes[paramNr] & 0xFF) == DTYPE_0B255 ? 255 : limitPotValue(127, paramNr);
    if (out->max < out->min)
        out->max = out->min;
    return 1;
}

/* A sound parameter set the way menu_parseKnobValue() sets one (its plain
   case): kept, then sent on through the kit's morph as a CC. Runs on the
   firmware's main loop (board.c). */
void volt_panel_set_param(int param, int value)
{
    if (param < 0 || !plain_sound_param((uint16_t)param))
        return;
    const uint16_t paramNr = (uint16_t)param;
    const uint8_t lo = limitPotValue(-128, paramNr);
    const uint8_t hi = (parameter_dtypes[paramNr] & 0xFF) == DTYPE_0B255 ? 255 : limitPotValue(127, paramNr);
    activeParameters[paramNr] = (uint8_t)(value < lo ? lo : value > hi ? hi : value);
    preset_touchTimer(PRESET_KIT_TIMER);
    uint8_t sent = activeParameters[paramNr];
    if (paramNr < PAR_AUDIO_OUT1 || paramNr > PAR_AUDIO_OUT6)
        sent = preset_getMorphValue(paramNr, activeParameters[PAR_MORPH]);
    if (paramNr < 128)
        frontPanel_sendData(MIDI_CC, (uint8_t)paramNr, sent);
    else
        frontPanel_sendData(CC_2, (uint8_t)(paramNr - 128), sent);
}

void volt_panel_repaint(void)
{
    menu_repaintAll();
}
