// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct GoldGamePatchesView: View {
    @StateObject private var store = IOSGamePatchStore()
    var body: some View {
        List {
            Section { Text(G("patches_description", "Choose patches for each game. Changes apply on the next launch; conflicting patches switch off automatically.")).foregroundStyle(.secondary) }
            ForEach(store.games) { game in
                NavigationLink { GoldPatchGameView(store: store, game: game) } label: {
                    Label(game.name, systemImage: "wrench.and.screwdriver")
                }.accessibilityIdentifier("patch-game-"+game.id)
            }
        }.navigationTitle(G("patches_title", "Game patches"))
    }
}
private struct GoldPatchGameView: View {
    @ObservedObject var store: IOSGamePatchStore
    let game: IOSPatchGame
    @State private var search = ""
    private let categories = ["Rendering", "Resolution", "Frame rate", "Physics", "Other"]
    var body: some View {
        List {
            Section {
                Toggle(G("patches_enable", "Enable game patches"), isOn: Binding(get: { store.enabled(game) }, set: { store.setEnabled($0, game: game) }))
                    .accessibilityIdentifier("patches-enable")
                if !game.community.isEmpty {
                    Picker(G("patches_catalogue", "Catalogue"), selection: Binding(get: { store.source(game) }, set: { store.setSource($0, game: game) })) {
                        Text(G("patches_builtin", "Built-in")).tag("android")
                        Text(G("patches_community", "Community")).tag("community")
                    }.pickerStyle(.segmented).accessibilityIdentifier("patches-source")
                }
                Text(G("patches_version_note", "The emulator checks the game version before applying a patch. The number in a patch name is a target, not a measured FPS result.")).font(.footnote).foregroundStyle(.secondary)
            }
            ForEach(categories, id: \.self) { category in
                let patches = store.patches(game).filter { $0.category == category && (search.isEmpty || ($0.label+" "+$0.note).localizedCaseInsensitiveContains(search)) }
                if !patches.isEmpty {
                    Section(G("patches_category_"+category, category)) {
                        ForEach(patches) { patch in
                            Toggle(isOn: Binding(get: { store.checked(patch, game: game) }, set: { store.setChecked($0, patch: patch, game: game) })) {
                                VStack(alignment: .leading, spacing: 5) {
                                    Text(patch.label)
                                    Text("v\(patch.version) · \(patch.author)").font(.caption).foregroundStyle(.secondary)
                                    if !patch.note.isEmpty { Text(patch.note).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
                                }
                            }.disabled(!store.enabled(game)).accessibilityIdentifier("patch-"+patch.id)
                        }
                    }
                }
            }
            Section {
                Button(G("patches_reset", "Restore defaults")) { store.reset(game) }.accessibilityIdentifier("patches-reset")
                Link(G("patches_source", "Patch source and authors"), destination: URL(string: "https://github.com/shadps4-emu/ps4_cheats/blob/main/PATCHES/Bloodborne.xml")!)
            }
        }.searchable(text: $search).navigationTitle(game.name)
    }
}
