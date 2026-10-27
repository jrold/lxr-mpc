# VOLTKIT

A drum computer for the MPC Live III (standalone, modded unit): the whole LXR
V2 firmware by Erica Synths, which is open source under the GPL, running on
VOLTKIT's own virtual LXR-02 board inside an MPC plug-in. Its sequencer with
parameter locks, its menus, kits, patterns and songs, its seven voices and its
FX all work as on the hardware. You play it from the MPC's touchscreen, which
shows the LXR-02's front panel, and from the MPC's pads, Q-Links and transport.
VOLTKIT is an Open MPC VST2 plug-in with a native touchscreen skin, like the
BOOM family.

> VOLTKIT is not made, supported or endorsed by Erica Synths. "Erica Synths",
> "LXR" and "LXR-02" are Erica Synths' trademarks; they appear here only to
> say where the firmware comes from.

## The panel

**PANEL** is the machine's front panel:

- the 2x16 display, as the firmware draws it;
- the four knobs under the display's four columns: each shows and sets the
  value of the parameter above it. The MPC's four **Q-Links** turn them too;
- the **DATA** encoder and its **PUSH**;
- **SHIFT** (stays on until you tap it again);
- the voice row (page keys, the seven voices, REC, PLAY, COPY), the page and
  mode row (OSC, AEG, MOD, FM, CLICK, FILTER, LFO, MIX; VOICE, PERF, LOAD,
  SAVE) and the 16 step keys, with what SHIFT makes of each under them. Each
  key has its LED.

**MIX** has the six voice faders (the drums, snare, clap/cymbal and hats), a
master level, and **MPC SYNC**.

**SETUP** sets what the pads do: **VOICES** (they play the seven voices:
General MIDI drum notes, or the pads **PAD LEARN** learned, remembered for new
tracks) or **STEPS** (they are the 16 step keys, held as long as a pad is).

## Playing it

- **Pads** play the voices, or in STEPS are the step keys.
- **Locks**: as on the LXR, a step held for half a second (its LED blinks)
  takes what the knobs and Q-Links do (two parameters a step, as on the LXR).
  On the screen, double-tap the step's key: it lights up and stays held until
  you tap it again; change pages meanwhile and lock there too (VOLTKIT lets
  go of the step around the page key and holds it again, since on the LXR a
  page key pressed with a step held edits its sub-steps). Or hold the step's
  pad (STEPS). A voice keeps the last lock it played until its next step. The screen can't hold a key itself:
  MPC tells a plug-in's skin when a finger touches, double-taps or presses
  ENTER, never when it lifts.
- **Recording**: REC on the panel, PLAY on the MPC, and play the pads.
- **Sync**: with MPC SYNC on (the default), the MPC's tempo, PLAY and STOP run
  the LXR's sequencer (MIDI clock, start and stop; the card's tempo is set to
  MID, MIDI clock). Off, set a tempo on the PERF page. **STOP twice** stops
  the LXR.
- **Saving**: each track keeps its own LXR SD card, compressed, inside the MPC
  project: its kits, patterns, songs and settings, with whatever you saved on
  the LXR (SAVE) and what it autosaves. New tracks start from the firmware's
  own Init project (its init kit), the card it writes when it formats one.

## How it's built

- `firmware/`: the LXR V2 firmware, from
  [erica-synths/lxr-desktop-app](https://github.com/erica-synths/lxr-desktop-app)
  (commit in `firmware/UPSTREAM_COMMIT`), under its own licence and notice
  (`firmware/LICENSE`, `firmware/NOTICE.md`).
- `board/`: VOLTKIT's virtual LXR-02 board, written from ST's RM0410 reference
  manual, the HD44780 datasheet and the firmware's own drivers: ST's HAL as
  the firmware calls it, every peripheral a register block the board sees
  accessed, and the panel hardware (shift-register keys and LEDs, the
  display, the endless pots and faders, the encoder, the MIDI UART, the two
  I2S DACs, the SD card in memory), with its own `core_cm7.h`. The firmware
  runs in its own context; each audio block it runs to the time the block
  ends, and interrupts arrive at their exact times.
- `engine/volt_machine.h`: the machine as a library: the card, boot, render,
  the panel's inputs, MIDI, and the display and LEDs. The firmware keeps its
  state in global variables, so every plug-in instance loads its own copy of
  the engine library.
- `mpc/`: the adapter (`VoltkitMpc.cpp`: taps, endless knobs, the transport's
  MIDI clock, the card in the project's state), the panel (`gen_layout.py`)
  and the skin (`skin/`: the display's 5x8 dot font and the hardware art).
- `tools/make_card.py`: makes the default card (`data/card.vkc`) from the
  card the firmware formats (`voltkit-machine-test --format`), with the tempo
  set to MID and each voice on its own MIDI note (36..42).

### Changes to the firmware

Each is marked `VOLTKIT (modified)` in the source and compiled only for
VOLTKIT:

- the main loop calls the board once per pass; a blank card is formatted
  without the encoder prompts, its Init project saved after `seq_init()`
  (its patterns were saved never set up), and its reboot asked for;
  `limitPotValue()` is visible, to read a knob's range;
- no tightly coupled memory sections;
- the codec's buffer addresses kept as pointer-sized integers (64-bit hosts);
- three null-pointer reads the STM32 tolerates (the morph kit at start-up, the
  USB serial class data with no USB host, a modulation target with no
  storage);
- the oscillator's phase-overflow test written in C instead of reading the
  Cortex-M status register; float-to-unsigned conversions saturate, as the
  Cortex-M7 does them; the interpolating oscillators wrap within their tables;
- CC 0 no longer writes 64 KB past `midiParser_originalCcValues`;
- the FX distortion's diode table index stays inside the table;
- the ping-pong delay keeps its positions in its half of the buffer.

## Build and install

```bash
bash mpc/build.sh      # needs zig, cmake, ninja, python3; no Docker
bash mpc/deploy.sh root@mpc-live-iii.local 2222
```

Installing restarts the MPC app, so save your project first.

## Tests

```bash
cmake -S mpc -B build-host -G Ninja -DMPC_VST_DIR=<mpc-vst-plugins checkout>
cmake --build build-host
build-host/voltkit-machine-test --format init.img   # the firmware prepares a blank card
python3 tools/make_card.py init.img data/card.vkc card.img
build-host/voltkit-machine-test card.img   # the machine: boot, voices, keys, knobs, encoder
VOLTKIT_ENGINE=$PWD/build-host/libvoltkit-engine.dylib VOLTKIT_CARD=data/card.vkc \
    build-host/voltkit-adapter-test        # the plug-in as the wrapper drives it: pads,
                                           # locks, recording, the sequencer on the MPC's clock
```

`mpc/build.sh` also cross-builds both for the MPC (`mpc/build/aarch64/`).

## Licence

GPL-3.0-only (`LICENSE`), as the firmware it is built from,
with the firmware's additional terms (`firmware/NOTICE.md`). The MPC wrapper
it builds against is MIT-licensed. The installed package carries the licence,
the firmware's notice and where this source is (`engine/`).
