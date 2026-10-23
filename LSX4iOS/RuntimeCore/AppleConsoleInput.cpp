// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleConsoleInput.h"
#include <cstdio>
#include <cstdlib>
extern "C" uint32_t executor_lsx4_runtime_system_ui_gamepad_button(uint32_t, int);
extern "C" int executor_lsx4_runtime_set_pad_button(uint32_t, int);

int executor_lsx4_apple_set_pad_button(uint32_t mask, int pressed) {
    const auto consumed = executor_lsx4_runtime_system_ui_gamepad_button(mask, pressed);
    const auto guest = mask & ~consumed;
    if(std::getenv("LSX4_PROBE_INPUT_ROUTE"))
        std::fprintf(stderr,"[APPLE_INPUT_ROUTE] mask=0x%x pressed=%d consumed=0x%x guest=0x%x\n",
            mask,pressed,consumed,guest);
    return guest ? executor_lsx4_runtime_set_pad_button(guest, pressed) : 0;
}
