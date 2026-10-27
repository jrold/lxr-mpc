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

uint8_t limitPotValue(int8_t diff, uint16_t paramNr);   /* menu.c (VOLTKIT: not static) */
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
