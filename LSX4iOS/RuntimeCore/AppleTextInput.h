// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// A nonzero token identifies a currently focused shared-runtime text field.
// Zero means no active field or that the linked runtime has no text interface.
uint64_t executor_lsx4_apple_text_session(void);
int executor_lsx4_apple_text_commit(uint64_t session,const char* utf8,size_t bytes);
int executor_lsx4_apple_text_backspace(uint64_t session,uint32_t count);
int executor_lsx4_apple_text_submit(uint64_t session);
int executor_lsx4_apple_text_cancel(uint64_t session);
#ifdef __cplusplus
}
#endif
