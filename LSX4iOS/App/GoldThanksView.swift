import Foundation
import SwiftUI

struct LSX4SupportersFeed: Codable {
    let version: Int
    let supporters: [String]
    var supporterLinks: [String: String]? = nil
    var links: [String: URL] {
        var result = [String: URL]()
        for name in supporters {
            guard let raw = supporterLinks?[name], raw.count <= 2048,
                  let url = URL(string: raw), url.scheme == "https", url.host != nil,
                  url.user == nil, url.password == nil else { continue }
            result[name] = url
        }
        return result
    }
    static func decode(_ data: Data) throws -> Self {
        guard data.count <= 256_000 else { throw URLError(.dataLengthExceedsMaximum) }
        let feed = try JSONDecoder().decode(Self.self, from: data)
        guard feed.version == 1, feed.supporters.count <= 1000, (feed.supporterLinks?.count ?? 0) <= 1000,
              Set(feed.supporters).count == feed.supporters.count,
              feed.supporters.allSatisfy({ !$0.isEmpty && $0.count <= 64 &&
                  $0 == $0.trimmingCharacters(in: .whitespacesAndNewlines) &&
                  $0.rangeOfCharacter(from: .controlCharacters) == nil }) else {
            throw URLError(.cannotParseResponse)
        }
        return feed
    }
}

@MainActor final class LSX4SupportersModel: ObservableObject {
    @Published private(set) var supporters: [String] = []
    @Published private(set) var links: [String: URL] = [:]
    @Published private(set) var loaded = false
    @Published private(set) var refreshing = false
    @Published private(set) var refreshFailed = false
    @Published private(set) var notes: [String: [String: String]] = [:]
    private let defaults: UserDefaults
    private let cacheKey = "lsx4.supporters.cache.v1"
    private let notesCacheKey = "lsx4.supporters.notes.cache.v1"
    private let session: URLSession
    init(defaults: UserDefaults = .standard, session: URLSession? = nil) {
        self.defaults = defaults
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil
        config.urlCache = nil
        config.timeoutIntervalForRequest = 3
        config.timeoutIntervalForResource = 4
        self.session = session ?? URLSession(configuration: config)
        if let data = defaults.data(forKey: cacheKey), let cached = try? LSX4SupportersFeed.decode(data) {
            supporters = cached.supporters
            links = cached.links
            loaded = true
        }
        if let data = defaults.data(forKey: notesCacheKey), let cached = Self.decodeNotes(data) { notes = cached }
    }
    static func decodeNotes(_ data: Data) -> [String: [String: String]]? {
        struct Feed: Codable { let version: Int; let notes: [String: [String: String]] }
        guard data.count <= 128_000, let feed = try? JSONDecoder().decode(Feed.self, from: data), feed.version == 1,
              feed.notes.count <= 1000,
              feed.notes.values.allSatisfy({ $0.values.allSatisfy { $0.count <= 160 } }) else { return nil }
        return feed.notes
    }
    func note(for username: String) -> String? {
        guard let values = notes[username] else { return nil }
        let text = values[LSX4AppText.language] ?? values["en"]
        return (text?.isEmpty ?? true) ? nil : text
    }
    private func refreshNotes() async {
        var request = URLRequest(url: URL(string: "https://lsx45.app/api/supporter-notes")!, cachePolicy: .reloadIgnoringLocalCacheData)
        request.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
        guard let (data, response) = try? await session.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200, http.url?.host == "lsx45.app",
              let decoded = Self.decodeNotes(data) else { return }
        notes = decoded
        defaults.set(data, forKey: notesCacheKey)
    }
    func refresh() async {
        guard !refreshing else { return }
        refreshing = true
        refreshFailed = false
        defer { refreshing = false }
        await refreshNotes()
        var request = URLRequest(url: URL(string: "https://lsx45.app/api/supporters")!, cachePolicy: .reloadIgnoringLocalCacheData)
        request.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
        do {
            let (data, response) = try await session.data(for: request)
            guard !Task.isCancelled else { return }
            guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                  http.url?.scheme == "https", http.url?.host == "lsx45.app",
                  http.url?.path == "/api/supporters" else { throw URLError(.badServerResponse) }
            let feed = try LSX4SupportersFeed.decode(data)
            supporters = feed.supporters
            links = feed.links
            loaded = true
            defaults.set(data, forKey: cacheKey)
        } catch {
            if !Task.isCancelled { refreshFailed = true }
        }
    }
}

struct GoldThanksView: View {
    @StateObject private var model = LSX4SupportersModel()
    var body: some View {
        GeometryReader { geometry in
        ScrollView {
        VStack(alignment: .leading, spacing: 16) {
            Text(.init(P("thanks_intro", "People who supported the project on [ko-fi.com/lsx](https://ko-fi.com/lsx), helped with testing, or contributed in other ways are listed here.")))
                .font(.body).frame(maxWidth: .infinity, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("thanks-intro")
            if model.loaded && !model.supporters.isEmpty {
                Group {
                    LazyVStack(alignment: .leading, spacing: 14) {
                        ForEach(model.supporters, id: \.self) { username in
                            GoldSupporterRow(username: username, url: model.links[username], note: model.note(for: username))
                        }
                    }.padding(.vertical, 8)
                }.accessibilityIdentifier("thanks-supporters")
            } else {
                VStack(alignment: .leading, spacing: 12) {
                    if model.loaded {
                        Text(P("thanks_empty", "Nobody yet")).font(.title3).foregroundStyle(.secondary)
                            .accessibilityIdentifier("thanks-empty")
                    }
                    if model.refreshing { ProgressView().accessibilityIdentifier("thanks-refreshing") }
                }.frame(maxWidth: .infinity, alignment: .leading)
            }
            if model.refreshFailed {
                VStack(spacing: 8) {
                    Text(P("thanks_refresh_failed", "Could not update the list. Please try again.")).font(.footnote).foregroundStyle(.secondary)
                    Button(P("thanks_retry", "Try again")) { Task { await model.refresh() } }
                }.accessibilityIdentifier("thanks-refresh-failed")
            }
            Spacer(minLength: 24)
            Text(P("thanks_android_note","The list of supporters is also included in the Android version of the emulator."))
                .font(.footnote).foregroundStyle(.secondary).frame(maxWidth:.infinity,alignment:.leading)
                .fixedSize(horizontal:false,vertical:true).accessibilityIdentifier("thanks-android-note")
            Text(P("thanks_crown_note","Supporters have a crown on their public profile and appear first in the user list."))
                .font(.footnote).foregroundStyle(.secondary).frame(maxWidth:.infinity,alignment:.leading)
                .fixedSize(horizontal:false,vertical:true).accessibilityIdentifier("thanks-crown-note")
        }.padding(20).frame(maxWidth: .infinity, minHeight: geometry.size.height, alignment: .top)
        }
        }.goldCommunityPage()
            .navigationTitle(P("thanks_title", "Thanks"))
            .task { await model.refresh() }
    }
}

private struct GoldSupporterRow: View {
    let username: String
    let url: URL?
    var note: String? = nil
    var body: some View {
        if let url {
            Link(destination: url) {
                HStack { label; Image(systemName: "arrow.up.right").font(.caption) }
            }.accessibilityIdentifier("thanks-supporter-" + username)
        } else { label }
    }
    private var label: some View {
        Label {
            if let note {
                Text(verbatim: username) + Text(verbatim: " (" + note + ")").foregroundColor(.secondary)
            } else {
                Text(verbatim: username)
            }
        } icon: { Image(systemName: "heart.fill").foregroundStyle(.pink) }
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}
