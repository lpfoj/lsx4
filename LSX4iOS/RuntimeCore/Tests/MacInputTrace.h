// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded, explicit host input events, delivered through the shared pad API.
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include "../AppleConsoleInput.h"
static void StartMacInputPipe() {
    const char* setting=std::getenv("LSX4_PROBE_INPUT_PIPE");
    if(!setting) return;
    const std::string path=setting;
    std::thread([path] {
        const auto started=std::chrono::steady_clock::now();
        // Blocking FIFO reads consume no CPU between commands. This control
        // channel is only enabled by the host test harness, never the app.
        FILE* input=std::fopen(path.c_str(),"r");
        if(!input) return;
        std::setvbuf(input,nullptr,_IONBF,0);
        char line[128];
        while(std::fgets(line,sizeof(line),input)) {
            if(std::string(line)=="capture\n" && std::getenv("LSX4_PROBE_CAPTURE_DIR")) {
                // Explicit readback outside the benchmark window can verify
                // the ending scene without continuous screenshot overhead.
                Lsx4::Apple::RequestDiagnosticFrameCapture(CaptureAppleFrame);
                std::fprintf(stderr,"[APPLE_CAPTURE_ON_DEMAND] performanceRun=0\n");
                continue;
            }
            std::istringstream fields(line);std::uint32_t button{};int pressed=-1;
            if(!(fields>>std::hex>>button>>std::dec>>pressed) || !button ||
               (button&~LSX4_APPLE_PAD_BUTTON_MASK) || (pressed!=0 && pressed!=1)) continue;
            const int result=executor_lsx4_apple_set_pad_button(button,pressed);
            const double elapsed=std::chrono::duration<double>(
                std::chrono::steady_clock::now()-started).count();
            std::fprintf(stderr,"[APPLE_INPUT_PIPE] seconds=%.3f button=0x%x pressed=%d result=%d\n",
                elapsed,button,pressed,result);
        }
        std::fclose(input);
        executor_lsx4_apple_set_pad_button(LSX4_APPLE_PAD_BUTTON_MASK,0);
    }).detach();
}
#include "AppleInputTrace.h"
