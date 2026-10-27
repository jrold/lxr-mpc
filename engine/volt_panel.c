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

/* Whether holding a step key now would arm a lock: voice mode (in PERF a held
   step rolls a voice, in step mode it selects a step), no SHIFT, LOAD or SAVE. */
int volt_panel_lockable(void)
{
    return buttonHandler_getMode() == SELECT_MODE_VOICE && !buttonHandler_getShift() && !buttonHandler_getLoad() &&
           !buttonHandler_getSave();
}

/* Whether a held step's lock is armed: its LED blinks, and knobs now lock to it. */
int volt_panel_lock_armed(void)
{
    return buttonHandler_getArmedAutomationStep() != NO_STEP_SELECTED;
}
