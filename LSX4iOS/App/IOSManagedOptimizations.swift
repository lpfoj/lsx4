// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Combine

struct IOSManagedOption: Codable, Identifiable {
    let key: String
    let labelKey: String
    let hintKey: String
    let label: String
    let defaultEnabled: Bool
    let nativeOption: Int?
    let available: Bool
    let category: String
    var id: String { key }
}
struct IOSManagedCatalogue: Codable { let options: [IOSManagedOption] }

@MainActor final class IOSManagedOptimizationStore: ObservableObject {
    let options: [IOSManagedOption]
    private let defaults: UserDefaults
    let title: String?
    private let titleDisabled: UInt32
    init(title: String? = nil, defaults: UserDefaults = .standard, data: Data? = nil) {
        self.title = title
        self.defaults = defaults
        let bytes = data ?? Bundle.main.url(forResource:"IOSManagedOptimizations",withExtension:"json")
            .flatMap { try? Data(contentsOf:$0) }
        options = bytes.flatMap { try? JSONDecoder().decode(IOSManagedCatalogue.self,from:$0).options } ?? []
        titleDisabled = Self.titleDisabledMask(title)
    }
    private static func titleDisabledMask(_ title: String?) -> UInt32 {
        guard let title,
              let url = Bundle.main.url(forResource:title,withExtension:"json",subdirectory:"CpuProfiles/LocalLaunch"),
              let data = try? Data(contentsOf:url),
              let root = try? JSONSerialization.jsonObject(with:data) as? [String:Any],
              let environment = root["environment"] as? [String:Any],
              let text = environment["LSX4_APPLE_MANAGED_DISABLE_MASK"] as? String,
              let mask = UInt32(text) else { return 0 }
        return mask
    }
    private var scope: String { title.map { "game.\($0)" } ?? "global" }
    private func key(_ name: String, scope: String? = nil) -> String {
        "lsx4.managed.v1.\(scope ?? self.scope).\(name)"
    }
    func checked(_ option: IOSManagedOption) -> Bool {
        if let value = defaults.object(forKey:key(option.key)) as? Bool { return value }
        if let id = option.nativeOption, (1...21).contains(id), titleDisabled & (1 << id) != 0 { return false }
        return defaults.object(forKey:key(option.key,scope:"global")) as? Bool ?? option.defaultEnabled
    }
    func hasOverride(_ option: IOSManagedOption) -> Bool { defaults.object(forKey:key(option.key)) != nil }
    func setChecked(_ value: Bool, option: IOSManagedOption) {
        guard option.available else { return }
        objectWillChange.send()
        defaults.set(value,forKey:key(option.key))
        if option.nativeOption == 5 && value, let tiered = options.first(where:{$0.nativeOption == 4}) {
            defaults.set(true,forKey:key(tiered.key))
        }
        if option.nativeOption == 4 && !value, let trace = options.first(where:{$0.nativeOption == 5}) {
            defaults.set(false,forKey:key(trace.key))
        }
    }
    var anisotropy: Int {
        get {
            let own = defaults.object(forKey:key("managed_anisotropy_mode")) as? Int
            let global = defaults.object(forKey:key("managed_anisotropy_mode",scope:"global")) as? Int
            return min(2,max(0,own ?? global ?? 0))
        }
        set {
            guard (0...2).contains(newValue) else { return }
            objectWillChange.send()
            defaults.set(newValue,forKey:key("managed_anisotropy_mode"))
        }
    }
    func reset() {
        objectWillChange.send()
        for option in options { defaults.removeObject(forKey:key(option.key)) }
        defaults.removeObject(forKey:key("managed_anisotropy_mode"))
    }
    func masks() -> (present: UInt32, enabled: UInt32) {
        var present: UInt32 = (1<<6) | (1<<12)
        var enabled: UInt32 = anisotropy == 1 ? 1<<12 : anisotropy == 2 ? 1<<6 : 0
        for option in options where option.available {
            guard let id = option.nativeOption, (1...21).contains(id) else { continue }
            present |= 1 << id
            if checked(option) { enabled |= 1 << id }
        }
        if enabled & (1<<5) != 0 { enabled |= 1<<4 }
        return (present,enabled)
    }
    func prepare(documents: URL = .documentsDirectory) throws {
        guard let title, title.count == 9, title.hasPrefix("CUSA"),
              title.dropFirst(4).utf8.allSatisfy({(48...57).contains($0)}), !options.isEmpty else {
            throw CocoaError(.fileReadCorruptFile)
        }
        let selected = masks()
        let receipt: [String:Any] = ["schema":1,"title":title,"presentMask":selected.present,
            "enabledMask":selected.enabled,"anisotropy":anisotropy,
            "requested":Dictionary(uniqueKeysWithValues:options.filter(\.available).map {($0.key,checked($0))}),
            "timestamp":Date().timeIntervalSince1970]
        try FileManager.default.createDirectory(at:documents,withIntermediateDirectories:true)
        try JSONSerialization.data(withJSONObject:receipt,options:[.sortedKeys]).write(
            to:documents.appendingPathComponent("managed-settings-status.json"),options:.atomic)
        setenv("LSX4_IOS_MANAGED_PRESENT_MASK",String(selected.present),1)
        setenv("LSX4_IOS_MANAGED_ENABLED_MASK",String(selected.enabled),1)
    }
}
