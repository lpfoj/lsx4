// SPDX-License-Identifier: GPL-2.0-or-later
// Xcode synchronized-source shell: same adapter; inert without shared exports.
#include "../../RuntimeCore/AppleTextInput.cpp"
#include "../../RuntimeCore/AppleSystemTextInput.mm"
// This shell has no shared ImGui runtime yet. Mach-O needs a weak provider,
// not merely an undefined weak import; future strong runtime exports win.
extern "C" __attribute__((weak)) uint64_t executor_lsx4_runtime_system_ui_text_session(void) {
    return 0;
}
extern "C" __attribute__((weak)) int executor_lsx4_runtime_system_ui_text_session_event(
    uint64_t,unsigned,const char*,size_t,unsigned) {
    return 0;
}
