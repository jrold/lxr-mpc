# VOLTKIT

A drum voice module for the MPC Live III (standalone, modded unit): the LXR
V2 firmware by Erica Synths, which is open source under the GPL, running on
VOLTKIT's own virtual LXR-02 board inside an MPC plug-in. Its seven voices,
their pages of parameters, its kits and its FX work as on the hardware; the
MPC's sequencer plays them through the pads (the LXR's own sequencer has no
keys here). You edit them from the MPC's touchscreen and Q-Links. VOLTKIT is
an Open MPC VST2 plug-in with a native touchscreen skin, like the BOOM family.

> VOLTKIT is not made, supported or endorsed by Erica Synths. "Erica Synths",
> "LXR" and "LXR-02" are Erica Synths' trademarks; they appear here only to
> say where the firmware comes from.

## The panel

**PANEL**:

- the 2x16 display, as the firmware draws it;
- the four knobs under the display's four columns: each shows and sets the
  value of the parameter above it. The MPC's four **Q-Links** turn these four
  knobs, on every page (no other Q-Link bank does anything);
- **ALL** (see below);
- the page keys OSC, AEG, MOD, FM, CLICK, FILTER, LFO and MIX, and under
  them the seven voice keys. A page key again shows that page's second screen
  (a `>` at the display's top right says there is one), and again its first.
  Each key has its LED.

**MIX** has the six voice faders (the drums, snare, clap/cymbal and hats) and
a master level. **SETUP** has the pads.

## Playing it

- **Pads** play the seven voices: General MIDI drum notes, or the pads
  **PAD LEARN** learned (remembered for new tracks). Record them on the MPC
  like any instrument. A pad played shows its voice on the panel, on the
  same page, while the MPC is stopped (playing, its sequence plays the same
  notes the pads do, and the panel stays where you put it).
- **ALL**: tap it on, and a knob turned moves its parameter on every voice
  that has the same parameter in that place: by as much from where each was,
  or, for a choice of a few settings (a waveform), to the same one. Tap it
  off and every parameter it moved snaps back. (The screen can't tell a
  plug-in when a finger lifts, so ALL is a latch rather than a key you hold.)
  Globals and modulation targets (MIX's second screen: track length, MIDI
  channel and note; LFO's targets) turn as usual and aren't part of it.
- **Saving**: each track keeps its own LXR SD card, compressed, inside the MPC
  project: its kits and settings, with what the LXR autosaves. New tracks
  start from the firmware's own Init kit, the card it writes when it formats
  one.

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
- `mpc/`: the adapter (`VoltkitMpc.cpp`: taps, endless knobs, ALL, the pads,
  the card in the project's state), the panel (`gen_layout.py`) and the skin
  (`skin/`: the display's 5x8 dot font and the hardware art).
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
                                           # pages, knobs, ALL, the state
VOLTKIT_ENGINE=$PWD/build-host/libvoltkit-engine.dylib VOLTKIT_CARD=data/card.vkc \
    build-host/voltkit-stress-test 30      # random playing: voices, pages, knobs, ALL
```

`mpc/build.sh` also cross-builds both for the MPC (`mpc/build/aarch64/`).

## Licence

GPL-3.0-only (`LICENSE`), as the firmware it is built from,
with the firmware's additional terms (`firmware/NOTICE.md`). The MPC wrapper
it builds against is MIT-licensed. The installed package carries the licence,
the firmware's notice and where this source is (`engine/`).
