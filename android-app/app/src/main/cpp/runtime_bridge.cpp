#include <jni.h>

#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>

#include <fcntl.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace
{
using str_fn = const char* (*)();
using init_fn = int (*)(const char*, const char*);
using path_fn = int (*)(const char*);
using int_fn = int (*)(int);
using android_memory_info_fn =
    int (*)(int, unsigned long long, unsigned long long, int);
using android_process_memory_info_fn =
    int (*)(unsigned long long, unsigned long long, unsigned long long);
using option_fn = int (*)(int, int);
using guest_cpu_backend_set_fn = int (*)(int, const char*, const char*);
using guest_cpu_backend_set_v2_fn =
    int (*)(int, const char*, const char*, const char*);
using guest_cpu_backend_set_v3_fn =
    int (*)(int, const char*, const char*, const char*, const char*, const char*,
            const char*);
using vulkan_driver_axis_set_fn = int (*)(int, const char*, const char*, const char*);
using vulkan_driver_axis_set_v2_fn =
    int (*)(int, const char*, const char*, const char*, const char*);
using vulkan_driver_axis_probe_v2_fn =
    const char* (*)(int, const char*, const char*, const char*, const char*);
using json_report_fn = int (*)(const char*);
using noarg_int_fn = int (*)();
using pad_button_fn = int (*)(unsigned int, int);
using system_ui_gamepad_button_fn = unsigned int (*)(unsigned int, int);
using system_ui_text_input_fn = int (*)(const char*);
using system_ui_text_active_fn = int (*)();
using system_ui_text_submit_fn = int (*)();
using system_ui_text_backspace_fn = int (*)(unsigned int);
using pad_axis_fn = int (*)(int, int);
using pad_touch_fn = int (*)(int, float, float);
using attach_surface_fn = int (*)(void*);
using detach_surface_fn = int (*)();
using present_test_pattern_fn = int (*)(int, int);
using present_homebrew_loader_frame_fn = int (*)(int, int);
using present_frame_dump_fn = int (*)(const char*);
using runtime_hud_stats_fn = int (*)(std::uint64_t*, std::size_t);

std::mutex g_mutex;
void* g_runtime = nullptr;
void* g_ps5_runtime = nullptr;
std::string g_ps5_runtime_path;
str_fn g_ps5_abi = nullptr;
str_fn g_ps5_status = nullptr;
guest_cpu_backend_set_fn g_ps5_set_guest_cpu_backend = nullptr;
guest_cpu_backend_set_v2_fn g_ps5_set_guest_cpu_backend_v2 = nullptr;
guest_cpu_backend_set_v3_fn g_ps5_set_guest_cpu_backend_v3 = nullptr;
str_fn g_ps5_guest_cpu_backend_status = nullptr;
vulkan_driver_axis_set_fn g_ps5_set_vulkan_driver_axis = nullptr;
vulkan_driver_axis_set_v2_fn g_ps5_set_vulkan_driver_axis_v2 = nullptr;
str_fn g_ps5_vulkan_driver_status = nullptr;
init_fn g_ps5_initialize = nullptr;
attach_surface_fn g_ps5_attach_surface = nullptr;
detach_surface_fn g_ps5_detach_surface = nullptr;
path_fn g_ps5_scan_game = nullptr;
path_fn g_ps5_launch_game_jit = nullptr;
pad_button_fn g_ps5_set_pad_button = nullptr;
pad_axis_fn g_ps5_set_pad_axis = nullptr;
int_fn g_ps5_set_runtime_paused = nullptr;
str_fn g_ps5_runtime_lifecycle_status = nullptr;
str_fn g_abi = nullptr;
str_fn g_version = nullptr;
str_fn g_system_info = nullptr;
init_fn g_initialize = nullptr;
str_fn g_firmware_status = nullptr;
attach_surface_fn g_attach_surface = nullptr;
detach_surface_fn g_detach_surface = nullptr;
str_fn g_surface_info = nullptr;
present_test_pattern_fn g_present_test_pattern = nullptr;
present_homebrew_loader_frame_fn g_present_homebrew_loader_frame = nullptr;
present_frame_dump_fn g_present_frame_dump = nullptr;
str_fn g_status = nullptr;
str_fn g_jit_status = nullptr;
str_fn g_fex_proof_metrics = nullptr;
str_fn g_runtime_proof_counters = nullptr;
str_fn g_gpu_residency_metrics = nullptr;
guest_cpu_backend_set_fn g_set_guest_cpu_backend = nullptr;
guest_cpu_backend_set_v2_fn g_set_guest_cpu_backend_v2 = nullptr;
guest_cpu_backend_set_v3_fn g_set_guest_cpu_backend_v3 = nullptr;
str_fn g_guest_cpu_backend_status = nullptr;
vulkan_driver_axis_set_fn g_set_vulkan_driver_axis = nullptr;
vulkan_driver_axis_set_v2_fn g_set_vulkan_driver_axis_v2 = nullptr;
str_fn g_vulkan_driver_status = nullptr;
runtime_hud_stats_fn g_runtime_hud_stats = nullptr;
runtime_hud_stats_fn g_ps5_runtime_hud_stats = nullptr;
std::atomic<bool> g_active_ps5_runtime{false};
str_fn g_jit_selftest = nullptr;
path_fn g_scan_game = nullptr;
path_fn g_launch_game = nullptr;
path_fn g_launch_game_jit = nullptr;
path_fn g_load_embedded_box64 = nullptr;
path_fn g_translator_install_helper = nullptr;
path_fn g_translator_run_guest_elf = nullptr;
path_fn g_prepare_translator_launch = nullptr;
int_fn g_report_translator_result = nullptr;
int_fn g_set_render_scale_percent = nullptr;
json_report_fn g_report_translator_result_json = nullptr;
str_fn g_translator_launch_request = nullptr;
noarg_int_fn g_vortek_ring_selftest = nullptr;
noarg_int_fn g_relocate_imports = nullptr;
noarg_int_fn g_prepare_box64_entry_trampoline = nullptr;
str_fn g_box64_entry_request = nullptr;
str_fn g_embedded_box64_info = nullptr;
noarg_int_fn g_validate_embedded_box64_mapped_entry = nullptr;
noarg_int_fn g_run_embedded_box64_mapped_entry = nullptr;
str_fn g_embedded_box64_mapped_entry_info = nullptr;
pad_button_fn g_set_pad_button = nullptr;
system_ui_gamepad_button_fn g_system_ui_gamepad_button = nullptr;
system_ui_text_input_fn g_system_ui_text_input = nullptr;
system_ui_text_active_fn g_system_ui_text_active = nullptr;
system_ui_text_submit_fn g_system_ui_text_submit = nullptr;
system_ui_text_backspace_fn g_system_ui_text_backspace = nullptr;
pad_axis_fn g_set_pad_axis = nullptr;
pad_touch_fn g_set_touchpad = nullptr;
noarg_int_fn g_audio_init = nullptr;
int_fn g_set_audio_enabled = nullptr;
int_fn g_set_runtime_paused = nullptr;
int_fn g_report_android_memory_pressure = nullptr;
android_memory_info_fn g_report_android_memory_info = nullptr;
android_process_memory_info_fn g_report_android_process_memory_info = nullptr;
noarg_int_fn g_android_memory_pressure_monitor_requested = nullptr;
str_fn g_runtime_lifecycle_status = nullptr;
option_fn g_set_managed_optimization = nullptr;
option_fn g_ps5_set_managed_optimization = nullptr;
noarg_int_fn g_audio_probe_tone = nullptr;
str_fn g_audio_status = nullptr;
std::string g_guest_cpu_backend_bridge_status =
    R"({"schema":"lsx4.guest-cpu-backend.v1","platform":"unknown","requested":"current","effective":"current","supported":true,"fallbackReason":"none","selectionLocked":false,"source":"bridge-default"})";
std::string g_vulkan_driver_bridge_status =
    R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"system","effective":"system","fallbackReason":"bridge-default"})";

std::string g_fallback_files_root;
std::string g_fallback_native_lib_dir;

using present_pump_start_fn = int (*)(const char*, int);
present_pump_start_fn g_start_present_pump = nullptr;
noarg_int_fn g_stop_present_pump = nullptr;

using run_embedded_elf_fn = int (*)(const char*, const char*, const char*);
run_embedded_elf_fn g_run_embedded_linux_elf = nullptr;






class HostPmuSampler
{
public:
    static constexpr std::size_t kEventCount = 4;

    struct Snapshot
    {
        bool available{};
        int error{};
        std::uint64_t timestamp_ns{};
        std::array<std::uint64_t, kEventCount> values{};
    };

    ~HostPmuSampler()
    {
        CloseEvents();
    }

    Snapshot Sample()
    {
        std::lock_guard lock{mutex_};
        Snapshot snapshot{};
        timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        {
            snapshot.timestamp_ns = static_cast<std::uint64_t>(now.tv_sec) *
                                        1'000'000'000ull +
                                    static_cast<std::uint64_t>(now.tv_nsec);
        }

        if (!attempted_)
        {
            attempted_ = true;
            OpenEvents();
        }
        snapshot.error = error_;
        if (!available_)
        {
            return snapshot;
        }

        for (std::size_t index = 0; index < events_.size(); ++index)
        {
            PerfReadValue value{};
            const ssize_t count = ::read(events_[index], &value, sizeof(value));
            if (count != static_cast<ssize_t>(sizeof(value)) || value.time_running == 0)
            {
                error_ = count < 0 ? errno : EIO;
                __android_log_print(ANDROID_LOG_WARN, "LSX4Pmu",
                                    "host PMU read failed event=%zu bytes=%zd running=%llu errno=%d",
                                    index, count,
                                    static_cast<unsigned long long>(value.time_running),
                                    error_);
                snapshot.error = error_;
                available_ = false;
                CloseEvents();
                return snapshot;
            }
            snapshot.values[index] = Scale(value);
        }
        snapshot.available = true;
        return snapshot;
    }

private:
    struct PerfReadValue
    {
        std::uint64_t value{};
        std::uint64_t time_enabled{};
        std::uint64_t time_running{};
    };

    static std::uint64_t Scale(const PerfReadValue& value)
    {
        if (value.time_enabled == value.time_running)
        {
            return value.value;
        }
        const long double scaled = static_cast<long double>(value.value) *
                                   static_cast<long double>(value.time_enabled) /
                                   static_cast<long double>(value.time_running);
        const long double maximum =
            static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
        return static_cast<std::uint64_t>(std::min(scaled, maximum));
    }

    void OpenEvents()
    {
        constexpr std::array<std::uint64_t, kEventCount> configs{
            PERF_COUNT_HW_INSTRUCTIONS,
            PERF_COUNT_HW_CPU_CYCLES,
            PERF_COUNT_HW_BRANCH_INSTRUCTIONS,
            PERF_COUNT_HW_BRANCH_MISSES,
        };
        for (std::size_t index = 0; index < configs.size(); ++index)
        {
            perf_event_attr attr{};
            attr.type = PERF_TYPE_HARDWARE;
            attr.size = sizeof(attr);
            attr.config = configs[index];
            attr.disabled = 0;
            attr.inherit = 1;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                               PERF_FORMAT_TOTAL_TIME_RUNNING;
            const int fd = static_cast<int>(::syscall(
                __NR_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
            if (fd < 0)
            {
                error_ = errno;
                __android_log_print(ANDROID_LOG_WARN, "LSX4Pmu",
                                    "host PMU unavailable event=%zu errno=%d",
                                    index, error_);
                CloseEvents();
                return;
            }
            events_[index] = fd;
        }
        available_ = true;
        __android_log_print(ANDROID_LOG_INFO, "LSX4Pmu",
                            "host PMU enabled inherited=1 events=%zu",
                            events_.size());
    }

    void CloseEvents()
    {
        for (int& fd : events_)
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }
    }

    std::mutex mutex_;
    std::array<int, kEventCount> events_{-1, -1, -1, -1};
    bool attempted_{};
    bool available_{};
    int error_{};
};

HostPmuSampler g_host_pmu_sampler;

jstring to_jstring(JNIEnv* env, const std::string& value)
{
    return env->NewStringUTF(value.c_str());
}

void append_utf8_code_point(std::string& output, std::uint32_t code_point)
{
    if (code_point <= 0x7f)
    {
        output.push_back(static_cast<char>(code_point));
    }
    else if (code_point <= 0x7ff)
    {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else if (code_point <= 0xffff)
    {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else
    {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

bool jstring_to_utf8(JNIEnv* env, jstring value, std::string& output)
{
    if (value == nullptr)
    {
        return false;
    }

    const jsize length = env->GetStringLength(value);
    const jchar* chars = env->GetStringChars(value, nullptr);
    if (chars == nullptr)
    {
        return false;
    }

    output.clear();
    output.reserve(static_cast<std::size_t>(length) * 3);
    for (jsize i = 0; i < length; ++i)
    {
        std::uint32_t code_point = chars[i];
        if (code_point >= 0xd800 && code_point <= 0xdbff)
        {
            if (i + 1 < length && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff)
            {
                code_point = 0x10000 + ((code_point - 0xd800) << 10) +
                             (static_cast<std::uint32_t>(chars[++i]) - 0xdc00);
            }
            else
            {
                code_point = 0xfffd;
            }
        }
        else if (code_point >= 0xdc00 && code_point <= 0xdfff)
        {
            code_point = 0xfffd;
        }
        append_utf8_code_point(output, code_point);
    }
    env->ReleaseStringChars(value, chars);
    return true;
}

template <typename T>
T resolve_required(JNIEnv* env, const char* name)
{
    void* symbol = dlsym(g_runtime, name);
    if (!symbol)
    {
        std::string error = "Missing runtime symbol: ";
        error += name;
        env->ThrowNew(env->FindClass("java/lang/UnsatisfiedLinkError"), error.c_str());
        return nullptr;
    }
    return reinterpret_cast<T>(symbol);
}

template <typename T>
T resolve_optional(const char* name)
{
    return reinterpret_cast<T>(dlsym(g_runtime, name));
}

template <typename T>
T resolve_ps5_required(JNIEnv* env, const char* name)
{
    void* symbol = dlsym(g_ps5_runtime, name);
    if (!symbol)
    {
        std::string error = "Missing PS5 runtime symbol: ";
        error += name;
        env->ThrowNew(env->FindClass("java/lang/UnsatisfiedLinkError"), error.c_str());
        return nullptr;
    }
    return reinterpret_cast<T>(symbol);
}

template <typename T>
T resolve_ps5_optional(const char* name)
{
    return reinterpret_cast<T>(dlsym(g_ps5_runtime, name));
}

const char* call_or_empty(str_fn fn)
{
    return fn ? fn() : "";
}

bool fb_is_file(const std::string& p)
{
    struct stat st {};
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool fb_mkdir_p(const std::string& p)
{
    if (p.empty()) return false;
    std::string acc;
    for (size_t i = 0; i < p.size(); ++i)
    {
        if (p[i] == '/' && i > 0)
        {
            mkdir(acc.c_str(), 0700);
        }
        acc += p[i];
    }
    mkdir(acc.c_str(), 0700);
    struct stat st {};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fb_copy_file(const std::string& src, const std::string& dst, mode_t mode)
{
    const int in = open(src.c_str(), O_RDONLY);
    if (in < 0) return false;
    const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { close(in); return false; }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0)
    {
        ssize_t off = 0;
        while (off < n)
        {
            const ssize_t w = write(out, buf + off, n - off);
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    if (n < 0) ok = false;
    close(in);
    close(out);
    if (ok) chmod(dst.c_str(), mode);
    return ok;
}

std::string fb_read_small(const std::string& path)
{
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    char buf[512];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "";
    std::string s(buf, static_cast<size_t>(n));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

int fb_install(const std::string& source_dir)
{
    const std::string marker = "/lsx4-home/";
    const size_t pos = source_dir.find(marker);
    if (pos == std::string::npos) return -10;
    const std::string files_root = source_dir.substr(0, pos);
    const std::string install_root = files_root + "/box64";
    if (!fb_mkdir_p(install_root + "/bin") || !fb_mkdir_p(install_root + "/lib"))
        return -2;
    struct Spec { const char* rel; mode_t mode; };
    const Spec specs[] = {
        {"bin/box64", 0700}, {"lib/ld-linux-aarch64.so.1", 0700},
        {"lib/libc.so.6", 0600}, {"lib/libm.so.6", 0600},
        {"lib/libresolv.so.2", 0600}, {"default.box64rc", 0600},
        {"env_vars.json", 0600},
    };
    for (const Spec& s : specs)
    {
        if (!fb_copy_file(source_dir + "/" + s.rel, install_root + "/" + s.rel, s.mode))
            return -3;
    }
    g_fallback_files_root = files_root;
    g_fallback_native_lib_dir = fb_read_small(source_dir + "/native-library-dir.txt");
    return 0;
}

int fb_run_guest(const std::string& elf)
{
    if (g_fallback_files_root.empty() || g_fallback_native_lib_dir.empty()) return -11;
    if (!fb_is_file(elf)) return -41;
    const std::string files_root = g_fallback_files_root;
    const std::string nld = g_fallback_native_lib_dir;
    const std::string translator_dir = files_root + "/lsx4-home/translator";
    const std::string ld_path = translator_dir + ":" + files_root + "/box64/lib";
    const std::string loader = nld + "/libwinlator_ld_linux_aarch64.so";
    const std::string box64 = nld + "/libwinlator_box64.so";
    if (!fb_is_file(loader) || !fb_is_file(box64)) return -30;

    const std::string frame_dump = translator_dir + "/executor-videoout-frame.bin";
    unlink(frame_dump.c_str());
    if (g_start_present_pump) g_start_present_pump(frame_dump.c_str(), 16);

    const bool embedded_enabled = fb_is_file(translator_dir + "/.embedded-elf-enabled");
    if (embedded_enabled && g_run_embedded_linux_elf &&
        (elf.find("SDL2") != std::string::npos || elf.find("threading") != std::string::npos ||
         elf.find("pthread_contract") != std::string::npos)) {
        std::string env;
        env += "HOME=" + files_root + "/lsx4-home\n";
        env += "TMPDIR=" + translator_dir + "\n";
        env += "BOX64_LD_LIBRARY_PATH=" + translator_dir + "\n";
        env += "BOX64_DYNAREC=1\nBOX64_NOBANNER=1\nBOX64_PREFER_EMULATED=1\nBOX64_LOG=2\n";
        env += "EXECUTOR_ENTRY_ABI_PS4_RDI_RSP=1\n";
        env += "EXECUTOR_APP0_HOST_ROOT=" + translator_dir + "/app0\n";
        env += "BOX64_EMULATED_LIBS=libkernel.so,libSceVideoOut.so,libSceSysmodule.so,"
               "libSceFreeType.so,libSceUserService.so,libSceAudioOut.so,libSceCommonDialog.so,"
               "libSceMsgDialog.so,libScePad.so,libSceKeyboard.so,libScePigletv2VSH.so\n";
        return g_run_embedded_linux_elf(elf.c_str(), translator_dir.c_str(), env.c_str());
    }

    const pid_t pid = fork();
    if (pid < 0) return -31;
    if (pid == 0)
    {
        chdir(translator_dir.c_str());
        const int of = open((translator_dir + "/box64.stdout.log").c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (of >= 0) { dup2(of, STDOUT_FILENO); close(of); }
        const int ef = open((translator_dir + "/box64.stderr.log").c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (ef >= 0) { dup2(ef, STDERR_FILENO); close(ef); }
        std::vector<std::string> env = {
            "PATH=/system/bin:/system/xbin:/apex/com.android.runtime/bin",
            "HOME=" + files_root + "/lsx4-home",
            "TMPDIR=" + translator_dir,
            "EXECUTOR_APP0_HOST_ROOT=" + translator_dir + "/app0",
            "LD_LIBRARY_PATH=" + ld_path,
            "BOX64_LD_LIBRARY_PATH=" + ld_path,
            "BOX64_PATH=" + files_root + "/box64/bin",
            "BOX64_RCFILE=" + files_root + "/box64/default.box64rc",
            "BOX64_LOG=1", "BOX64_NOBANNER=1", "BOX64_DYNAREC=1",
            "BOX64_PREFER_EMULATED=1", "BOX64_PREFER_WRAPPED=0",
            "BOX64_EMULATED_LIBS=libkernel.so,libSceVideoOut.so,libSceSysmodule.so,"
            "libSceFreeType.so,libSceUserService.so,libSceAudioOut.so,libSceCommonDialog.so,"
            "libSceMsgDialog.so,libScePad.so,libSceKeyboard.so,libSceHttp.so,libSceNet.so,"
            "libSceSsl.so,libScePigletv2VSH.so,libScePrecompiledShaders.so,libSceSystemService.so,"
            "libSceLibcInternal.so,libSceRtc.so,libSceShellCoreUtil.so",
        };
        std::vector<char*> envp;
        for (std::string& e : env) envp.push_back(e.data());
        envp.push_back(nullptr);
        std::vector<std::string> args = {loader, box64, elf};
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execve(loader.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (g_stop_present_pump) g_stop_present_pump();
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -(128 + WTERMSIG(status));
    return -39;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_load(JNIEnv* env, jclass, jstring path)
{
    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    if (!raw_path)
    {
        return to_jstring(env, "Runtime path is empty.");
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_runtime)
    {
        dlclose(g_runtime);
        g_runtime = nullptr;
    }

    g_runtime = dlopen(raw_path, RTLD_NOW | RTLD_LOCAL);
    std::string loaded_path = raw_path;
    env->ReleaseStringUTFChars(path, raw_path);

    if (!g_runtime)
    {
        std::string error = "dlopen failed: ";
        error += dlerror();
        env->ThrowNew(env->FindClass("java/lang/UnsatisfiedLinkError"), error.c_str());
        return nullptr;
    }

    g_abi = resolve_required<str_fn>(env, "executor_lsx4_runtime_abi");
    g_version = resolve_required<str_fn>(env, "executor_lsx4_runtime_version");
    g_system_info = resolve_required<str_fn>(env, "executor_lsx4_runtime_system_info");
    g_initialize = resolve_required<init_fn>(env, "executor_lsx4_runtime_initialize");
    g_firmware_status = resolve_optional<str_fn>("executor_lsx4_runtime_firmware_status");
    g_attach_surface = resolve_optional<attach_surface_fn>("executor_lsx4_runtime_attach_surface");
    g_detach_surface = resolve_optional<detach_surface_fn>("executor_lsx4_runtime_detach_surface");
    g_surface_info = resolve_optional<str_fn>("executor_lsx4_runtime_surface_info");
    g_present_test_pattern =
        resolve_optional<present_test_pattern_fn>("executor_lsx4_runtime_present_test_pattern");
    g_present_homebrew_loader_frame = resolve_optional<present_homebrew_loader_frame_fn>(
        "executor_lsx4_runtime_present_homebrew_loader_frame");
    g_present_frame_dump =
        resolve_optional<present_frame_dump_fn>("executor_lsx4_runtime_present_frame_dump_file");
    g_status = resolve_optional<str_fn>("executor_lsx4_runtime_status");
    g_jit_status = resolve_optional<str_fn>("executor_lsx4_runtime_jit_status");
    g_fex_proof_metrics = resolve_optional<str_fn>(
        "executor_lsx4_runtime_fex_proof_metrics");
    g_runtime_proof_counters = resolve_optional<str_fn>(
        "executor_lsx4_runtime_proof_counters");
    g_gpu_residency_metrics = resolve_optional<str_fn>(
        "executor_lsx4_runtime_gpu_residency_metrics");
    g_report_android_memory_pressure = resolve_optional<int_fn>(
        "executor_lsx4_runtime_report_android_memory_pressure");
    g_report_android_memory_info = resolve_optional<android_memory_info_fn>(
        "executor_lsx4_runtime_report_android_memory_info");
    g_report_android_process_memory_info =
        resolve_optional<android_process_memory_info_fn>(
            "executor_lsx4_runtime_report_android_process_memory_info");
    g_android_memory_pressure_monitor_requested = resolve_optional<noarg_int_fn>(
        "executor_lsx4_runtime_android_memory_pressure_monitor_requested");
    g_set_guest_cpu_backend = resolve_optional<guest_cpu_backend_set_fn>(
        "executor_lsx4_runtime_set_guest_cpu_backend");
    g_set_guest_cpu_backend_v2 = resolve_optional<guest_cpu_backend_set_v2_fn>(
        "executor_lsx4_runtime_set_guest_cpu_backend_v2");
    g_set_guest_cpu_backend_v3 = resolve_optional<guest_cpu_backend_set_v3_fn>(
        "executor_lsx4_runtime_set_guest_cpu_backend_v3");
    g_guest_cpu_backend_status = resolve_optional<str_fn>(
        "executor_lsx4_runtime_guest_cpu_backend_status");
    g_set_vulkan_driver_axis = resolve_optional<vulkan_driver_axis_set_fn>(
        "executor_lsx4_runtime_set_vulkan_driver_axis");
    g_set_vulkan_driver_axis_v2 = resolve_optional<vulkan_driver_axis_set_v2_fn>(
        "executor_lsx4_runtime_set_vulkan_driver_axis_v2");
    g_vulkan_driver_status = resolve_optional<str_fn>(
        "executor_lsx4_runtime_vulkan_driver_status");
    g_runtime_hud_stats =
        resolve_optional<runtime_hud_stats_fn>("executor_lsx4_runtime_hud_stats");
    g_jit_selftest = resolve_optional<str_fn>("executor_lsx4_runtime_jit_selftest");
    g_scan_game = resolve_optional<path_fn>("executor_lsx4_runtime_scan_game");
    g_launch_game = resolve_optional<path_fn>("executor_lsx4_runtime_launch_game");
    g_launch_game_jit =
        resolve_optional<path_fn>("executor_lsx4_runtime_launch_game_jit");
    g_load_embedded_box64 =
        resolve_optional<path_fn>("executor_lsx4_runtime_load_embedded_box64");
    g_translator_install_helper =
        resolve_optional<path_fn>("executor_lsx4_runtime_translator_install_helper");
    g_translator_run_guest_elf =
        resolve_optional<path_fn>("executor_lsx4_runtime_translator_run_guest_elf");
    g_start_present_pump =
        resolve_optional<present_pump_start_fn>("executor_lsx4_runtime_start_present_pump");
    g_stop_present_pump =
        resolve_optional<noarg_int_fn>("executor_lsx4_runtime_stop_present_pump");
    g_run_embedded_linux_elf =
        resolve_optional<run_embedded_elf_fn>("executor_lsx4_runtime_run_embedded_linux_elf");
    g_prepare_translator_launch =
        resolve_optional<path_fn>("executor_lsx4_runtime_prepare_translator_launch");
    g_report_translator_result =
        resolve_optional<int_fn>("executor_lsx4_runtime_report_translator_result");
    g_set_render_scale_percent =
        resolve_optional<int_fn>("executor_lsx4_runtime_set_render_scale_percent");
    g_report_translator_result_json =
        resolve_optional<json_report_fn>("executor_lsx4_runtime_report_translator_result_json");
    g_translator_launch_request =
        resolve_optional<str_fn>("executor_lsx4_runtime_translator_launch_request");
    g_vortek_ring_selftest = resolve_optional<noarg_int_fn>("executor_vortek_ring_selftest");
    g_relocate_imports = resolve_optional<noarg_int_fn>("executor_lsx4_runtime_relocate_imports");
    g_prepare_box64_entry_trampoline =
        resolve_optional<noarg_int_fn>("executor_lsx4_runtime_prepare_box64_entry_trampoline");
    g_box64_entry_request =
        resolve_optional<str_fn>("executor_lsx4_runtime_box64_entry_request");
    g_embedded_box64_info =
        resolve_optional<str_fn>("executor_lsx4_runtime_embedded_box64_info");
    g_validate_embedded_box64_mapped_entry =
        resolve_optional<noarg_int_fn>("executor_lsx4_runtime_validate_embedded_box64_mapped_entry");
    g_run_embedded_box64_mapped_entry =
        resolve_optional<noarg_int_fn>("executor_lsx4_runtime_run_embedded_box64_mapped_entry");
    g_embedded_box64_mapped_entry_info =
        resolve_optional<str_fn>("executor_lsx4_runtime_embedded_box64_mapped_entry_info");
    g_set_pad_button =
        resolve_optional<pad_button_fn>("executor_lsx4_runtime_set_pad_button");
    g_system_ui_gamepad_button = resolve_optional<system_ui_gamepad_button_fn>(
        "executor_lsx4_runtime_system_ui_gamepad_button");
    g_system_ui_text_input = resolve_optional<system_ui_text_input_fn>(
        "executor_lsx4_runtime_system_ui_text_input");
    g_system_ui_text_active = resolve_optional<system_ui_text_active_fn>(
        "executor_lsx4_runtime_system_ui_text_active");
    g_system_ui_text_submit = resolve_optional<system_ui_text_submit_fn>(
        "executor_lsx4_runtime_system_ui_text_submit");
    g_system_ui_text_backspace = resolve_optional<system_ui_text_backspace_fn>(
        "executor_lsx4_runtime_system_ui_text_backspace");
    g_set_pad_axis = resolve_optional<pad_axis_fn>("executor_lsx4_runtime_set_pad_axis");
    g_set_touchpad =
        resolve_optional<pad_touch_fn>("executor_lsx4_runtime_set_touchpad");
    g_audio_init = resolve_optional<noarg_int_fn>("executor_lsx4_runtime_audio_init");
    g_set_audio_enabled =
        resolve_optional<int_fn>("executor_lsx4_runtime_set_audio_enabled");
    g_set_runtime_paused =
        resolve_optional<int_fn>("executor_lsx4_runtime_set_paused");
    g_runtime_lifecycle_status =
        resolve_optional<str_fn>("executor_lsx4_runtime_lifecycle_status");
    g_set_managed_optimization = resolve_optional<option_fn>(
        "executor_lsx4_runtime_set_managed_optimization");
    g_audio_probe_tone =
        resolve_optional<noarg_int_fn>("executor_lsx4_runtime_audio_probe_tone");
    g_audio_status = resolve_optional<str_fn>("executor_lsx4_runtime_audio_status");
    if (env->ExceptionCheck())
    {
        return nullptr;
    }
    g_active_ps5_runtime.store(false, std::memory_order_release);

    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "[EXECUTOR_UI_BUILD_MARKER] id=ui-bridge-live-cusa-marker-r1 "
                        "date=\"%s\" time=\"%s\" file=%s",
                        __DATE__, __TIME__, __FILE__);
    __android_log_print(ANDROID_LOG_INFO, "LSX4", "Loaded runtime: %s",
                        loaded_path.c_str());
    return to_jstring(env, "Loaded runtime: " + loaded_path);
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_loadPs5(JNIEnv* env, jclass, jstring path)
{
    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    if (!raw_path)
    {
        return to_jstring(env, "PS5 runtime path is empty.");
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    std::string loaded_path = raw_path;
    env->ReleaseStringUTFChars(path, raw_path);
    if (g_ps5_runtime && g_ps5_runtime_path == loaded_path)
    {
        g_active_ps5_runtime.store(true, std::memory_order_release);
        return to_jstring(
            env, "PS5 runtime already loaded: " + loaded_path);
    }
    if (g_ps5_runtime)
    {
        dlclose(g_ps5_runtime);
        g_ps5_runtime = nullptr;
        g_ps5_runtime_path.clear();
    }

    g_ps5_runtime = dlopen(loaded_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_ps5_runtime)
    {
        std::string error = "PS5 dlopen failed: ";
        error += dlerror();
        env->ThrowNew(env->FindClass("java/lang/UnsatisfiedLinkError"), error.c_str());
        return nullptr;
    }

    g_ps5_abi = resolve_ps5_required<str_fn>(
        env, "executor_lsx4_ps5_runtime_abi");
    g_ps5_status = resolve_ps5_required<str_fn>(
        env, "executor_lsx4_ps5_runtime_status");
    g_ps5_set_guest_cpu_backend = resolve_ps5_optional<guest_cpu_backend_set_fn>(
        "executor_lsx4_ps5_runtime_set_guest_cpu_backend");
    g_ps5_set_guest_cpu_backend_v2 =
        resolve_ps5_optional<guest_cpu_backend_set_v2_fn>(
            "executor_lsx4_ps5_runtime_set_guest_cpu_backend_v2");
    g_ps5_set_guest_cpu_backend_v3 =
        resolve_ps5_optional<guest_cpu_backend_set_v3_fn>(
            "executor_lsx4_ps5_runtime_set_guest_cpu_backend_v3");
    g_ps5_guest_cpu_backend_status = resolve_ps5_optional<str_fn>(
        "executor_lsx4_ps5_runtime_guest_cpu_backend_status");
    g_ps5_set_vulkan_driver_axis = resolve_ps5_optional<vulkan_driver_axis_set_fn>(
        "executor_lsx4_ps5_runtime_set_vulkan_driver_axis");
    g_ps5_set_vulkan_driver_axis_v2 = resolve_ps5_optional<vulkan_driver_axis_set_v2_fn>(
        "executor_lsx4_ps5_runtime_set_vulkan_driver_axis_v2");
    g_ps5_vulkan_driver_status = resolve_ps5_optional<str_fn>(
        "executor_lsx4_ps5_runtime_vulkan_driver_status");
    g_ps5_initialize = resolve_ps5_required<init_fn>(
        env, "executor_lsx4_ps5_runtime_initialize_android");
    g_ps5_attach_surface = resolve_ps5_required<attach_surface_fn>(
        env, "executor_lsx4_ps5_runtime_attach_surface");
    g_ps5_detach_surface = resolve_ps5_required<detach_surface_fn>(
        env, "executor_lsx4_ps5_runtime_detach_surface");
    g_ps5_scan_game = resolve_ps5_required<path_fn>(
        env, "executor_lsx4_ps5_runtime_scan_game");
    g_ps5_launch_game_jit = resolve_ps5_required<path_fn>(
        env, "executor_lsx4_ps5_runtime_launch_game_jit");
    g_ps5_set_pad_button = resolve_ps5_required<pad_button_fn>(
        env, "executor_lsx4_ps5_runtime_set_pad_button");
    g_ps5_set_pad_axis = resolve_ps5_required<pad_axis_fn>(
        env, "executor_lsx4_ps5_runtime_set_pad_axis");
    g_ps5_set_runtime_paused = resolve_ps5_optional<int_fn>(
        "executor_lsx4_ps5_runtime_set_paused");
    g_ps5_runtime_lifecycle_status = resolve_ps5_optional<str_fn>(
        "executor_lsx4_ps5_runtime_lifecycle_status");
    g_ps5_runtime_hud_stats = resolve_ps5_optional<runtime_hud_stats_fn>(
        "executor_lsx4_ps5_runtime_hud_stats");
    g_ps5_set_managed_optimization = resolve_ps5_optional<option_fn>(
        "executor_lsx4_ps5_runtime_set_managed_optimization");
    if (env->ExceptionCheck())
    {
        return nullptr;
    }
    g_ps5_runtime_path = loaded_path;
    g_active_ps5_runtime.store(true, std::memory_order_release);

    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "Loaded isolated PS5 runtime: %s",
                        loaded_path.c_str());
    return to_jstring(env, "Loaded isolated PS5 runtime: " + loaded_path);
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_ps5Abi(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_ps5_abi));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setGuestCpuBackend(
    JNIEnv* env, jclass, jint requested_backend, jstring adapter_path,
    jstring runtime_root, jstring launch_title_id, jstring hardware_profile,
    jstring driver_family, jstring driver_version)
{
    const char* raw_adapter = adapter_path
        ? env->GetStringUTFChars(adapter_path, nullptr) : nullptr;
    const char* raw_root = runtime_root
        ? env->GetStringUTFChars(runtime_root, nullptr) : nullptr;
    const char* raw_title = launch_title_id
        ? env->GetStringUTFChars(launch_title_id, nullptr) : nullptr;
    const char* raw_hardware = hardware_profile
        ? env->GetStringUTFChars(hardware_profile, nullptr) : nullptr;
    const char* raw_driver_family = driver_family
        ? env->GetStringUTFChars(driver_family, nullptr) : nullptr;
    const char* raw_driver_version = driver_version
        ? env->GetStringUTFChars(driver_version, nullptr) : nullptr;
    const std::string adapter = raw_adapter ? raw_adapter : "";
    const std::string root = raw_root ? raw_root : "";
    const std::string title = raw_title ? raw_title : "";
    const std::string hardware = raw_hardware ? raw_hardware : "";
    const std::string family = raw_driver_family ? raw_driver_family : "";
    const std::string version = raw_driver_version ? raw_driver_version : "";
    if (raw_adapter)
    {
        env->ReleaseStringUTFChars(adapter_path, raw_adapter);
    }
    if (raw_root)
    {
        env->ReleaseStringUTFChars(runtime_root, raw_root);
    }
    if (raw_title)
    {
        env->ReleaseStringUTFChars(launch_title_id, raw_title);
    }
    if (raw_hardware)
    {
        env->ReleaseStringUTFChars(hardware_profile, raw_hardware);
    }
    if (raw_driver_family)
    {
        env->ReleaseStringUTFChars(driver_family, raw_driver_family);
    }
    if (raw_driver_version)
    {
        env->ReleaseStringUTFChars(driver_version, raw_driver_version);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    guest_cpu_backend_set_v3_fn setter_v3 =
        g_active_ps5_runtime.load(std::memory_order_acquire)
            ? g_ps5_set_guest_cpu_backend_v3
            : g_set_guest_cpu_backend_v3;
    guest_cpu_backend_set_v2_fn setter_v2 =
        g_active_ps5_runtime.load(std::memory_order_acquire)
            ? g_ps5_set_guest_cpu_backend_v2
            : g_set_guest_cpu_backend_v2;
    guest_cpu_backend_set_fn setter =
        g_active_ps5_runtime.load(std::memory_order_acquire)
            ? g_ps5_set_guest_cpu_backend
            : g_set_guest_cpu_backend;
    if (setter_v3 || setter_v2 || setter)
    {
        const int effective = setter_v3
            ? setter_v3(static_cast<int>(requested_backend), adapter.c_str(),
                        root.c_str(), title.c_str(), hardware.c_str(),
                        family.c_str(), version.c_str())
            : (setter_v2
                   ? setter_v2(static_cast<int>(requested_backend), adapter.c_str(),
                               root.c_str(), title.c_str())
                   : setter(static_cast<int>(requested_backend), adapter.c_str(),
                            root.c_str()));
        str_fn status = g_active_ps5_runtime.load(std::memory_order_acquire)
            ? g_ps5_guest_cpu_backend_status
            : g_guest_cpu_backend_status;
        if (status)
        {
            const char* value = status();
            g_guest_cpu_backend_bridge_status = value ? value : "{}";
        }
        return effective == 1 ? 1 : 0;
    }

    const bool requested_fex = requested_backend == 1;
    g_guest_cpu_backend_bridge_status = requested_fex
        ? R"({"schema":"lsx4.guest-cpu-backend.v1","platform":"unknown","requested":"fex-experimental","effective":"current","supported":false,"fallbackReason":"runtime-symbol-missing","selectionLocked":false,"source":"jni-bridge"})"
        : R"({"schema":"lsx4.guest-cpu-backend.v1","platform":"unknown","requested":"current","effective":"current","supported":true,"fallbackReason":"none","selectionLocked":false,"source":"jni-bridge-legacy-runtime"})";
    return 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_guestCpuBackendStatus(JNIEnv* env, jclass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    str_fn status = g_active_ps5_runtime.load(std::memory_order_acquire)
        ? g_ps5_guest_cpu_backend_status
        : g_guest_cpu_backend_status;
    if (status)
    {
        const char* value = status();
        g_guest_cpu_backend_bridge_status = value ? value : "{}";
    }
    return to_jstring(env, g_guest_cpu_backend_bridge_status);
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setVulkanDriverAxis(
    JNIEnv* env, jclass, jboolean requested_turnip, jstring native_library_dir,
    jstring turnip_library_dir, jstring report_path, jstring turnip_sha256)
{
    const char* raw_native_dir = native_library_dir
        ? env->GetStringUTFChars(native_library_dir, nullptr) : nullptr;
    const char* raw_turnip_dir = turnip_library_dir
        ? env->GetStringUTFChars(turnip_library_dir, nullptr) : nullptr;
    const char* raw_report = report_path
        ? env->GetStringUTFChars(report_path, nullptr) : nullptr;
    const char* raw_sha = turnip_sha256
        ? env->GetStringUTFChars(turnip_sha256, nullptr) : nullptr;
    const std::string native_dir = raw_native_dir ? raw_native_dir : "";
    const std::string turnip_dir = raw_turnip_dir ? raw_turnip_dir : "";
    const std::string report = raw_report ? raw_report : "";
    const std::string sha = raw_sha ? raw_sha : "";
    if (raw_sha)
        env->ReleaseStringUTFChars(turnip_sha256, raw_sha);
    if (raw_report)
        env->ReleaseStringUTFChars(report_path, raw_report);
    if (raw_native_dir)
        env->ReleaseStringUTFChars(native_library_dir, raw_native_dir);
    if (raw_turnip_dir)
        env->ReleaseStringUTFChars(turnip_library_dir, raw_turnip_dir);

    std::lock_guard<std::mutex> lock(g_mutex);
    const bool ps5 = g_active_ps5_runtime.load(std::memory_order_acquire);
    vulkan_driver_axis_set_fn setter =
        ps5 ? g_ps5_set_vulkan_driver_axis : g_set_vulkan_driver_axis;
    vulkan_driver_axis_set_v2_fn setter_v2 =
        ps5 ? g_ps5_set_vulkan_driver_axis_v2 : g_set_vulkan_driver_axis_v2;
    str_fn status = ps5 ? g_ps5_vulkan_driver_status : g_vulkan_driver_status;
    if (setter_v2)
    {
        const int effective = setter_v2(requested_turnip ? 1 : 0, native_dir.c_str(),
                                        turnip_dir.c_str(), report.c_str(), sha.c_str());
        if (status)
        {
            const char* value = status();
            g_vulkan_driver_bridge_status = value ? value : "{}";
        }
        return effective;
    }
    if (setter && (!requested_turnip || turnip_dir == native_dir))
    {
        const int effective = setter(requested_turnip ? 1 : 0, native_dir.c_str(),
                                     report.c_str(), sha.c_str());
        if (status)
        {
            const char* value = status();
            g_vulkan_driver_bridge_status = value ? value : "{}";
        }
        return effective;
    }
    if (setter && requested_turnip)
    {
        g_vulkan_driver_bridge_status =
            R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"turnip","effective":"system","fallbackReason":"runtime-driver-path-unsupported","selectionLocked":true,"source":"jni-bridge-v1-runtime"})";
        return 0;
    }
    g_vulkan_driver_bridge_status = requested_turnip
        ? R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"turnip","effective":"system","fallbackReason":"runtime-symbol-missing","selectionLocked":true})"
        : R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"system","effective":"system","fallbackReason":"runtime-symbol-missing","selectionLocked":true})";
    return 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_probeVulkanDriverAxis(
    JNIEnv* env, jclass, jstring runtime_path, jstring native_library_dir,
    jstring turnip_library_dir, jstring report_path, jstring turnip_sha256)
{
    const char* raw_runtime = runtime_path
        ? env->GetStringUTFChars(runtime_path, nullptr) : nullptr;
    const char* raw_native_dir = native_library_dir
        ? env->GetStringUTFChars(native_library_dir, nullptr) : nullptr;
    const char* raw_turnip_dir = turnip_library_dir
        ? env->GetStringUTFChars(turnip_library_dir, nullptr) : nullptr;
    const char* raw_report = report_path
        ? env->GetStringUTFChars(report_path, nullptr) : nullptr;
    const char* raw_sha = turnip_sha256
        ? env->GetStringUTFChars(turnip_sha256, nullptr) : nullptr;
    const std::string runtime = raw_runtime ? raw_runtime : "";
    const std::string native_dir = raw_native_dir ? raw_native_dir : "";
    const std::string turnip_dir = raw_turnip_dir ? raw_turnip_dir : "";
    const std::string report = raw_report ? raw_report : "";
    const std::string sha = raw_sha ? raw_sha : "";
    if (raw_sha)
        env->ReleaseStringUTFChars(turnip_sha256, raw_sha);
    if (raw_report)
        env->ReleaseStringUTFChars(report_path, raw_report);
    if (raw_turnip_dir)
        env->ReleaseStringUTFChars(turnip_library_dir, raw_turnip_dir);
    if (raw_native_dir)
        env->ReleaseStringUTFChars(native_library_dir, raw_native_dir);
    if (raw_runtime)
        env->ReleaseStringUTFChars(runtime_path, raw_runtime);

    void* const probe_runtime = dlopen(runtime.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!probe_runtime)
    {
        return to_jstring(
            env,
            R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"turnip","effective":"system","fallbackReason":"runtime-driver-probe-dlopen-failed","selectionLocked":true,"source":"jni-probe"})");
    }
    const auto probe = reinterpret_cast<vulkan_driver_axis_probe_v2_fn>(
        dlsym(probe_runtime, "executor_lsx4_runtime_probe_vulkan_driver_axis_v2"));
    if (!probe)
    {
        return to_jstring(
            env,
            R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"turnip","effective":"system","fallbackReason":"runtime-driver-probe-missing","selectionLocked":true,"source":"jni-probe"})");
    }
    const char* value = probe(
        1, native_dir.c_str(), turnip_dir.c_str(), report.c_str(), sha.c_str());
    return to_jstring(env, value ? value : "{}");
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_vulkanDriverStatus(JNIEnv* env, jclass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool ps5 = g_active_ps5_runtime.load(std::memory_order_acquire);
    str_fn status = ps5 ? g_ps5_vulkan_driver_status : g_vulkan_driver_status;
    if (status)
    {
        const char* value = status();
        g_vulkan_driver_bridge_status = value ? value : "{}";
    }
    return to_jstring(env, g_vulkan_driver_bridge_status);
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_ps5Status(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_ps5_status));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_initializePs5(
    JNIEnv* env, jclass, jstring root_dir, jstring user_id)
{
    if (!g_ps5_initialize)
    {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                      "PS5 runtime is not loaded.");
        return -1;
    }
    const char* raw_root = env->GetStringUTFChars(root_dir, nullptr);
    const char* raw_user = env->GetStringUTFChars(user_id, nullptr);
    const int result = g_ps5_initialize(
        raw_root ? raw_root : "", raw_user ? raw_user : "");
    if (raw_user)
    {
        env->ReleaseStringUTFChars(user_id, raw_user);
    }
    if (raw_root)
    {
        env->ReleaseStringUTFChars(root_dir, raw_root);
    }
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_attachPs5Surface(
    JNIEnv* env, jclass, jobject surface)
{
    if (!g_ps5_attach_surface)
    {
        return -2;
    }
    if (!surface)
    {
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),
                      "Surface is null.");
        return -1;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window)
    {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                      "Could not obtain ANativeWindow from PS5 Surface.");
        return -1;
    }
    const int result = g_ps5_attach_surface(window);
    ANativeWindow_release(window);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_detachPs5Surface(JNIEnv*, jclass)
{
    return g_ps5_detach_surface ? g_ps5_detach_surface() : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_scanPs5Game(
    JNIEnv* env, jclass, jstring path)
{
    if (!g_ps5_scan_game)
    {
        return -2;
    }
    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    const int result = g_ps5_scan_game(raw_path ? raw_path : "");
    if (raw_path)
    {
        env->ReleaseStringUTFChars(path, raw_path);
    }
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_launchPs5GameJit(
    JNIEnv* env, jclass, jstring path)
{
    if (!g_ps5_launch_game_jit)
    {
        return -2;
    }
    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    const int result = g_ps5_launch_game_jit(raw_path ? raw_path : "");
    if (raw_path)
    {
        env->ReleaseStringUTFChars(path, raw_path);
    }
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setPs5PadButton(
    JNIEnv*, jclass, jint button_mask, jboolean pressed)
{
    return g_ps5_set_pad_button
        ? g_ps5_set_pad_button(
              static_cast<unsigned int>(button_mask),
              pressed ? 1 : 0)
        : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setPs5PadAxis(
    JNIEnv*, jclass, jint axis, jint value)
{
    return g_ps5_set_pad_axis
        ? g_ps5_set_pad_axis(
              static_cast<int>(axis), static_cast<int>(value))
        : -2;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_abi(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_abi));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_version(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_version));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_systemInfo(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_system_info));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_firmwareStatus(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_firmware_status));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_initialize(JNIEnv* env, jclass, jstring root_dir, jstring user_id)
{
    if (!g_initialize)
    {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Runtime is not loaded.");
        return -1;
    }

    const char* raw_root = env->GetStringUTFChars(root_dir, nullptr);
    const char* raw_user = env->GetStringUTFChars(user_id, nullptr);
    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "[EXECUTOR_UI_INIT_MARKER] id=ui-bridge-live-cusa-marker-r1 "
                        "root=%s user=%s runtimeLoaded=%d",
                        raw_root ? raw_root : "", raw_user ? raw_user : "",
                        g_runtime ? 1 : 0);
    const int result = g_initialize(raw_root, raw_user);
    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "[EXECUTOR_UI_INIT_MARKER] id=ui-bridge-live-cusa-marker-r1 result=%d",
                        result);
    env->ReleaseStringUTFChars(root_dir, raw_root);
    env->ReleaseStringUTFChars(user_id, raw_user);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_installTranslatorHelper(JNIEnv* env, jclass, jstring source_dir)
{
    const char* raw_source = source_dir ? env->GetStringUTFChars(source_dir, nullptr) : nullptr;
    const std::string src = raw_source ? raw_source : "";
    if (raw_source)
    {
        env->ReleaseStringUTFChars(source_dir, raw_source);
    }
    if (!g_translator_install_helper)
    {
        return fb_install(src);
    }
    return g_translator_install_helper(src.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_translatorRunGuestElf(JNIEnv* env, jclass, jstring path)
{
    const char* raw_path = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
    const std::string elf = raw_path ? raw_path : "";
    if (raw_path)
    {
        env->ReleaseStringUTFChars(path, raw_path);
    }
    if (!g_translator_run_guest_elf)
    {
        return fb_run_guest(elf);
    }
    return g_translator_run_guest_elf(elf.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_elfBox64ServiceRun(
    JNIEnv* env, jclass, jstring elfPath, jstring translatorDir, jstring envBlob, jstring embedSoPath)
{
    auto jstr = [&](jstring s) -> std::string {
        if (!s) return {};
        const char* raw = env->GetStringUTFChars(s, nullptr);
        std::string out = raw ? raw : "";
        if (raw) env->ReleaseStringUTFChars(s, raw);
        return out;
    };
    const std::string elf = jstr(elfPath);
    const std::string translator = jstr(translatorDir);
    const std::string blob = jstr(envBlob);
    const std::string embed = jstr(embedSoPath);
    if (elf.empty() || embed.empty()) return -1;

    {
        std::size_t pos = 0;
        while (pos < blob.size()) {
            std::size_t nl = blob.find('\n', pos);
            if (nl == std::string::npos) nl = blob.size();
            std::string line = blob.substr(pos, nl - pos);
            pos = nl + 1;
            std::size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
        }
    }

    if (!translator.empty()) chdir(translator.c_str());

    if (!translator.empty()) {
        const std::string logp = translator + "/elfbox64-service.stderr.log";
        const int lf = open(logp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (lf >= 0) { dup2(lf, STDERR_FILENO); close(lf); }
    }

    void* h = dlopen(embed.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        __android_log_print(ANDROID_LOG_ERROR, "ElfBox64Service",
                            "dlopen(%s) failed: %s", embed.c_str(), dlerror());
        return -2;
    }
    using box64_run_linux_elf_fn = int (*)(int, const char**, char**);
    auto run = reinterpret_cast<box64_run_linux_elf_fn>(
        dlsym(h, "executor_box64_embed_run_linux_elf"));
    if (!run) {
        __android_log_print(ANDROID_LOG_ERROR, "ElfBox64Service",
                            "dlsym(executor_box64_embed_run_linux_elf) failed");
        return -3;
    }

    std::vector<std::string> lines;
    {
        std::size_t pos = 0;
        while (pos < blob.size()) {
            std::size_t nl = blob.find('\n', pos);
            if (nl == std::string::npos) nl = blob.size();
            if (nl > pos) lines.push_back(blob.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }
    std::vector<char*> envp;
    for (auto& l : lines) envp.push_back(l.data());
    envp.push_back(nullptr);

    const char* argv[3] = {embed.c_str(), elf.c_str(), nullptr};
    __android_log_print(ANDROID_LOG_INFO, "ElfBox64Service",
                        "run_linux_elf argv0=%s elf=%s translator=%s env=%zu", embed.c_str(),
                        elf.c_str(), translator.c_str(), lines.size());
    const int rc = run(2, argv, envp.data());
    __android_log_print(ANDROID_LOG_INFO, "ElfBox64Service", "run_linux_elf rc=%d", rc);
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_startServicePresentPump(JNIEnv* env, jclass, jstring translatorDir)
{
    if (!g_start_present_pump) {
        __android_log_print(ANDROID_LOG_ERROR, "ElfBox64Service",
                            "startServicePresentPump: g_start_present_pump NULL (runtime symbol unresolved)");
        return -1;
    }
    const char* raw = translatorDir ? env->GetStringUTFChars(translatorDir, nullptr) : nullptr;
    std::string dir = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(translatorDir, raw);
    if (dir.empty()) return -2;
    const std::string frame = dir + "/executor-videoout-frame.bin";
    const int rc = g_start_present_pump(frame.c_str(), 16);
    __android_log_print(ANDROID_LOG_INFO, "ElfBox64Service",
                        "startServicePresentPump rc=%d frame=%s", rc, frame.c_str());
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_stopServicePresentPump(JNIEnv*, jclass)
{
    if (!g_stop_present_pump) return -1;
    return g_stop_present_pump();
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_probeDlopen(JNIEnv* env, jclass, jstring path)
{
    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    if (!raw_path || raw_path[0] == '\0')
    {
        if (raw_path)
        {
            env->ReleaseStringUTFChars(path, raw_path);
        }
        return to_jstring(env, "dlopen probe skipped: empty path");
    }

    void* handle = dlopen(raw_path, RTLD_NOW | RTLD_LOCAL);
    std::string loaded_path = raw_path;
    env->ReleaseStringUTFChars(path, raw_path);

    if (!handle)
    {
        std::string error = "dlopen probe failed: ";
        const char* raw_error = dlerror();
        error += raw_error ? raw_error : "unknown";
        return to_jstring(env, error);
    }

    const char* symbols[] = {
        "executor_box64_embed_abi",
        "executor_box64_embed_version",
        "executor_box64_embed_mapped_entry_abi",
        "executor_box64_embed_run_linux_elf",
        "executor_box64_embed_validate_mapped_entry",
        "executor_box64_embed_run_mapped_entry",
        "executor_box64_embed_run_mapped_function",
        "main_loop",
        "box64_version_string",
        "GetBox64Version",
        "main"};
    std::string found;
    for (const char* symbol : symbols)
    {
        if (dlsym(handle, symbol))
        {
            if (!found.empty())
            {
                found += ",";
            }
            found += symbol;
        }
    }
    dlclose(handle);

    if (found.empty())
    {
        found = "none";
    }
    return to_jstring(env, "dlopen probe ok: " + loaded_path + " symbols=" + found);
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_loadEmbeddedBox64(JNIEnv* env, jclass, jstring path)
{
    if (!g_load_embedded_box64)
    {
        return -2;
    }

    const char* raw_path = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
    const int result = g_load_embedded_box64(raw_path ? raw_path : "");
    if (raw_path)
    {
        env->ReleaseStringUTFChars(path, raw_path);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_embeddedBox64Info(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_embedded_box64_info));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_validateEmbeddedBox64MappedEntry(JNIEnv*, jclass)
{
    if (!g_validate_embedded_box64_mapped_entry)
    {
        return -2;
    }
    return g_validate_embedded_box64_mapped_entry();
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_runEmbeddedBox64MappedEntry(JNIEnv*, jclass)
{
    if (!g_run_embedded_box64_mapped_entry)
    {
        return -2;
    }
    return g_run_embedded_box64_mapped_entry();
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_embeddedBox64MappedEntryInfo(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_embedded_box64_mapped_entry_info));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setPadButton(JNIEnv*, jclass, jint button_mask, jboolean pressed)
{
    const auto mask = static_cast<unsigned int>(button_mask);
    auto guest_mask = mask;

    if (g_system_ui_gamepad_button) {
        const auto consumed = g_system_ui_gamepad_button(mask, pressed ? 1 : 0);
        if (consumed != 0) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4",
                                "[EXECUTOR_SYSTEM_UI_GAMEPAD] mask=0x%08x pressed=%d "
                                "consumed=0x%08x",
                                mask, pressed ? 1 : 0, consumed);
        }
        guest_mask &= ~consumed;
        if (guest_mask == 0) {
            return 0;
        }
    }
    if (!g_set_pad_button)
    {
        return -2;
    }
    return g_set_pad_button(guest_mask, pressed ? 1 : 0);
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_sendSystemUiText(JNIEnv* env, jclass, jstring text)
{
    if (!g_system_ui_text_input)
    {
        __android_log_print(ANDROID_LOG_INFO, "LSX4",
                            "[EXECUTOR_SYSTEM_UI_TEXT] bytes=0 consumed=-2 available=0");
        return -2;
    }

    std::string utf8;
    if (!jstring_to_utf8(env, text, utf8))
    {
        __android_log_print(ANDROID_LOG_WARN, "LSX4",
                            "[EXECUTOR_SYSTEM_UI_TEXT] bytes=0 consumed=-3 invalid_input=1");
        return -3;
    }

    const int consumed = g_system_ui_text_input(utf8.c_str());
    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "[EXECUTOR_SYSTEM_UI_TEXT] bytes=%zu consumed=%d available=1",
                        utf8.size(), consumed);
    return consumed;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_isSystemUiTextActive(JNIEnv*, jclass)
{
    return g_system_ui_text_active ? g_system_ui_text_active() : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_submitSystemUiText(JNIEnv*, jclass)
{
    const int consumed = g_system_ui_text_submit ? g_system_ui_text_submit() : -2;
    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "[EXECUTOR_SYSTEM_UI_TEXT_SUBMIT] consumed=%d available=%d",
                        consumed, g_system_ui_text_submit ? 1 : 0);
    return consumed;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_backspaceSystemUiText(JNIEnv*, jclass, jint count)
{
    if (!g_system_ui_text_backspace || count <= 0)
    {
        return g_system_ui_text_backspace ? 0 : -2;
    }
    const int consumed = g_system_ui_text_backspace(static_cast<unsigned int>(count));
    __android_log_print(ANDROID_LOG_INFO, "LSX4",
                        "[EXECUTOR_SYSTEM_UI_TEXT_BACKSPACE] count=%d consumed=%d",
                        count, consumed);
    return consumed;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setPadAxis(JNIEnv*, jclass, jint axis, jint value)
{
    if (!g_set_pad_axis)
    {
        return -2;
    }
    return g_set_pad_axis(static_cast<int>(axis), static_cast<int>(value));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setTouchPad(JNIEnv*, jclass, jboolean pressed, jfloat x, jfloat y)
{
    if (!g_set_touchpad)
    {
        return -2;
    }
    return g_set_touchpad(pressed ? 1 : 0, static_cast<float>(x), static_cast<float>(y));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_audioInit(JNIEnv*, jclass)
{
    if (!g_audio_init)
    {
        return -2;
    }
    return g_audio_init();
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setAudioEnabled(JNIEnv*, jclass, jboolean enabled)
{
    return g_set_audio_enabled ? g_set_audio_enabled(enabled ? 1 : 0) : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setManagedOptimization(JNIEnv*, jclass, jint option,
                                                           jboolean enabled)
{
    option_fn setter = g_active_ps5_runtime.load(std::memory_order_acquire)
        ? g_ps5_set_managed_optimization
        : g_set_managed_optimization;
    return setter ? setter(static_cast<int>(option), enabled ? 1 : 0) : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_audioProbeTone(JNIEnv*, jclass)
{
    if (!g_audio_probe_tone)
    {
        return -2;
    }
    return g_audio_probe_tone();
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_audioStatus(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_audio_status));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_attachSurface(JNIEnv* env, jclass, jobject surface)
{
    if (!g_attach_surface)
    {
        return -2;
    }
    if (!surface)
    {
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), "Surface is null.");
        return -1;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window)
    {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Could not obtain ANativeWindow from Surface.");
        return -1;
    }

    const int result = g_attach_surface(window);
    ANativeWindow_release(window);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_detachSurface(JNIEnv*, jclass)
{
    if (!g_detach_surface)
    {
        return -2;
    }
    return g_detach_surface();
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_surfaceInfo(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_surface_info));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_presentTestPattern(JNIEnv*, jclass, jint width, jint height)
{
    if (!g_present_test_pattern)
    {
        return -2;
    }
    return g_present_test_pattern(static_cast<int>(width), static_cast<int>(height));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_presentHomebrewLoaderFrame(JNIEnv*, jclass, jint width, jint height)
{
    if (!g_present_homebrew_loader_frame)
    {
        return -2;
    }
    return g_present_homebrew_loader_frame(static_cast<int>(width), static_cast<int>(height));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_presentFrameDumpFile(JNIEnv* env, jclass, jstring path)
{
    if (!g_present_frame_dump)
    {
        return -2;
    }

    const char* raw_path = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
    const int result = g_present_frame_dump(raw_path ? raw_path : "");
    if (raw_path)
    {
        env->ReleaseStringUTFChars(path, raw_path);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_status(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_status));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_jitStatus(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_jit_status));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setRuntimePaused(JNIEnv*, jclass, jboolean paused)
{
    const bool ps5 = g_active_ps5_runtime.load(std::memory_order_acquire);
    int_fn setter = ps5 ? g_ps5_set_runtime_paused : g_set_runtime_paused;
    if (!setter)
    {
        __android_log_print(ANDROID_LOG_ERROR, "LSX4",
                            "[EXECUTOR_RUNTIME_LIFECYCLE] requested=%s platform=%s "
                            "result=unsupported",
                            paused ? "paused" : "running", ps5 ? "ps5" : "ps4");
        return -2;
    }
    const int result = setter(paused ? 1 : 0);
    __android_log_print(result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LSX4",
                        "[EXECUTOR_RUNTIME_LIFECYCLE] requested=%s platform=%s result=%d",
                        paused ? "paused" : "running", ps5 ? "ps5" : "ps4", result);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_runtimeLifecycleStatus(JNIEnv* env, jclass)
{
    const bool ps5 = g_active_ps5_runtime.load(std::memory_order_acquire);
    str_fn status = ps5 ? g_ps5_runtime_lifecycle_status : g_runtime_lifecycle_status;
    if (status)
    {
        return to_jstring(env, call_or_empty(status));
    }
    return to_jstring(env,
                      std::string{"{\"schema\":\"lsx4.runtime-lifecycle.v1\","
                                  "\"platform\":\""} +
                          (ps5 ? "ps5" : "ps4") +
                          "\",\"supported\":false,\"effective\":\"unknown\"}");
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_fexProofMetrics(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_fex_proof_metrics));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_runtimeProofCounters(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_runtime_proof_counters));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_gpuResidencyMetrics(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_gpu_residency_metrics));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_reportAndroidMemoryPressure(
    JNIEnv*, jclass, jint level)
{
    return g_report_android_memory_pressure
               ? g_report_android_memory_pressure(static_cast<int>(level))
               : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_reportAndroidMemoryInfo(
    JNIEnv*, jclass, jint level, jlong available_bytes,
    jlong threshold_bytes, jboolean low_memory)
{
    return g_report_android_memory_info
               ? g_report_android_memory_info(
                     static_cast<int>(level),
                     static_cast<unsigned long long>(available_bytes),
                     static_cast<unsigned long long>(threshold_bytes),
                     low_memory == JNI_TRUE ? 1 : 0)
               : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_reportAndroidProcessMemoryInfo(
    JNIEnv*, jclass, jlong total_pss_bytes, jlong graphics_bytes,
    jlong total_swap_bytes)
{
    return g_report_android_process_memory_info
               ? g_report_android_process_memory_info(
                     static_cast<unsigned long long>(total_pss_bytes),
                     static_cast<unsigned long long>(graphics_bytes),
                     static_cast<unsigned long long>(total_swap_bytes))
               : -2;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_androidMemoryPressureMonitorRequested(
    JNIEnv*, jclass)
{
    return g_android_memory_pressure_monitor_requested
               ? g_android_memory_pressure_monitor_requested()
               : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_setRenderScalePercent(JNIEnv*, jclass, jint percent)
{
    if (g_set_render_scale_percent == nullptr) {
        return -1;
    }
    return g_set_render_scale_percent(static_cast<int>(percent));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_app_lsx4_android_RuntimeBridge_runtimeHudStats(JNIEnv* env, jclass)
{
    constexpr std::size_t kValueCapacity = 166;
    std::uint64_t values[kValueCapacity]{};
    const auto hud_stats =
        g_active_ps5_runtime.load(std::memory_order_acquire)
            ? g_ps5_runtime_hud_stats
            : g_runtime_hud_stats;
    const int returned_count =
        hud_stats ? hud_stats(values, kValueCapacity) : 0;
    if (returned_count <= 0 || returned_count > static_cast<int>(kValueCapacity))
    {
        return env->NewLongArray(0);
    }
    const auto value_count = static_cast<std::size_t>(returned_count);
    jlong converted[kValueCapacity]{};
    for (std::size_t i = 0; i < value_count; ++i)
    {
        converted[i] = static_cast<jlong>(values[i]);
    }
    jlongArray result = env->NewLongArray(static_cast<jsize>(value_count));
    if (result)
    {
        env->SetLongArrayRegion(result, 0, static_cast<jsize>(value_count), converted);
    }
    return result;
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_app_lsx4_android_RuntimeBridge_hostPmuStats(JNIEnv* env, jclass)
{



    constexpr std::size_t kValueCount = 8;
    const auto snapshot = g_host_pmu_sampler.Sample();
    const jlong values[kValueCount]{
        1,
        snapshot.available ? 1 : 0,
        static_cast<jlong>(snapshot.error),
        static_cast<jlong>(snapshot.timestamp_ns),
        static_cast<jlong>(snapshot.values[0]),
        static_cast<jlong>(snapshot.values[1]),
        static_cast<jlong>(snapshot.values[2]),
        static_cast<jlong>(snapshot.values[3]),
    };
    jlongArray result = env->NewLongArray(static_cast<jsize>(kValueCount));
    if (result)
    {
        env->SetLongArrayRegion(result, 0, static_cast<jsize>(kValueCount), values);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_jitSelfTest(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_jit_selftest));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_vortekRingSelfTest(JNIEnv*, jclass)
{
    if (!g_vortek_ring_selftest)
    {
        return -2;
    }
    return g_vortek_ring_selftest();
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_relocateImports(JNIEnv*, jclass)
{
    if (!g_relocate_imports)
    {
        return -2;
    }
    return g_relocate_imports();
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_prepareBox64EntryTrampoline(JNIEnv*, jclass)
{
    if (!g_prepare_box64_entry_trampoline)
    {
        return -2;
    }
    return g_prepare_box64_entry_trampoline();
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_box64EntryRequest(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_box64_entry_request));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_scanGame(JNIEnv* env, jclass, jstring path)
{
    if (!g_scan_game)
    {
        return -2;
    }

    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    const int result = g_scan_game(raw_path);
    env->ReleaseStringUTFChars(path, raw_path);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_launchGame(JNIEnv* env, jclass, jstring path)
{
    if (!g_launch_game)
    {
        return -2;
    }

    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    const int result = g_launch_game(raw_path);
    env->ReleaseStringUTFChars(path, raw_path);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_launchGameJit(JNIEnv* env, jclass, jstring path)
{
    if (!g_launch_game_jit)
    {
        return -2;
    }

    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    const int result = g_launch_game_jit(raw_path);
    env->ReleaseStringUTFChars(path, raw_path);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_prepareTranslatorLaunch(JNIEnv* env, jclass, jstring path)
{
    if (!g_prepare_translator_launch)
    {
        return Java_app_lsx4_android_RuntimeBridge_launchGame(env, nullptr, path);
    }

    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    const int result = g_prepare_translator_launch(raw_path);
    env->ReleaseStringUTFChars(path, raw_path);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_reportTranslatorResult(JNIEnv*, jclass, jint helper_exit_code)
{
    if (!g_report_translator_result)
    {
        return -2;
    }
    return g_report_translator_result(static_cast<int>(helper_exit_code));
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_lsx4_android_RuntimeBridge_translatorLaunchRequest(JNIEnv* env, jclass)
{
    return to_jstring(env, call_or_empty(g_translator_launch_request));
}

extern "C" JNIEXPORT jint JNICALL
Java_app_lsx4_android_RuntimeBridge_reportTranslatorResultJson(JNIEnv* env, jclass, jstring result_json)
{
    if (!g_report_translator_result_json)
    {
        return -2;
    }

    if (!result_json)
    {
        return g_report_translator_result_json("");
    }

    const char* raw_json = env->GetStringUTFChars(result_json, nullptr);
    if (!raw_json)
    {
        return -1;
    }
    const int result = g_report_translator_result_json(raw_json ? raw_json : "");
    env->ReleaseStringUTFChars(result_json, raw_json);
    return result;
}
