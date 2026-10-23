// SPDX-License-Identifier: GPL-2.0-or-later
// Force the complete initialize/launch closure through the Apple linker.
// This CLI-shaped target is not installed as a UIKit application.
#include <cstdio>
extern "C" int executor_lsx4_runtime_initialize(const char*,const char*);
extern "C" int executor_lsx4_runtime_launch_game_jit(const char*);
extern "C" const char* executor_lsx4_runtime_status();
int main(int argc,char** argv) {
    if(argc<2) { std::puts(executor_lsx4_runtime_status());return 0; }
    const int initialized=executor_lsx4_runtime_initialize(argv[1],"");
    if(initialized!=0) return initialized;
    if(argc>2) return executor_lsx4_runtime_launch_game_jit(argv[2]);
    std::puts(executor_lsx4_runtime_status());
    return 0;
}
