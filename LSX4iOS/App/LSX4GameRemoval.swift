// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum LSX4GameRemoval {
    static let visibilityKey = "library.hiddenGameIDs.v1"

    enum Failure: LocalizedError {
        case invalidFolder, missingGame, deleteFailed(String)
        var errorDescription: String? {
            switch self {
            case .invalidFolder:
                return P("game_remove_invalid_folder", "This folder cannot be deleted from the app. Use the Files app to manage it.")
            case .missingGame:
                return P("game_remove_missing", "The installed game folder could not be found. Refresh the library and try again.")
            case .deleteFailed(let detail):
                return P("game_remove_failed", "Could not delete the game files: {detail}", ["detail": detail])
            }
        }
    }

    static func hiddenIDs(defaults: UserDefaults = .standard) -> Set<String> {
        Set((defaults.stringArray(forKey: visibilityKey) ?? []).filter(LSX4PkgInstaller.validID))
    }

    static func removeFromList(_ id: String, defaults: UserDefaults = .standard) throws {
        guard LSX4PkgInstaller.validID(id) else { throw Failure.invalidFolder }
        var ids = hiddenIDs(defaults: defaults)
        ids.insert(id)
        defaults.set(ids.sorted(), forKey: visibilityKey)
    }

    static func restoreToList(_ id: String, defaults: UserDefaults = .standard) {
        var ids = hiddenIDs(defaults: defaults)
        ids.remove(id)
        defaults.set(ids.sorted(), forKey: visibilityKey)
    }

    static func deleteInstalledFiles(id: String, expectedFolder: URL,
                                     documents: URL = LSX4PkgInstaller.documents) throws {
        let fm = FileManager.default
        let documents = documents.standardizedFileURL
        let canonicalDocuments = documents.resolvingSymlinksInPath()
        guard LSX4PkgInstaller.validID(id) else { throw Failure.invalidFolder }
        let source = expectedFolder.standardizedFileURL
        let candidates = [documents.appendingPathComponent("Games", isDirectory: true).appendingPathComponent(id, isDirectory: true),
                          documents.appendingPathComponent(id, isDirectory: true)]
        guard let candidate = candidates.first(where: { $0.path == source.path }) else { throw Failure.invalidFolder }
        let relative = candidate.path.dropFirst(documents.path.count + 1)
        let canonicalCandidate = canonicalDocuments.appendingPathComponent(String(relative), isDirectory: true)
        guard source.resolvingSymlinksInPath().path == canonicalCandidate.path else { throw Failure.invalidFolder }
        guard fm.fileExists(atPath: source.path) else { throw Failure.missingGame }
        for folder in [source.deletingLastPathComponent(), source] {
            let values = try folder.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard values.isDirectory == true, values.isSymbolicLink != true else { throw Failure.invalidFolder }
        }
        guard let metadata = try? LSX4PkgInstaller.folderMetadata(source), metadata.id == id else { throw Failure.invalidFolder }

        let staging = documents.appendingPathComponent(".lsx4-removals", isDirectory: true)
        guard staging.resolvingSymlinksInPath().path == canonicalDocuments.appendingPathComponent(".lsx4-removals").path else {
            throw Failure.invalidFolder
        }
        try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        let values = try staging.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
        guard values.isDirectory == true, values.isSymbolicLink != true else { throw Failure.invalidFolder }
        let transaction = staging.appendingPathComponent(UUID().uuidString, isDirectory: true)
        do {
            try fm.moveItem(at: source, to: transaction)
            do {
                try fm.removeItem(at: transaction)
            } catch {
                if !fm.fileExists(atPath: source.path) { try? fm.moveItem(at: transaction, to: source) }
                throw error
            }
        } catch {
            throw Failure.deleteFailed(error.localizedDescription)
        }
    }
}
