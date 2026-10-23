// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
// Existing buttons plus the separate Orbis touchpad click bit.
#define LSX4_APPLE_PAD_BUTTON_MASK 0x11ffffu
#ifdef __cplusplus
extern "C" {
#endif
// Match Android host routing: modal console dialogs consume their buttons;
// only remaining input reaches the game's emulated pad.
int executor_lsx4_apple_set_pad_button(uint32_t mask, int pressed);
#ifdef __cplusplus
}
#endif
