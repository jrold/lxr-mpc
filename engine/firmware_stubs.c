// VOLTKIT: what the LXR sound engine expects from the rest of the firmware
// (sequencer, menu, front panel, MIDI and USB ports, hardware), reduced to
// what a plug-in needs. The MPC sequences, so the sequencer calls do nothing.
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

#include <string.h>

#include "Parameters.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/config.h"
#include "stm32f7xx_hal.h"

extern float volt_bpm;
extern int volt_running;

// --- hardware ---------------------------------------------------------------

RNG_HandleTypeDef hrng;
static uint32_t rngState = 0x9E3779B9u;

uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef* h)
{
    (void)h;
    // xorshift32 in place of the STM32's hardware generator.
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

VoltDwt volt_dwt;
volatile uint32_t systick_ticks;

// Output jack switches: main pair plugged in (GPIOD 6 and 7), second pair
// empty (GPIOB 4 and 6), so the mixer folds every route onto the main pair.
VoltGpio volt_gpio_b = { 0 };
VoltGpio volt_gpio_d = { GPIO_PIN_6 | GPIO_PIN_7 };

float adc_getFaderVolume(uint8_t idx)
{
    (void)idx;
    return 1.0f;   // as the firmware without fader volume
}

// --- parameters the menu owns ------------------------------------------------

uint8_t activeParameters[NUM_PARAMS];

uint8_t menu_activePage = 0;
uint8_t menu_getActiveVoice(void) { return 0; }
uint8_t menu_getSubPage(void) { return 0; }
uint8_t menu_getViewedPattern(void) { return 0; }
void menu_repaint(void) {}
void menu_switchSubPage(uint8_t subPageNr, uint8_t toggle) { (void)subPageNr; (void)toggle; }

// --- sequencer --------------------------------------------------------------

PatternSet seq_patternSet;

uint16_t seq_getBpm(void) { return (uint16_t)(volt_bpm + 0.5f); }
uint8_t seq_isRunning(void) { return volt_running ? 1 : 0; }
uint8_t seq_getExtSync(void) { return 0; }
uint8_t seq_isTrackMuted(uint8_t trackNr) { (void)trackNr; return 0; }
void seq_addNote(uint8_t trackNr, uint8_t vel, uint8_t note) { (void)trackNr; (void)vel; (void)note; }
void seq_midiNoteOff(uint8_t chan) { (void)chan; }
void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc) { (void)channel; (void)note; (void)veloc; }
void seq_recordAutomation(uint8_t voice, uint8_t dest, uint8_t value) { (void)voice; (void)dest; (void)value; }
void seq_setMute(uint8_t trackNr, uint8_t isMuted) { (void)trackNr; (void)isMuted; }
void seq_setNextPattern(const uint8_t patNr) { (void)patNr; }
void seq_setRunning(uint8_t isRunning) { (void)isRunning; }
void seq_sync(void) {}
void sync_midiResetPosition(void) {}
void sync_midiStartStop(uint8_t isStart) { (void)isStart; }

// --- ports ------------------------------------------------------------------

void uart_sendFrontpanelByte(uint8_t data) { (void)data; }
void uart_sendMidi(MidiMsg msg) { (void)msg; }
void usb_sendMidi(MidiMsg msg) { (void)msg; }

// The track the front panel has selected (CortexParser.c).
uint8_t frontParser_activeTrack = 0;
