# VOLTKIT

A drum computer for the MPC Live III (standalone, modded unit): the whole LXR
V2 firmware by Erica Synths, which is open source under the GPL, running on a
virtual LXR-02 board inside an MPC plug-in. Its sequencer with parameter
locks, its menus, kits, patterns and songs, its seven voices and its FX all
work as on the hardware. You play it from the MPC's touchscreen, which shows
the LXR-02's front panel, and from the MPC's pads, Q-Links and transport.

The board follows [lxr02-recomp](https://github.com/gregsadetsky/lxr02-recomp)
by Greg Sadetsky, which runs the same firmware in a browser. VOLTKIT is an
Open MPC VST2 plug-in with a native touchscreen skin, like the BOOM family.

> VOLTKIT is not made, supported or endorsed by Erica Synths or by Greg
> Sadetsky. "Erica Synths", "LXR" and "LXR-02" are Erica Synths' trademarks;
> they appear here only to say where the firmware comes from.

## The panel

**PANEL** is the machine's front panel:

- the 2x16 display, as the firmware draws it;
- the four endless knobs under the display's four columns: each turns the
  parameter above it. The MPC's four **Q-Links** turn them too;
- the **DATA** encoder and its **PUSH**;
- **SHIFT** (stays on until you tap it again) and **HOLD**: tap HOLD, then a
  key, and that key stays held down until you tap it again, for the LXR's
  key combinations;
- the voice row (page keys, the seven voices, REC, PLAY, COPY), the page and
  mode row (OSC, AEG, MOD, FM, CLICK, FILTER, LFO, MIX; VOICE, PERF, LOAD,
  SAVE) and the 16 step keys, with what SHIFT makes of each under them. Each
  key has its LED.

**MIX** has the six voice faders (the drums, snare, clap/cymbal and hats), a
master level, and **MPC SYNC**.

## Playing it

- **Pads** send MIDI notes to the LXR (its voices listen on channel 1).
- **Sync**: with MPC SYNC on (the default), the MPC's tempo, PLAY and STOP run
  the LXR's sequencer (MIDI clock, start and stop; the card's tempo is set to
  MID, MIDI clock). Off, set a tempo on the PERF page. **STOP twice** stops
  the LXR.
- **Saving**: each track keeps its own LXR SD card, compressed, inside the MPC
  project: its kits, patterns, songs and settings, with whatever you saved on
  the LXR (SAVE). New tracks start from lxr02-recomp's demo card (11 kits).

## How it's built

- `firmware/`: the LXR V2 firmware, from
  [erica-synths/lxr-desktop-app](https://github.com/erica-synths/lxr-desktop-app)
  (commit in `firmware/UPSTREAM_COMMIT`), under its own licence and notice
  (`firmware/LICENSE`, `firmware/NOTICE.md`).
- `platform/`: the virtual board the firmware runs on, mostly from
  lxr02-recomp (`platform/README.md`): the STM32 peripherals as C objects, the
  HAL, and the hardware model (virtual time, interrupts, the HD44780 display,
  the buttons and LEDs, the knobs' ADC, the MIDI UART, the DACs, the encoder),
  with the SD card in memory. The firmware runs as a coroutine that the
  plug-in's audio callback resumes.
- `engine/volt_machine.h`: the machine as a library: the card, boot, render,
  the panel's inputs, MIDI, and the display and LEDs. The firmware keeps its
  state in global variables, so every plug-in instance loads its own copy of
  the engine library.
- `mpc/`: the adapter (`VoltkitMpc.cpp`: taps, endless knobs, the transport's
  MIDI clock, the card in the project's state), the panel (`gen_layout.py`)
  and the skin (`skin/`: the display's 5x8 dot font and the hardware art).
- `tools/make_card.py`: makes the default card (`data/card.vkc`) from
  lxr02-recomp's `card.img`.

### Changes to the firmware

Each is marked `VOLTKIT (modified)` in the source and compiled only for
VOLTKIT:

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
build-host/voltkit-machine-test card.img   # the machine: boot, notes, a key
VOLTKIT_ENGINE=$PWD/build-host/libvoltkit-engine.dylib VOLTKIT_CARD=data/card.vkc \
    build-host/voltkit-adapter-test        # the plug-in, as the wrapper drives it
```

`mpc/build.sh` also cross-builds both for the MPC (`mpc/build/aarch64/`).

## Licence

GPL-3.0-only (`LICENSE`), as the firmware and lxr02-recomp it is built from,
with the firmware's additional terms (`firmware/NOTICE.md`). The MPC wrapper
it builds against is MIT-licensed. The installed package carries the licence,
the firmware's notice and where this source is (`engine/`).
