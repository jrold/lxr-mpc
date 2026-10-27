/* volt_no_sections.h: forced into every file of the Mac build (mpc/CMakeLists.txt).
 * Mach-O wants "segment,section" names; the firmware's ".itcm_text" etc. are
 * for the STM32's tightly coupled memory, which a host doesn't have.
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only. */
#define section(x) __used__
