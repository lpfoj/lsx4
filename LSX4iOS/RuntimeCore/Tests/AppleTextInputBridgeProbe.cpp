// SPDX-License-Identifier: GPL-2.0-or-later
#include "../AppleTextInput.h"
#include <cassert>
#include <cstdio>
#include <string>
#if defined(TEST_LINK_SHARED_BACKEND)
static uint64_t receivedToken;
static unsigned receivedAction,receivedCount;
static std::string receivedText;
extern "C" uint64_t executor_lsx4_runtime_system_ui_text_session(){return 42;}
extern "C" int executor_lsx4_runtime_system_ui_text_session_event(uint64_t token,unsigned action,
        const char* text,size_t bytes,unsigned count) {
    receivedToken=token;receivedAction=action;receivedCount=count;
    receivedText=text?std::string(text,bytes):std::string{};
    return token==42;
}
#endif
int main() {
#if defined(TEST_LINK_SHARED_BACKEND)
    assert(executor_lsx4_apple_text_session()==42);
    assert(executor_lsx4_apple_text_commit(42,"A\xd0\x96",3)==1);
    assert(receivedToken==42&&receivedAction==0&&receivedCount==1&&receivedText=="A\xd0\x96");
    assert(executor_lsx4_apple_text_backspace(42,2)==1&&receivedAction==1&&receivedCount==2&&receivedText.empty());
    assert(executor_lsx4_apple_text_submit(42)==1&&receivedAction==2&&receivedCount==1);
    assert(executor_lsx4_apple_text_cancel(42)==1&&receivedAction==3&&receivedCount==1);
    assert(executor_lsx4_apple_text_submit(41)==0); // Backend refusal is preserved.
    assert(executor_lsx4_apple_text_submit(0)==0);
    std::puts("APPLE_TEXT_BRIDGE routing=1 exactUtf8Bytes=1 backspace=1 submit=1 cancel=1 refusal=1 passed=1");
#else
    assert(executor_lsx4_apple_text_session()==0);
    assert(executor_lsx4_apple_text_commit(42,"name",4)==0);
    assert(executor_lsx4_apple_text_backspace(42,1)==0);
    assert(executor_lsx4_apple_text_submit(42)==0);
    assert(executor_lsx4_apple_text_cancel(42)==0);
    std::puts("APPLE_TEXT_BRIDGE absentBackend=1 inert=1 passed=1");
#endif
}
