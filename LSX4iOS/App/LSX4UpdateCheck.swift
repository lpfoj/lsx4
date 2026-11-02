// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI
import UIKit

struct LSX4UpdateInfo: Codable, Equatable {
    let show: Bool
    let latestVersion: String
    var releasedAt: String? = nil
    var url: String? = nil
    var notes: [String: String]? = nil
}

struct LSX4UpdatesFeed: Codable {
    let version: Int
    var ios: LSX4UpdateInfo? = nil
    var android: LSX4UpdateInfo? = nil
    static func decode(_ data: Data) throws -> Self {
        guard data.count <= 64_000 else { throw URLError(.dataLengthExceedsMaximum) }
        let feed = try JSONDecoder().decode(Self.self, from: data)
        guard feed.version == 1 else { throw URLError(.cannotParseResponse) }
        return feed
    }
}

enum LSX4UpdatePolicy {
    static func components(_ value: String) -> [Int]? {
        let parts = value.split(separator: ".").map(String.init)
        guard !parts.isEmpty, parts.count <= 6 else { return nil }
        var result = [Int]()
        for part in parts {
            guard let number = Int(part), number >= 0 else { return nil }
            result.append(number)
        }
        return result
    }
    static func compare(_ lhs: [Int], _ rhs: [Int]) -> Int {
        for index in 0..<max(lhs.count, rhs.count) {
            let a = index < lhs.count ? lhs[index] : 0
            let b = index < rhs.count ? rhs[index] : 0
            if a != b { return a < b ? -1 : 1 }
        }
        return 0
    }
    static func date(_ value: String?) -> Date? {
        guard let value else { return nil }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: value) { return date }
        formatter.formatOptions = [.withInternetDateTime]
        if let date = formatter.date(from: value) { return date }
        formatter.formatOptions = [.withFullDate]
        return formatter.date(from: value)
    }
    static var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
    }
    static var buildDate: Date? {
        if let raw = Bundle.main.object(forInfoDictionaryKey: "LSX4BuildDate") as? String, let date = date(raw) { return date }
        guard let path = Bundle.main.executablePath,
              let attributes = try? FileManager.default.attributesOfItem(atPath: path) else { return nil }
        return attributes[.modificationDate] as? Date
    }
    static func isNewer(_ info: LSX4UpdateInfo, current: String = currentVersion, built: Date? = buildDate) -> Bool {
        guard info.show else { return false }
        if let latest = components(info.latestVersion), let installed = components(current) {
            let order = compare(installed, latest)
            if order != 0 { return order < 0 }
            return false
        }
        if info.latestVersion == current { return false }
        guard let released = date(info.releasedAt), let built else { return false }
        return released > built
    }
}

@MainActor final class LSX4UpdateChecker: ObservableObject {
    @Published var available: LSX4UpdateInfo?
    private let defaults: UserDefaults
    private let session: URLSession
    private let dismissedKey = "lsx4.update.dismissed.v1"
    private var checked = false
    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil
        config.urlCache = nil
        config.timeoutIntervalForRequest = 4
        config.timeoutIntervalForResource = 6
        session = URLSession(configuration: config)
    }
    func check() async {
        guard !checked else { return }
        checked = true
        var request = URLRequest(url: URL(string: "https://lsx45.app/api/updates")!, cachePolicy: .reloadIgnoringLocalCacheData)
        request.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
        guard let (data, response) = try? await session.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200,
              http.url?.host == "lsx45.app",
              let feed = try? LSX4UpdatesFeed.decode(data), let info = feed.ios,
              LSX4UpdatePolicy.isNewer(info) else { return }
        if defaults.string(forKey: dismissedKey) == info.latestVersion + "|" + (info.releasedAt ?? "") { return }
        available = info
    }
    func dismiss() {
        if let info = available { defaults.set(info.latestVersion + "|" + (info.releasedAt ?? ""), forKey: dismissedKey) }
        available = nil
    }
    var releaseURL: URL? {
        guard let raw = available?.url, let url = URL(string: raw), url.scheme == "https", url.host != nil else { return nil }
        return url
    }
    var message: String {
        guard let info = available else { return "" }
        var lines = [P("update_available_version", "Version {version}", ["version": info.latestVersion])]
        if let released = LSX4UpdatePolicy.date(info.releasedAt) {
            let formatter = DateFormatter()
            formatter.locale = Locale(identifier: LSX4AppText.language)
            formatter.dateStyle = .medium
            formatter.timeStyle = .none
            lines.append(P("update_available_date", "Released {date}", ["date": formatter.string(from: released)]))
        }
        lines.append(P("update_available_installed", "Installed: {version}", ["version": LSX4UpdatePolicy.currentVersion]))
        if let notes = info.notes, let text = notes[LSX4AppText.language] ?? notes["en"], !text.isEmpty {
            lines.append("")
            lines.append(text)
        }
        return lines.joined(separator: "\n")
    }
}

private struct LSX4UpdatePromptModifier: ViewModifier {
    @StateObject private var checker = LSX4UpdateChecker()
    @Environment(\.openURL) private var openURL
    func body(content: Content) -> some View {
        content
            .alert(P("update_available_title", "Update available"),
                   isPresented: Binding(get: { checker.available != nil }, set: { if !$0 { checker.dismiss() } })) {
                if let url = checker.releaseURL {
                    Button(P("update_open", "Open release")) { checker.dismiss(); openURL(url) }
                }
                Button(P("update_later", "Later"), role: .cancel) { checker.dismiss() }
            } message: {
                Text(verbatim: checker.message)
            }
            .task {
                try? await Task.sleep(for: .seconds(2))
                await checker.check()
            }
    }
}

extension View {
    func lsx4UpdatePrompt() -> some View { modifier(LSX4UpdatePromptModifier()) }
}
