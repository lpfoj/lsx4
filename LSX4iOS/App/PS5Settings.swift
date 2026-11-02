import Foundation
import SwiftUI

enum PS5SettingsStore {
    static let prefix = "lsx4.ps5."
    static var activeTitle:String?
    static func gamePrefix(_ title:String)->String? {
        title.range(of:"^PPSA[0-9]{5}$",options:.regularExpression) != nil ? prefix+"game."+title+"." : nil
    }
    static func setting(_ key:String,title:String?=nil,defaults:UserDefaults = .standard)->Any? {
        if let title,let scoped=gamePrefix(title),let value=defaults.object(forKey:scoped+key) {return value}
        return defaults.object(forKey:prefix+key)
    }
    static func resetGame(_ title:String,defaults:UserDefaults = .standard) {
        guard let scope=gamePrefix(title) else {return}
        for key in defaults.dictionaryRepresentation().keys where key.hasPrefix(scope) {defaults.removeObject(forKey:key)}
    }
    static var root: URL {
        URL.documentsDirectory.appendingPathComponent("shared-runtime/ps5", isDirectory: true)
    }
    static func value(_ key: String, fallback: Int, title:String?=nil, defaults: UserDefaults = .standard) -> Int {
        (setting(key,title:title,defaults:defaults) as? NSNumber)?.intValue ?? fallback
    }
    static func enabled(_ key: String, fallback: Bool, title:String?=nil, defaults: UserDefaults = .standard) -> Bool {
        (setting(key,title:title,defaults:defaults) as? NSNumber)?.boolValue ?? fallback
    }
    static func cpuProfile(preset: Int, workers: Int) -> String {
        var fields: [String: Int] = [
            "preset": preset, "execution_mode": 0, "fast_memory": 1, "memory_manager": 1,
            "block_linking": 1, "persistent_cache": 1, "function_replacements": 1,
            "guest_clock_percent": 100, "cycle_skip": 0, "idle_loop_skip": 1,
            "wait_loop_detection": 1, "spin_loop_detection": 1, "max_guest_workers": 0,
            "fast_event_timing": 1, "jit_block_size": 1, "fp_accuracy": 0, "atomics": 1,
            "accurate_cache_line_stores": 0, "accurate_dma_timing": 0, "memory_exceptions": 1,
            "icache_accuracy": 1, "ignore_bad_accesses": 0, "jit_feature_mask": 0,
            "jit_disable_mask": 0, "cpu_gpu_dual_thread": 2, "scheduler": 3,
            "preferred_heavy_threads": 0, "subsystem_multithreading": 2, "separate_io_thread": 2,
            "separate_audio_hle_thread": 2, "guest_thread_delay_mode": 0, "guest_thread_delay_us": 0,
            "sync_on_idle_skip": 1, "cpu_gpu_sync": 1, "force_real_clock_sync": 0,
            "hle_mode": 1, "hle_fast_timers": 1, "per_game_profile": 2, "auto_game_db": 1
        ]
        let overrides: [String: Int]
        switch preset {
        case 0:
            overrides = ["execution_mode": 1, "fast_memory": 0, "memory_manager": 0,
                "block_linking": 0, "function_replacements": 0, "fast_event_timing": 0,
                "jit_block_size": 0, "atomics": 0, "accurate_cache_line_stores": 1,
                "accurate_dma_timing": 1, "cpu_gpu_dual_thread": 0, "scheduler": 0,
                "jit_feature_mask": 1, "subsystem_multithreading": 0, "separate_io_thread": 0,
                "separate_audio_hle_thread": 0, "cpu_gpu_sync": 0, "force_real_clock_sync": 1,
                "hle_mode": 0, "hle_fast_timers": 0]
        case 2:
            overrides = ["function_replacements": 2, "idle_loop_skip": 2,
                "wait_loop_detection": 2, "spin_loop_detection": 2, "guest_clock_percent": 85,
                "max_guest_workers": 4, "fast_event_timing": 2, "jit_block_size": 2,
                "fp_accuracy": 1, "atomics": 2, "memory_exceptions": 0, "icache_accuracy": 0,
                "scheduler": 2, "preferred_heavy_threads": 4, "cpu_gpu_sync": 2, "hle_fast_timers": 2]
        default: overrides = [:]
        }
        fields.merge(overrides) { _, new in new }
        if workers > 0 { fields["max_guest_workers"] = workers }
        return "# LSX4 PS5 CPU profile\n" + fields.keys.sorted().map { "\($0)=\(fields[$0]!)\n" }.joined()
    }
    static func prepare(at root: URL = PS5SettingsStore.root, defaults: UserDefaults = .standard) throws {
        let directory = root.appendingPathComponent("runtime-config", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let workers = min(32, max(0, value("cpu.workers", fallback: 0, title:activeTitle, defaults: defaults)))
        let preset = min(2, max(0, value("cpu.preset", fallback: 1, title:activeTitle, defaults: defaults)))
        let cpu = cpuProfile(preset: preset, workers: workers)
        try Data(cpu.utf8).write(to: directory.appendingPathComponent("android-cpu-tuning-v1.ini"), options: .atomic)
        let resolution = setting("resolution",title:activeTitle,defaults:defaults) as? String ?? "544p"
        let dimensions: (Int, Int)
        switch resolution {
        case "360p": dimensions = (640, 360)
        case "720p": dimensions = (1280, 720)
        case "1080p": dimensions = (1920, 1080)
        default: dimensions = (960, 544)
        }
        let configuration: [String: Any] = [
            "version": 1, "platform": "ps5", "outputWidth": dimensions.0,
            "outputHeight": dimensions.1, "cpuPreset": preset, "maxGuestWorkers": workers,
            "managedOptimizations": Dictionary(uniqueKeysWithValues: PS5ManagedOption.all.map {
                (String($0.id), enabled("optimization.\($0.id)", fallback: $0.defaultEnabled, title:activeTitle, defaults: defaults))
            })
        ]
        try JSONSerialization.data(withJSONObject: configuration, options: [.sortedKeys, .prettyPrinted])
            .write(to: directory.appendingPathComponent("ios-ps5-settings.json"), options: .atomic)
    }
    static func reset(defaults: UserDefaults = .standard) {
        for key in defaults.dictionaryRepresentation().keys where key.hasPrefix(prefix) && !key.hasPrefix(prefix+"game.") {
            defaults.removeObject(forKey: key)
        }
    }
}

struct PS5ManagedOption: Identifiable {
    let id: Int
    let key: String
    let title: String
    let defaultEnabled: Bool
    static let all: [Self] = [
        .init(id: 4, key: "tiered_jit", title: "Tiered JIT", defaultEnabled: true),
        .init(id: 5, key: "jit_trace_compilation", title: "JIT trace compilation", defaultEnabled: true),
        .init(id: 6, key: "limit_anisotropy", title: "Limit anisotropy to 2×", defaultEnabled: false),
        .init(id: 7, key: "readback_reuse", title: "Reuse texture readbacks", defaultEnabled: false),
        .init(id: 8, key: "driver_optimization", title: "Optimize graphics commands", defaultEnabled: false),
        .init(id: 9, key: "async_pipeline", title: "Asynchronous shader compilation", defaultEnabled: false),
        .init(id: 12, key: "force_anisotropy_1x", title: "Anisotropy 1×", defaultEnabled: false),
        .init(id: 13, key: "fast_guest_memory", title: "Fast stack memory", defaultEnabled: false),
        .init(id: 14, key: "relaxed_fp_fusion", title: "Floating-point fusion", defaultEnabled: false),
        .init(id: 16, key: "host_flagm", title: "Host flag instructions", defaultEnabled: false),
        .init(id: 18, key: "host_rcpc", title: "Host acquire instructions", defaultEnabled: false),
        .init(id: 19, key: "guest_poll_wait", title: "Event-driven guest waits", defaultEnabled: false)
    ]
}

@objc(LSX4PS5Settings) @MainActor
public final class LSX4PS5Settings: NSObject {
    @objc public static func selectTitle(_ title:String) {
        PS5SettingsStore.activeTitle=PS5SettingsStore.gamePrefix(title) == nil ? nil : title
    }
    @objc public static func prepareForGame() -> String? {
        do { try PS5SettingsStore.prepare(); return nil }
        catch { return error.localizedDescription }
    }
    @objc public static var runtimeRoot: String { PS5SettingsStore.root.path }
    @objc public static var outputWidth: Int {
        dimensions.0
    }
    @objc public static var outputHeight: Int {
        dimensions.1
    }
    private static var dimensions: (Int, Int) {
        switch PS5SettingsStore.setting("resolution",title:PS5SettingsStore.activeTitle) as? String ?? "544p" {
        case "360p": return (640, 360)
        case "720p": return (1280, 720)
        case "1080p": return (1920, 1080)
        default: return (960, 544)
        }
    }
}

struct PS5SettingsView: View {
    @AppStorage("lsx4.ps5.resolution") private var resolution = "544p"
    @AppStorage("lsx4.ps5.cpu.preset") private var preset = 1
    @AppStorage("lsx4.ps5.cpu.workers") private var workers = 0
    @State private var revision = 0
    var body: some View {
        Form {
            Section {
                Text(G("ps5_settings_independent", "These settings apply only to PS5 games."))
                    .foregroundStyle(.secondary)
            }
            Section(G("category_graphics", "Graphics")) {
                Picker(G("output_resolution", "Output resolution"), selection: $resolution) {
                    Text("640 × 360").tag("360p")
                    Text("960 × 544").tag("544p")
                    Text("1280 × 720").tag("720p")
                    Text("1920 × 1080").tag("1080p")
                }.accessibilityIdentifier("ps5-output-resolution")
            }
            Section("CPU") {
                Picker(G("cpu_preset", "CPU preset"), selection: $preset) {
                    Text(G("compatibility", "Compatibility")).tag(0)
                    Text(G("balanced", "Balanced")).tag(1)
                    Text(G("performance", "Performance")).tag(2)
                }.accessibilityIdentifier("ps5-cpu-preset")
                Picker(G("guest_workers", "Guest workers"), selection: $workers) {
                    Text(G("automatic", "Automatic")).tag(0)
                    ForEach(1...8, id: \.self) { Text(String($0)).tag($0) }
                }.accessibilityIdentifier("ps5-cpu-workers")
            }
            Section(G("category_managed_optimizations", "Managed optimizations")) {
                ForEach(PS5ManagedOption.all) { option in
                    PS5ManagedToggle(option: option)
                }
            }
            Section {
                Text(G("restart_game_to_apply", "Restart the game to apply changes."))
                    .foregroundStyle(.secondary)
                Button(G("reset_defaults", "Reset PS5 settings"), role: .destructive) {
                    PS5SettingsStore.reset(); resolution = "544p"; preset = 1; workers = 0; revision += 1
                }.accessibilityIdentifier("ps5-reset-settings")
            }
        }.id(revision).navigationTitle(G("ps5_preset_title", "PS5 Settings"))
    }
}

private struct PS5ManagedToggle: View {
    let option: PS5ManagedOption
    @AppStorage private var value: Bool
    init(option: PS5ManagedOption) {
        self.option = option
        _value = AppStorage(wrappedValue: option.defaultEnabled, "lsx4.ps5.optimization.\(option.id)")
    }
    var body: some View {
        Toggle(G(option.key, option.title), isOn: $value)
            .accessibilityIdentifier("ps5-optimization-\(option.id)")
    }
}
