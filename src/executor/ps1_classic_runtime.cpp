#include "ps1_classic_runtime.h"
#include "ps1_classic_package.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <libretro.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Executor::Ps1Classic {
namespace {

using Clock = std::chrono::steady_clock;

void Log(int priority, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    __android_log_vprint(priority, "LSX4-PS1", format, arguments);
    va_end(arguments);
}

struct CoreApi {
    void* module{};
    void (*set_environment)(retro_environment_t){};
    void (*set_video_refresh)(retro_video_refresh_t){};
    void (*set_audio_sample)(retro_audio_sample_t){};
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t){};
    void (*set_input_poll)(retro_input_poll_t){};
    void (*set_input_state)(retro_input_state_t){};
    void (*init)(){};
    void (*deinit)(){};
    void (*get_system_info)(retro_system_info*){};
    void (*get_system_av_info)(retro_system_av_info*){};
    void (*set_controller_port_device)(unsigned, unsigned){};
    bool (*load_game)(const retro_game_info*){};
    void (*unload_game)(){};
    void (*run)(){};
    void* (*get_memory_data)(unsigned){};
    std::size_t (*get_memory_size)(unsigned){};
};

struct Runtime {
    std::mutex state_mutex;
    std::condition_variable state_changed;
    std::mutex window_mutex;
    std::thread thread;
    std::atomic<bool> active{false};
    std::atomic<bool> stop{false};
    bool paused{};
    std::atomic<std::uint32_t> buttons{0};
    std::array<std::atomic<int>, 4> axes{{128, 128, 128, 128}};
    std::atomic<std::uint64_t> presents{0};
    std::atomic<std::uint16_t> touch_buttons{0};
    ANativeWindow* window{};
    int surface_width{};
    int surface_height{};
    int configured_width{};
    int configured_height{};
    retro_pixel_format pixel_format{RETRO_PIXEL_FORMAT_0RGB1555};
    float display_aspect{4.0f / 3.0f};
    std::filesystem::path app0;
    std::filesystem::path image;
    std::filesystem::path system_dir;
    std::filesystem::path save_dir;
    std::filesystem::path save_file;
    std::string system_dir_string;
    std::string save_dir_string;
    std::map<std::string, std::string> variables;
    CoreApi api;
    AAudioStream* audio{};
    std::vector<std::int16_t> audio_ring;
    std::atomic<std::uint64_t> audio_read{0};
    std::atomic<std::uint64_t> audio_write{0};
    double frames_per_second{60.0};
};

Runtime g_runtime;

template <typename T>
bool LoadSymbol(void* module, const char* name, T& output) {
    output = reinterpret_cast<T>(dlsym(module, name));
    if (!output) {
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] missing symbol=%s error=%s", name, dlerror());
        return false;
    }
    return true;
}

bool LoadCore(CoreApi& api) {
    constexpr std::array names{
        "libswanstation_libretro_android.so",
        "swanstation_libretro_android.so",
    };
    for (const char* name : names) {
        api.module = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (api.module) {
            break;
        }
    }
    if (!api.module) {
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] core dlopen failed error=%s", dlerror());
        return false;
    }
    const bool loaded =
        LoadSymbol(api.module, "retro_set_environment", api.set_environment) &&
        LoadSymbol(api.module, "retro_set_video_refresh", api.set_video_refresh) &&
        LoadSymbol(api.module, "retro_set_audio_sample", api.set_audio_sample) &&
        LoadSymbol(api.module, "retro_set_audio_sample_batch", api.set_audio_sample_batch) &&
        LoadSymbol(api.module, "retro_set_input_poll", api.set_input_poll) &&
        LoadSymbol(api.module, "retro_set_input_state", api.set_input_state) &&
        LoadSymbol(api.module, "retro_init", api.init) &&
        LoadSymbol(api.module, "retro_deinit", api.deinit) &&
        LoadSymbol(api.module, "retro_get_system_info", api.get_system_info) &&
        LoadSymbol(api.module, "retro_get_system_av_info", api.get_system_av_info) &&
        LoadSymbol(api.module, "retro_set_controller_port_device",
                   api.set_controller_port_device) &&
        LoadSymbol(api.module, "retro_load_game", api.load_game) &&
        LoadSymbol(api.module, "retro_unload_game", api.unload_game) &&
        LoadSymbol(api.module, "retro_run", api.run) &&
        LoadSymbol(api.module, "retro_get_memory_data", api.get_memory_data) &&
        LoadSymbol(api.module, "retro_get_memory_size", api.get_memory_size);
    if (!loaded) {
        dlclose(api.module);
        api = {};
    }
    return loaded;
}

void CloseCore(CoreApi& api) {
    if (api.module) {
        dlclose(api.module);
    }
    api = {};
}

std::optional<std::filesystem::path> FindConfiguredImage(
    const std::filesystem::path& app0) {
    for (const auto& config : {
             app0 / "conf/PS1HD/package-ps4.conf",
             app0 / "conf/PS1HD/dev-ps4.conf",
             app0 / "config-title.txt"}) {
        std::ifstream stream(config);
        std::string line;
        while (std::getline(stream, line)) {
            const auto key = line.find("--image=");
            if (key == std::string::npos) {
                continue;
            }
            std::string value = line.substr(key + 8);
            value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
                value.pop_back();
            }
            const auto candidate = app0 / value;
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveImage(const std::filesystem::path& app0) {
    if (auto configured = FindConfiguredImage(app0)) {
        return configured;
    }
    struct Candidate {
        int score{};
        std::uintmax_t size{};
        std::filesystem::path path;
    };
    std::vector<Candidate> candidates;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(app0 / "data", error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) {
            continue;
        }
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (extension != ".cue" && extension != ".chd" && extension != ".pbp" &&
            extension != ".img" && extension != ".bin") {
            continue;
        }
        std::string name = it->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
        int score = extension == ".cue" ? 1000 : extension == ".chd" ? 900 : 100;
        if (name.find("SCEA") != std::string::npos || name.find("SCUS") != std::string::npos) {
            score += 500;
        }
        if (name.find("_EN") != std::string::npos || name.find("ENG") != std::string::npos) {
            score += 100;
        }
        candidates.push_back({score, it->file_size(error), it->path()});
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                       const Candidate& right) {
        return left.score != right.score ? left.score > right.score : left.size > right.size;
    });
    return candidates.front().path;
}

void InstallVariableDefaults(const retro_variable* variables) {
    if (!variables) {
        return;
    }
    for (const retro_variable* variable = variables; variable->key; ++variable) {
        if (g_runtime.variables.contains(variable->key) || !variable->value) {
            continue;
        }
        std::string definition = variable->value;
        const auto separator = definition.find(';');
        if (separator != std::string::npos) {
            definition.erase(0, separator + 1);
        }
        while (!definition.empty() && definition.front() == ' ') {
            definition.erase(definition.begin());
        }
        if (const auto alternatives = definition.find('|'); alternatives != std::string::npos) {
            definition.resize(alternatives);
        }
        g_runtime.variables.emplace(variable->key, std::move(definition));
    }
}

void CoreLog(enum retro_log_level level, const char* format, ...) {
    char message[2048]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    const int priority = level == RETRO_LOG_ERROR ? ANDROID_LOG_ERROR
                         : level == RETRO_LOG_WARN ? ANDROID_LOG_WARN
                                                   : ANDROID_LOG_INFO;
    Log(priority, "[SWANSTATION] %s", message);
}

bool Rumble(unsigned, enum retro_rumble_effect, std::uint16_t) {
    return true;
}

bool Environment(unsigned command, void* data) {
    switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = g_runtime.system_dir_string.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *static_cast<const char**>(data) = g_runtime.save_dir_string.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_runtime.pixel_format = *static_cast<const retro_pixel_format*>(data);
        return g_runtime.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ||
               g_runtime.pixel_format == RETRO_PIXEL_FORMAT_RGB565 ||
               g_runtime.pixel_format == RETRO_PIXEL_FORMAT_0RGB1555;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        InstallVariableDefaults(static_cast<const retro_variable*>(data));
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* variable = static_cast<retro_variable*>(data);
        const auto found = g_runtime.variables.find(variable->key ? variable->key : "");
        variable->value = found == g_runtime.variables.end() ? nullptr : found->second.c_str();
        return variable->value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = CoreLog;
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        static_cast<retro_rumble_interface*>(data)->set_rumble_state = Rumble;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto* message = static_cast<const retro_message*>(data);
        Log(ANDROID_LOG_INFO, "[PS1_DIRECT_MESSAGE] %s", message && message->msg ? message->msg : "");
        return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:
        g_runtime.stop.store(true, std::memory_order_release);
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    default:
        return false;
    }
}

void InputPoll() {}

std::uint16_t ReadButtons() {
    const std::uint32_t source = g_runtime.buttons.load(std::memory_order_acquire);
    std::uint16_t output = g_runtime.touch_buttons.load(std::memory_order_acquire);
    const auto set = [&](std::uint32_t mask, unsigned id) {
        if (source & mask) {
            output = static_cast<std::uint16_t>(output | (1u << id));
        }
    };
    set(0x4000, RETRO_DEVICE_ID_JOYPAD_B);      // Cross
    set(0x8000, RETRO_DEVICE_ID_JOYPAD_Y);      // Square
    set(0x0008, RETRO_DEVICE_ID_JOYPAD_START);  // Options
    set(0x0010, RETRO_DEVICE_ID_JOYPAD_UP);
    set(0x0040, RETRO_DEVICE_ID_JOYPAD_DOWN);
    set(0x0080, RETRO_DEVICE_ID_JOYPAD_LEFT);
    set(0x0020, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    set(0x2000, RETRO_DEVICE_ID_JOYPAD_A);      // Circle
    set(0x1000, RETRO_DEVICE_ID_JOYPAD_X);      // Triangle
    set(0x0400, RETRO_DEVICE_ID_JOYPAD_L);
    set(0x0800, RETRO_DEVICE_ID_JOYPAD_R);
    set(0x0100, RETRO_DEVICE_ID_JOYPAD_L2);
    set(0x0200, RETRO_DEVICE_ID_JOYPAD_R2);
    set(0x0002, RETRO_DEVICE_ID_JOYPAD_L3);
    set(0x0004, RETRO_DEVICE_ID_JOYPAD_R3);
    return output;
}

std::int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) {
        return 0;
    }
    if (device == RETRO_DEVICE_JOYPAD) {
        const std::uint16_t buttons = ReadButtons();
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            return static_cast<std::int16_t>(buttons);
        }
        return id < 16 && (buttons & (1u << id)) ? 1 : 0;
    }
    if (device == RETRO_DEVICE_ANALOG && index < 2 && id < 2) {
        const int axis = static_cast<int>(index * 2 + id);
        const int value = g_runtime.axes[axis].load(std::memory_order_acquire);
        return static_cast<std::int16_t>((value - 128) * 256);
    }
    return 0;
}

std::uint32_t ConvertPixel(const void* data, std::size_t index) {
    if (g_runtime.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        const std::uint32_t value = static_cast<const std::uint32_t*>(data)[index];
        return 0xff000000u | ((value & 0x00ff0000u) >> 16) | (value & 0x0000ff00u) |
               ((value & 0x000000ffu) << 16);
    }
    const std::uint16_t value = static_cast<const std::uint16_t*>(data)[index];
    const unsigned red = g_runtime.pixel_format == RETRO_PIXEL_FORMAT_RGB565
                             ? (value >> 11) & 31
                             : (value >> 10) & 31;
    const unsigned green = g_runtime.pixel_format == RETRO_PIXEL_FORMAT_RGB565
                               ? (value >> 5) & 63
                               : (value >> 5) & 31;
    const unsigned blue = value & 31;
    const unsigned r8 = (red * 255 + 15) / 31;
    const unsigned g8 = g_runtime.pixel_format == RETRO_PIXEL_FORMAT_RGB565
                            ? (green * 255 + 31) / 63
                            : (green * 255 + 15) / 31;
    const unsigned b8 = (blue * 255 + 15) / 31;
    return 0xff000000u | (b8 << 16) | (g8 << 8) | r8;
}

void VideoRefresh(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0) {
        return;
    }
    ANativeWindow* window = nullptr;
    int target_width = 0;
    int target_height = 0;
    {
        std::lock_guard lock(g_runtime.window_mutex);
        window = g_runtime.window;
        target_width = g_runtime.surface_width;
        target_height = g_runtime.surface_height;
        if (window) {
            ANativeWindow_acquire(window);
        }
    }
    if (!window || target_width <= 0 || target_height <= 0) {
        return;
    }
    if (g_runtime.configured_width != target_width ||
        g_runtime.configured_height != target_height) {
        ANativeWindow_setBuffersGeometry(window, target_width, target_height,
                                         WINDOW_FORMAT_RGBA_8888);
        g_runtime.configured_width = target_width;
        g_runtime.configured_height = target_height;
    }
    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(window, &buffer, nullptr) != 0) {
        ANativeWindow_release(window);
        return;
    }
    auto* destination = static_cast<std::uint32_t*>(buffer.bits);
    const int destination_width = std::min(buffer.width, target_width);
    const int destination_height = std::min(buffer.height, target_height);
    for (int y = 0; y < destination_height; ++y) {
        std::fill_n(destination + y * buffer.stride, destination_width, 0xff000000u);
    }
    const float aspect = g_runtime.display_aspect > 0.0f
                             ? g_runtime.display_aspect
                             : static_cast<float>(width) / static_cast<float>(height);
    int draw_width = destination_width;
    int draw_height = static_cast<int>(draw_width / aspect + 0.5f);
    if (draw_height > destination_height) {
        draw_height = destination_height;
        draw_width = static_cast<int>(draw_height * aspect + 0.5f);
    }
    const int offset_x = (destination_width - draw_width) / 2;
    const int offset_y = (destination_height - draw_height) / 2;
    const std::size_t bytes_per_pixel =
        g_runtime.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    const auto* source = static_cast<const std::uint8_t*>(data);
    for (int y = 0; y < draw_height; ++y) {
        const unsigned source_y = static_cast<unsigned>((static_cast<std::uint64_t>(y) * height) /
                                                        static_cast<unsigned>(draw_height));
        const void* source_row = source + source_y * pitch;
        auto* destination_row = destination + (offset_y + y) * buffer.stride + offset_x;
        for (int x = 0; x < draw_width; ++x) {
            const unsigned source_x = static_cast<unsigned>((static_cast<std::uint64_t>(x) * width) /
                                                            static_cast<unsigned>(draw_width));
            destination_row[x] = ConvertPixel(source_row, source_x * bytes_per_pixel /
                                                              bytes_per_pixel);
        }
    }
    ANativeWindow_unlockAndPost(window);
    ANativeWindow_release(window);
    g_runtime.presents.fetch_add(1, std::memory_order_relaxed);
}

std::size_t AudioBatch(const std::int16_t* data, std::size_t frames);

void AudioSample(std::int16_t left, std::int16_t right) {
    const std::array samples{left, right};
    AudioBatch(samples.data(), 1);
}

std::size_t AudioBatch(const std::int16_t* data, std::size_t frames) {
    if (!g_runtime.audio || !data || !frames || g_runtime.audio_ring.empty()) {
        return frames;
    }
    const std::uint64_t read = g_runtime.audio_read.load(std::memory_order_acquire);
    const std::uint64_t write = g_runtime.audio_write.load(std::memory_order_relaxed);
    const std::size_t samples = frames * 2;
    const std::size_t capacity = g_runtime.audio_ring.size();
    if (write - read + samples > capacity) {
        return frames;
    }
    for (std::size_t index = 0; index < samples; ++index) {
        g_runtime.audio_ring[(write + index) % capacity] = data[index];
    }
    g_runtime.audio_write.store(write + samples, std::memory_order_release);
    return frames;
}

aaudio_data_callback_result_t AudioCallback(AAudioStream*, void*, void* audio_data,
                                            std::int32_t frame_count) {
    auto* output = static_cast<std::int16_t*>(audio_data);
    const std::size_t requested = static_cast<std::size_t>(std::max(frame_count, 0)) * 2;
    const std::uint64_t read = g_runtime.audio_read.load(std::memory_order_relaxed);
    const std::uint64_t write = g_runtime.audio_write.load(std::memory_order_acquire);
    const std::size_t available = static_cast<std::size_t>(write - read);
    const std::size_t copied = std::min(requested, available);
    const std::size_t capacity = g_runtime.audio_ring.size();
    for (std::size_t index = 0; index < copied; ++index) {
        output[index] = g_runtime.audio_ring[(read + index) % capacity];
    }
    std::fill(output + copied, output + requested, 0);
    g_runtime.audio_read.store(read + copied, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool OpenAudio(double sample_rate) {
    std::lock_guard state_lock{g_runtime.state_mutex};
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] AAudio builder creation failed");
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder,
                                      std::max(8000, static_cast<int>(sample_rate + 0.5)));
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, AudioCallback, nullptr);
    const aaudio_result_t open_result = AAudioStreamBuilder_openStream(builder, &g_runtime.audio);
    AAudioStreamBuilder_delete(builder);
    if (open_result != AAUDIO_OK || !g_runtime.audio) {
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] AAudio open failed result=%d", open_result);
        g_runtime.audio = nullptr;
        return false;
    }
    g_runtime.audio_ring.assign(1u << 18, 0);
    g_runtime.audio_read.store(0, std::memory_order_release);
    g_runtime.audio_write.store(0, std::memory_order_release);
    const aaudio_result_t start_result = AAudioStream_requestStart(g_runtime.audio);
    Log(start_result == AAUDIO_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "[PS1_DIRECT] AAudio start result=%d rate=%d burst=%d", start_result,
        AAudioStream_getSampleRate(g_runtime.audio),
        AAudioStream_getFramesPerBurst(g_runtime.audio));
    if (start_result == AAUDIO_OK && g_runtime.paused) {
        (void)AAudioStream_requestPause(g_runtime.audio);
    }
    return start_result == AAUDIO_OK;
}

void CloseAudio() {
    std::lock_guard state_lock{g_runtime.state_mutex};
    if (g_runtime.audio) {
        AAudioStream_requestStop(g_runtime.audio);
        AAudioStream_close(g_runtime.audio);
        g_runtime.audio = nullptr;
    }
    g_runtime.audio_ring.clear();
}

void RestoreMemoryCard() {
    void* memory = g_runtime.api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const std::size_t size = g_runtime.api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    std::ifstream stream(g_runtime.save_file, std::ios::binary);
    if (memory && size && stream) {
        stream.read(static_cast<char*>(memory), static_cast<std::streamsize>(size));
        Log(ANDROID_LOG_INFO, "[PS1_DIRECT] restored memory card bytes=%zu path=%s", size,
            g_runtime.save_file.string().c_str());
    }
}

void SaveMemoryCard() {
    void* memory = g_runtime.api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const std::size_t size = g_runtime.api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!memory || !size) {
        return;
    }
    const auto temporary = g_runtime.save_file.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(static_cast<const char*>(memory), static_cast<std::streamsize>(size));
        if (!stream) {
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, g_runtime.save_file, error);
    if (error) {
        std::filesystem::remove(g_runtime.save_file, error);
        std::filesystem::rename(temporary, g_runtime.save_file, error);
    }
}

void Run() {
    CoreApi& api = g_runtime.api;
    if (!api.module && !LoadCore(api)) {
        g_runtime.active.store(false, std::memory_order_release);
        return;
    }
    g_runtime.variables = {
        {"swanstation_Console_Region", "Auto"},
        {"swanstation_BIOS_PathNTSCJ", "SCPH5500.bin"},
        {"swanstation_BIOS_PathNTSCU", "SCPH5501.bin"},
        {"swanstation_BIOS_PathPAL", "SCPH5502.bin"},
        {"swanstation_BIOS_PatchFastBoot", "true"},
        {"swanstation_CDROM_RegionCheck", "false"},
        {"swanstation_CDROM_ReadThread", "true"},
        {"swanstation_CPU_ExecutionMode", "Recompiler"},
        {"swanstation_GPU_Renderer", "Software"},
        {"swanstation_GPU_UseThread", "true"},
        {"swanstation_GPU_ResolutionScale", "1"},
        {"swanstation_Audio_FastHook", "true"},
        {"swanstation_MemoryCards_Card1Type", "Libretro"},
        {"swanstation_Controller1_AnalogDPadInDigitalMode", "true"},
    };
    api.set_environment(Environment);
    api.set_video_refresh(VideoRefresh);
    api.set_audio_sample(AudioSample);
    api.set_audio_sample_batch(AudioBatch);
    api.set_input_poll(InputPoll);
    api.set_input_state(InputState);
    api.init();
    retro_system_info system{};
    api.get_system_info(&system);
    Log(ANDROID_LOG_INFO, "[PS1_DIRECT] core=%s version=%s image=%s",
        system.library_name ? system.library_name : "unknown",
        system.library_version ? system.library_version : "unknown",
        g_runtime.image.string().c_str());
    const std::string image_string = g_runtime.image.string();
    retro_game_info game{image_string.c_str(), nullptr, 0, nullptr};
    if (!api.load_game(&game)) {
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] load_game failed image=%s", image_string.c_str());
        api.deinit();
        CloseCore(api);
        g_runtime.active.store(false, std::memory_order_release);
        return;
    }
    // Expose a DualShock rather than a digital-only pad.  The core keeps it
    // protocol-compatible with digital-only games, while the option above
    // maps both sticks to the D-pad until a game enables analog mode.
    api.set_controller_port_device(
        0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0));
    retro_system_av_info av{};
    api.get_system_av_info(&av);
    g_runtime.display_aspect = av.geometry.aspect_ratio > 0.0f
                                   ? av.geometry.aspect_ratio
                                   : 4.0f / 3.0f;
    g_runtime.frames_per_second = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
    OpenAudio(av.timing.sample_rate);
    RestoreMemoryCard();
    const auto frame_duration = std::chrono::duration<double>(1.0 / g_runtime.frames_per_second);
    auto deadline = Clock::now();
    auto metric_start = deadline;
    auto next_save = deadline + std::chrono::seconds(5);
    std::uint64_t metric_frames = 0;
    double execution_ms = 0.0;
    while (!g_runtime.stop.load(std::memory_order_acquire)) {
        bool resumed = false;
        {
            std::unique_lock lock{g_runtime.state_mutex};
            if (g_runtime.paused) {
                resumed = true;
                g_runtime.state_changed.wait(lock, [] {
                    return !g_runtime.paused ||
                           g_runtime.stop.load(std::memory_order_acquire);
                });
            }
        }
        if (g_runtime.stop.load(std::memory_order_acquire)) {
            break;
        }
        if (resumed) {
            // A stale pre-background deadline would cause a burst of catch-up
            // frames. Resume from a fresh pacing epoch instead.
            deadline = Clock::now();
            metric_start = deadline;
            metric_frames = 0;
            execution_ms = 0.0;
        }
        const auto begin = Clock::now();
        api.run();
        const auto end = Clock::now();
        execution_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        ++metric_frames;
        deadline += std::chrono::duration_cast<Clock::duration>(frame_duration);
        if (end < deadline) {
            std::this_thread::sleep_until(deadline);
        } else if (end - deadline > std::chrono::milliseconds(250)) {
            deadline = end;
        }
        const auto now = Clock::now();
        if (now >= next_save) {
            SaveMemoryCard();
            next_save = now + std::chrono::seconds(5);
        }
        if (now - metric_start >= std::chrono::seconds(2)) {
            const double elapsed = std::chrono::duration<double>(now - metric_start).count();
            Log(ANDROID_LOG_INFO,
                "[PS1_DIRECT_METRIC] fps=%.2f coreMs=%.2f budgetMs=%.2f presents=%llu",
                metric_frames / elapsed, execution_ms / std::max<std::uint64_t>(metric_frames, 1),
                1000.0 / g_runtime.frames_per_second,
                static_cast<unsigned long long>(g_runtime.presents.load()));
            metric_start = now;
            metric_frames = 0;
            execution_ms = 0.0;
        }
    }
    SaveMemoryCard();
    CloseAudio();
    api.unload_game();
    api.deinit();
    CloseCore(api);
    g_runtime.active.store(false, std::memory_order_release);
    Log(ANDROID_LOG_INFO, "[PS1_DIRECT] stopped");
}

} // namespace

bool IsPackage(const std::filesystem::path& executable) {
    return Detail::IsPackageLayout(executable);
}

int Launch(const std::filesystem::path& executable,
           const std::filesystem::path& runtime_root) {
    if (!IsPackage(executable)) {
        return -1;
    }
    Stop();
    const auto image = ResolveImage(executable.parent_path());
    if (!image) {
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] no supported disc image app0=%s",
            executable.parent_path().string().c_str());
        return -2;
    }
    if (!LoadCore(g_runtime.api)) {
        Log(ANDROID_LOG_ERROR,
            "[PS1_DIRECT] launch rejected: native SwanStation core is unavailable");
        return -3;
    }
    g_runtime.app0 = executable.parent_path();
    g_runtime.image = *image;
    g_runtime.system_dir = g_runtime.app0 / "assets/PS1HD/bios";
    g_runtime.save_dir = runtime_root / "ps1-classics" / g_runtime.app0.filename();
    std::error_code error;
    std::filesystem::create_directories(g_runtime.save_dir, error);
    g_runtime.save_file = g_runtime.save_dir / "card1.srm";
    g_runtime.system_dir_string = g_runtime.system_dir.string();
    g_runtime.save_dir_string = g_runtime.save_dir.string();
    g_runtime.stop.store(false, std::memory_order_release);
    g_runtime.presents.store(0, std::memory_order_release);
    g_runtime.buttons.store(0, std::memory_order_release);
    g_runtime.touch_buttons.store(0, std::memory_order_release);
    g_runtime.active.store(true, std::memory_order_release);
    Log(ANDROID_LOG_INFO, "[PS1_DIRECT] launch app0=%s image=%s save=%s",
        g_runtime.app0.string().c_str(), g_runtime.image.string().c_str(),
        g_runtime.save_dir.string().c_str());
    try {
        g_runtime.thread = std::thread(Run);
    } catch (const std::exception& error) {
        g_runtime.active.store(false, std::memory_order_release);
        CloseCore(g_runtime.api);
        Log(ANDROID_LOG_ERROR, "[PS1_DIRECT] thread start failed error=%s", error.what());
        return -4;
    }
    return 0;
}

void Stop() {
    g_runtime.stop.store(true, std::memory_order_release);
    g_runtime.state_changed.notify_all();
    if (g_runtime.thread.joinable()) {
        g_runtime.thread.join();
    }
    g_runtime.active.store(false, std::memory_order_release);
}

bool IsActive() {
    return g_runtime.active.load(std::memory_order_acquire);
}

void SetPaused(const bool paused) {
    std::lock_guard lock{g_runtime.state_mutex};
    if (g_runtime.paused == paused) {
        return;
    }
    g_runtime.paused = paused;
    if (g_runtime.audio != nullptr) {
        const aaudio_result_t result = paused
            ? AAudioStream_requestPause(g_runtime.audio)
            : AAudioStream_requestStart(g_runtime.audio);
        Log(result == AAUDIO_OK ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
            "[PS1_DIRECT] lifecycle_audio paused=%d result=%d", paused ? 1 : 0,
            result);
    }
    if (!paused) {
        g_runtime.state_changed.notify_all();
    }
}

bool IsPaused() {
    std::lock_guard lock{g_runtime.state_mutex};
    return g_runtime.paused;
}

void AttachSurface(void* native_window, int width, int height) {
    std::lock_guard lock(g_runtime.window_mutex);
    if (g_runtime.window) {
        ANativeWindow_release(g_runtime.window);
    }
    g_runtime.window = static_cast<ANativeWindow*>(native_window);
    if (g_runtime.window) {
        ANativeWindow_acquire(g_runtime.window);
    }
    g_runtime.surface_width = width;
    g_runtime.surface_height = height;
    g_runtime.configured_width = 0;
    g_runtime.configured_height = 0;
}

void SetButton(std::uint32_t orbis_mask, bool pressed) {
    if (pressed) {
        g_runtime.buttons.fetch_or(orbis_mask, std::memory_order_acq_rel);
    } else {
        g_runtime.buttons.fetch_and(~orbis_mask, std::memory_order_acq_rel);
    }
}

void SetAxis(int axis, int value) {
    if (axis >= 0 && axis < static_cast<int>(g_runtime.axes.size())) {
        g_runtime.axes[axis].store(std::clamp(value, 0, 255), std::memory_order_release);
    }
}

void SetTouchpad(bool pressed, float x, float) {
    const unsigned id = x < 0.5f ? RETRO_DEVICE_ID_JOYPAD_SELECT
                                 : RETRO_DEVICE_ID_JOYPAD_START;
    const std::uint16_t mask = static_cast<std::uint16_t>(1u << id);
    if (pressed) {
        g_runtime.touch_buttons.fetch_or(mask, std::memory_order_acq_rel);
    } else {
        g_runtime.touch_buttons.fetch_and(static_cast<std::uint16_t>(~mask),
                                          std::memory_order_acq_rel);
    }
}

std::uint64_t PresentCount() {
    return g_runtime.presents.load(std::memory_order_acquire);
}

} // namespace Executor::Ps1Classic
