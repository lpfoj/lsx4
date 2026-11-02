// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Combine

struct IOSGamePatch: Codable, Identifiable {
    let id: String
    let preferenceKey: String
    let label: String
    let marker: String
    let activeWhenChecked: Bool
    let defaultEnabled: Bool
    let group: String
    let conflicts: [String]
    let note: String
    let author: String
    let version: String
    let category: String
    let xml: String
    func conflicts(with peer: IOSGamePatch) -> Bool {
        (!group.isEmpty && group == peer.group) || conflicts.contains(peer.id) || peer.conflicts.contains(id)
    }
}
struct IOSPatchGame: Codable, Identifiable {
    let id: String
    let name: String
    let titleIds: [String]
    let android: [IOSGamePatch]
    let community: [IOSGamePatch]
}
struct IOSPatchCatalogue: Codable { let games: [IOSPatchGame] }

@MainActor final class IOSGamePatchStore: ObservableObject {
    let games: [IOSPatchGame]
    private let defaults: UserDefaults
    init(defaults: UserDefaults = .standard, data: Data? = nil) {
        self.defaults = defaults
        let bytes = data ?? Bundle.main.url(forResource: "IOSGamePatches", withExtension: "json").flatMap { try? Data(contentsOf: $0) }
        games = bytes.flatMap { try? JSONDecoder().decode(IOSPatchCatalogue.self, from: $0).games } ?? []
    }
    func matchingGame(title: String, name: String? = nil) -> IOSPatchGame? {
        if let exact=games.first(where: { $0.titleIds.contains(title) }) { return exact }
        let normalized=(name ?? "").lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        if normalized.hasPrefix("bloodborne") || normalized.hasPrefix("ブラッドボーン") {
            return games.first(where: { $0.id=="CUSA00207" })
        }
        return nil
    }
    private func prefix(_ game: IOSPatchGame) -> String { "lsx4.gamePatches.v1.\(game.id)" }
    func source(_ game: IOSPatchGame) -> String { defaults.string(forKey: prefix(game)+".source") ?? "android" }
    func setSource(_ value: String, game: IOSPatchGame) {
        guard value == "android" || (value == "community" && !game.community.isEmpty) else { return }
        objectWillChange.send();defaults.set(value, forKey: prefix(game)+".source")
    }
    func enabled(_ game: IOSPatchGame) -> Bool {
        defaults.object(forKey: prefix(game)+".enabled") as? Bool ?? game.titleIds.contains("CUSA03173")
    }
    func setEnabled(_ value: Bool, game: IOSPatchGame) {
        objectWillChange.send();defaults.set(value, forKey: prefix(game)+".enabled")
    }
    func patches(_ game: IOSPatchGame) -> [IOSGamePatch] { source(game) == "community" ? game.community : game.android }
    private func key(_ patch: IOSGamePatch, _ game: IOSPatchGame) -> String { prefix(game)+".\(source(game)).\(patch.id)" }
    func checked(_ patch: IOSGamePatch, game: IOSPatchGame) -> Bool {
        defaults.object(forKey: key(patch, game)) as? Bool ?? patch.defaultEnabled
    }
    func setChecked(_ value: Bool, patch: IOSGamePatch, game: IOSPatchGame) {
        objectWillChange.send()
        if value {
            for peer in patches(game) where peer.id != patch.id && patch.conflicts(with: peer) {
                defaults.set(false, forKey: key(peer, game))
            }
        }
        defaults.set(value, forKey: key(patch, game))
    }
    func reset(_ game: IOSPatchGame) {
        objectWillChange.send()
        for patch in patches(game) { defaults.removeObject(forKey: key(patch, game)) }
    }
    func activePatches(_ game: IOSPatchGame) -> [IOSGamePatch] {
        var accepted: [IOSGamePatch] = []
        for patch in patches(game) where checked(patch, game: game) {
            if !accepted.contains(where: { patch.conflicts(with: $0) }) { accepted.append(patch) }
        }
        return accepted
    }
    private static func safeToken(_ token: String) -> Bool {
        (token.hasPrefix("run-") || token.hasPrefix("lsx4-")) && token.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [45,46,95].contains($0) }
    }
    private static let bloodborneEnvironment: [String: String] = [
        "MVK_EXPERIMENTAL_GEOMETRY": "1",
        "MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE": "8",
        "LSX4_APPLE_PRESERVE_BDA_IDENTITIES": "1",
        "LSX4_APPLE_PRESERVE_IMAGE_IDENTITIES": "1",
        "LSX4_APPLE_MEMORY_PRESSURE_RETIRE": "1",
        "LSX4_APPLE_BINDING_RETIREMENT_GUARD": "1",
        "LSX4_APPLE_LATE_BUFFER_BINDINGS": "1",
        "LSX4_APPLE_NATIVE_BALLOT_PREFIX": "1",
        "LSX4_APPLE_EXACT_DIRTY_UPLOAD": "1",
        "LSX4_APPLE_SNAPSHOT_RETRY_BUDGET": "1"
    ]
    private static let inheritedEnvironment: [String: String] = {
        var values: [String: String] = [:]
        for key in bloodborneEnvironment.keys {
            if let value = getenv(key) { values[key] = String(cString: value) }
        }
        return values
    }()
    private func prepareRuntime(title: String, name: String?) {
        let inherited = Self.inheritedEnvironment
        let bloodborne = matchingGame(title:title,name:name)?.id=="CUSA00207"
        for (key, value) in Self.bloodborneEnvironment {
            if let inheritedValue = inherited[key] {
                setenv(key, inheritedValue, 1)
            } else if bloodborne {
                setenv(key, value, 1)
            } else {
                unsetenv(key)
            }
        }
    }
    func prepare(title: String, name: String? = nil, documents: URL = .documentsDirectory) throws {
        prepareRuntime(title: title,name:name)
        let fm = FileManager.default
        let native = documents.appendingPathComponent("shared-runtime/native", isDirectory: true)
        try fm.createDirectory(at: native, withIntermediateDirectories: true)
        let ownership = native.appendingPathComponent("ios-patch-markers.json")
        let previous = (try? Data(contentsOf: ownership)).flatMap { try? JSONDecoder().decode([String].self, from: $0) } ?? []
        for token in previous where Self.safeToken(token) {
            let file = native.appendingPathComponent(token)
            if fm.fileExists(atPath: file.path) { try fm.removeItem(at: file) }
        }
        setenv("LSX4_IOS_PATCHES_ENABLED", "0", 1)
        unsetenv("LSX4_IOS_PATCH_TITLE");unsetenv("LSX4_IOS_XML_RESOLUTION")
        guard let game = matchingGame(title:title,name:name) else {
            try JSONEncoder().encode([String]()).write(to: ownership, options: .atomic)
            return
        }
        let gameEnabled = enabled(game)
        let required = (game.android + game.community).filter { $0.category == "Required" && !$0.xml.isEmpty }
        guard gameEnabled || !required.isEmpty else {
            try JSONEncoder().encode([String]()).write(to: ownership, options: .atomic)
            return
        }
        guard title.count == 9, title.hasPrefix("CUSA"), title.dropFirst(4).allSatisfy(\.isNumber) else {
            throw CocoaError(.fileWriteInvalidFileName)
        }
        var accepted = gameEnabled ? activePatches(game) : []
        for patch in required.reversed() where !accepted.contains(where: { $0.id == patch.id }) { accepted.insert(patch, at: 0) }
        var xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><Patch><TitleID><ID>\(title)</ID></TitleID>"
        for patch in accepted where !patch.xml.isEmpty {
            let raw = patch.xml.replacingOccurrences(of: #"\s+isEnabled\s*=\s*"[^"]*""#, with: "", options: .regularExpression)
            guard raw.hasPrefix("<Metadata ") else { throw CocoaError(.fileReadCorruptFile) }
            xml += raw.replacingOccurrences(of: "<Metadata ", with: "<Metadata isEnabled=\"true\" ", options: [], range: raw.startIndex..<raw.index(raw.startIndex, offsetBy: 10))
        }
        xml += "</Patch>"
        let directory = native.appendingPathComponent("patches/ios-selected", isDirectory: true)
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        try Data(xml.utf8).write(to: directory.appendingPathComponent(title+".xml"), options: .atomic)
        var markers: [String] = []
        for patch in patches(game) where gameEnabled && !patch.marker.isEmpty {
            let selected = checked(patch, game: game)
            let requested = patch.activeWhenChecked ? accepted.contains(where: { $0.id == patch.id }) : !selected
            guard requested, Self.safeToken(patch.marker) else { continue }
            let file = native.appendingPathComponent(patch.marker)
            if !fm.fileExists(atPath: file.path) {
                markers.append(patch.marker)
            }
        }
        try JSONEncoder().encode(markers).write(to: ownership, options: .atomic)
        for marker in markers {
            try Data("1\n".utf8).write(to: native.appendingPathComponent(marker), options: .atomic)
        }
        setenv("LSX4_IOS_PATCH_TITLE", title, 1)
        setenv("LSX4_IOS_XML_RESOLUTION", accepted.contains(where: { $0.category == "Resolution" }) ? "1" : "0", 1)
        setenv("LSX4_IOS_PATCHES_ENABLED", "1", 1)
        let receipt: [String: Any] = ["title": title, "source": source(game), "selected": accepted.map(\.id), "markers": markers, "timestamp": Date().timeIntervalSince1970]
        try JSONSerialization.data(withJSONObject: receipt, options: [.sortedKeys]).write(to: documents.appendingPathComponent("game-patch-status.json"), options: .atomic)
    }
}
