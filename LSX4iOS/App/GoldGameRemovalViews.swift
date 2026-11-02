// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

enum GoldGameRemovalDialog: Identifiable {
    case hide, files, failure(String)
    var id: String {
        switch self { case .hide: return "hide"; case .files: return "files"; case .failure: return "failure" }
    }
}

struct GoldGameRemovalSection: View {
    let game: ConsoleTitle
    let allowed: Bool
    @Binding var busy: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var dialog: GoldGameRemovalDialog?

    var body: some View {
        Section {
            Button { dialog = .hide } label: {
                Label(P("game_remove_list", "Remove from list"), systemImage: "eye.slash")
            }.accessibilityIdentifier("game-remove-from-list")
            if game.folder != nil {
                Button(role: .destructive) { dialog = .files } label: {
                    Label(P("game_remove_files", "Delete game and files"), systemImage: "trash")
                }.accessibilityIdentifier("game-delete-with-files")
            }
            if busy { ProgressView(P("game_remove_working", "Removing game…")) }
        }.disabled(busy || !allowed)
            .alert(item: $dialog) { selection in
                switch selection {
                case .hide:
                    return Alert(title: Text(P("game_remove_list", "Remove from list")),
                                 message: Text(P("game_remove_list_warning", "Remove “{game}” from your library? Its files will be kept. You can restore it in Settings → Hidden games.", ["game": game.title])),
                                 primaryButton: .destructive(Text(P("game_remove_list", "Remove from list"))) { remove(files: false) },
                                 secondaryButton: .cancel(Text(L("Отмена"))))
                case .files:
                    return Alert(title: Text(P("game_remove_files", "Delete game and files")),
                                 message: Text(P("game_remove_files_warning", "Permanently delete the installed files for “{game}”? Other emulators using these files will lose access. Files outside the game folder are kept.", ["game": game.title])),
                                 primaryButton: .destructive(Text(P("game_remove_files", "Delete game and files"))) { remove(files: true) },
                                 secondaryButton: .cancel(Text(L("Отмена"))))
                case .failure(let message):
                    return Alert(title: Text(P("game_remove_error_title", "Could not remove game")), message: Text(message), dismissButton: .default(Text("OK")))
                }
            }
    }

    private func remove(files: Bool) {
        guard allowed, !busy else { return }
        if !files {
            do {
                try LSX4GameRemoval.removeFromList(game.id)
                dismiss()
            } catch { dialog = .failure(error.localizedDescription) }
            return
        }
        guard let folder = game.folder else {
            dialog = .failure(LSX4GameRemoval.Failure.missingGame.localizedDescription)
            return
        }
        busy = true
        let id = game.id
        Task { @MainActor in
            do {
                try await Task.detached(priority: .userInitiated) {
                    try LSX4GameRemoval.deleteInstalledFiles(id: id, expectedFolder: folder)
                }.value
                try LSX4GameRemoval.removeFromList(id)
                busy = false
                dismiss()
            } catch {
                busy = false
                dialog = .failure(error.localizedDescription)
            }
        }
    }
}

struct GoldHiddenGamesView: View {
    @StateObject private var model = ConsoleLibraryModel()
    @State private var hidden = Set<String>()
    var body: some View {
        Group {
            if hidden.isEmpty {
                ContentUnavailableView(P("game_hidden_empty", "No hidden games"), systemImage: "eye")
                    .accessibilityIdentifier("hidden-games-empty")
            } else {
                List {
                    ForEach(hidden.sorted(), id: \.self) { id in
                        let game = model.games.first { $0.id == id }
                        HStack(spacing: 12) {
                            if let game { GoldCover(game: game).frame(width: 44, height: 44).clipShape(RoundedRectangle(cornerRadius: 8)) }
                            VStack(alignment: .leading, spacing: 4) {
                                Text(game?.title ?? id).font(.headline)
                                Text(id).font(.caption).foregroundStyle(.secondary)
                            }.frame(maxWidth: .infinity, alignment: .leading)
                            Button(P("game_restore", "Restore")) {
                                LSX4GameRemoval.restoreToList(id)
                                hidden = LSX4GameRemoval.hiddenIDs()
                            }.buttonStyle(.bordered).accessibilityIdentifier("game-restore-" + id)
                        }.padding(.vertical, 4)
                    }
                }.scrollContentBackground(.hidden)
            }
        }.goldCommunityPage().navigationTitle(P("game_hidden_title", "Hidden games"))
            .onAppear { model.refresh(); hidden = LSX4GameRemoval.hiddenIDs() }
    }
}
