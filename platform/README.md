# platform: the virtual LXR-02 board

The firmware in `firmware/` runs on these files instead of the STM32 hardware.
Most come from [lxr02-recomp](https://github.com/gregsadetsky/lxr02-recomp)
by Greg Sadetsky (GPL-3.0-only, the commit in `RECOMP_COMMIT`), which runs the
same firmware in a browser:

- `shim/stm32f7xx.h`, `shim/lxr_board.h`: found before ST's headers; every
  peripheral instance (`GPIOE`, `USART3`, `TIM2`, ...) points at a plain C
  object (`tools/gen-remap.py` generates them from ST's device header).
- `portable/lxr_intrinsics.h` and `tools/gen-core-portable.py` (recomp's
  `gen-core-wasm.py`): the CMSIS core intrinsics in portable C and the core
  peripherals (SCB, DWT, ...) remapped to memory, for a non-ARM-M build.
- `hal.c`, `board.h`: the bodies of the ST HAL functions the firmware calls.
- `vboard.c`: recomp's `board.c` (the model of the hardware: virtual time,
  interrupts, GPIO, the HD44780 LCD, buttons and LEDs, the ADC, the UART, the
  DACs, the encoder) with its file and scenario I/O replaced by the host
  interface in `engine/volt_machine.h`, the SD card in memory, and the
  firmware run as a coroutine the host's render resumes.
- `hal_unused.c` (VOLTKIT's): HAL functions the firmware references but never
  calls on this board.
