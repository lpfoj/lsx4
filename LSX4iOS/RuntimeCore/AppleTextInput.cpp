// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleTextInput.h"
// The older SwiftUI shell does not link the complete shared runtime. Its
// adapter remains inactive until these optional host UI exports exist.
extern "C" uint64_t executor_lsx4_runtime_system_ui_text_session(void) __attribute__((weak_import));
extern "C" int executor_lsx4_runtime_system_ui_text_session_event(uint64_t,unsigned,const char*,size_t,unsigned) __attribute__((weak_import));
uint64_t executor_lsx4_apple_text_session(void) {
    return executor_lsx4_runtime_system_ui_text_session?executor_lsx4_runtime_system_ui_text_session():0;
}
static int Send(uint64_t session,unsigned action,const char* text,size_t bytes,unsigned count) {
    if(!session || !executor_lsx4_runtime_system_ui_text_session_event)return 0;
    return executor_lsx4_runtime_system_ui_text_session_event(session,action,text,bytes,count);
}
int executor_lsx4_apple_text_commit(uint64_t session,const char* utf8,size_t bytes) {return Send(session,0,utf8,bytes,1);}
int executor_lsx4_apple_text_backspace(uint64_t session,uint32_t count) {return Send(session,1,nullptr,0,count);}
int executor_lsx4_apple_text_submit(uint64_t session) {return Send(session,2,nullptr,0,1);}
int executor_lsx4_apple_text_cancel(uint64_t session) {return Send(session,3,nullptr,0,1);}
