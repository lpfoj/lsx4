#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        return 2;
    }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 3;
    }
    if (argc == 3) {
        using Initialize = int (*)(const char*, const char*);
        auto initialize = reinterpret_cast<Initialize>(
            dlsym(library, "executor_lsx4_runtime_initialize"));
        if (initialize == nullptr) {
            std::fprintf(stderr, "dlsym initialize: %s\n", dlerror());
            return 4;
        }
        if (initialize(argv[2], nullptr) != 0) {
            std::fprintf(stderr, "runtime initialize failed\n");
            return 5;
        }
    }
    using SelfTest = const char* (*)();
    auto selftest = reinterpret_cast<SelfTest>(
        dlsym(library, "executor_lsx4_runtime_jit_selftest"));
    if (selftest == nullptr) {
        std::fprintf(stderr, "dlsym: %s\n", dlerror());
        return 6;
    }
    const char* result = selftest();
    if (result == nullptr) {
        return 7;
    }
    std::fputs(result, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    std::_Exit(0);
}
