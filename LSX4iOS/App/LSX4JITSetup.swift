// SPDX-License-Identifier: GPL-2.0-or-later
// Included only in the development JIT target, never the Store target.
import SwiftUI
import UniformTypeIdentifiers
import Darwin

enum LSX4JITInstallation {
    static var standalone: Bool {
        Bundle.main.object(forInfoDictionaryKey: "LSX4StandaloneDistribution") as? Bool == true
    }

    // CS_OPS_STATUS / CS_GET_TASK_ALLOW from Apple's XNU codesign contract.
    // Query our own process; no debug attachment or device connection is made.
    static var allowsDebugging: Bool? {
        typealias Query = @convention(c) (Int32, UInt32, UnsafeMutableRawPointer?, Int) -> Int32
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "csops") else { return nil }
        let query = unsafeBitCast(symbol, to: Query.self)
        var flags: UInt32 = 0
        guard query(getpid(), 0, &flags, MemoryLayout<UInt32>.size) == 0 else { return nil }
        return flags & 0x4 != 0
    }

    @MainActor static var problem: String? {
        guard #available(iOS 26.0, *) else { return L("jit.setup.os") }
        if getenv("LC_HOME_PATH") != nil { return L("jit.setup.container") }
        if allowsDebugging == false { return L("jit.setup.signature.error") }
        if LSX4BuiltInJIT.helperIdentifier == nil { return L("jit.setup.helper.error") }
        if (try? LSX4PairingStore.read()) == nil { return L("jit.setup.pairing.error") }
        return nil
    }

    @MainActor static func recordStatus() {
        let report: [String: Any] = [
            "bundleVersion": Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "",
            "allowsDebugging": allowsDebugging.map { $0 as Any } ?? NSNull(),
            "helperPresent": LSX4BuiltInJIT.helperIdentifier != nil,
            "pairingImported": (try? LSX4PairingStore.read()) != nil,
            "standalone": standalone, "requiresMacAtRuntime": false,
            "timestamp": Date().timeIntervalSince1970
        ]
        if let data = try? JSONSerialization.data(withJSONObject: report, options: .sortedKeys) {
            try? data.write(to: LSX4PairingStore.file.deletingLastPathComponent().deletingLastPathComponent()
                .appendingPathComponent("jit-installation-status.json"), options: .atomic)
        }
    }
}

struct LSX4JITSetupView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var busy = false
    @State private var revision = 0
    @State private var result: String?
    @State private var successful = false
    private var hasPairing: Bool { _ = revision; return (try? LSX4PairingStore.read()) != nil }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text(L("jit.setup.intro"))
                    Text(L("jit.setup.computer")).font(.footnote).foregroundStyle(.secondary)
                    Link(L("jit.setup.install.guide"), destination: URL(string: "https://sideloadly.io/")!)
                }
                Section(L("jit.setup.installation")) {
                    LabeledContent(L("jit.setup.signature"), value: LSX4JITInstallation.allowsDebugging == true ? L("jit.setup.ok") : L("jit.setup.check"))
                    LabeledContent(L("jit.setup.helper"), value: LSX4BuiltInJIT.helperIdentifier != nil ? L("jit.setup.ok") : L("jit.setup.missing"))
                    if let problem = LSX4JITInstallation.problem {
                        Text(problem).font(.footnote).foregroundStyle(.orange)
                    }
                }
                Section(L("jit.setup.vpn")) {
                    Text(L("jit.setup.vpn.instructions"))
                    Link(L("jit.setup.vpn.open"), destination: URL(string: "https://apps.apple.com/app/id6755608044")!)
                }
                Section(L("jit.setup.pairing")) {
                    LabeledContent(L("jit.setup.pairing.status"), value: hasPairing ? L("jit.setup.imported") : L("jit.setup.missing"))
                    Text(L("jit.setup.pairing.instructions")).font(.footnote)
                    Button(L(hasPairing ? "jit.setup.replace" : "jit.setup.import")) { importing = true }
                        .accessibilityIdentifier("jit-import-pairing").disabled(busy)
                    Text(L("jit.setup.private")).font(.footnote).foregroundStyle(.secondary)
                }
                Section {
                    Button {
                        guard !busy else { return }
                        if let problem = LSX4JITInstallation.problem { successful = false; result = problem; return }
                        guard let data = try? LSX4PairingStore.read() else { result = L("jit.setup.pairing.error"); return }
                        busy = true; result = nil; successful = false
                        LSX4BuiltInJIT.send(.prepare(pairingData: data)) { response in
                            busy = false
                            switch response {
                            case .success(let value):
                                successful = value.success
                                result = value.success ? L("jit.setup.ready") : L("jit.setup.connection.error") + "\n" + value.message
                            case .failure(let error):
                                successful = false
                                result = L("jit.setup.connection.error") + "\n" + error.localizedDescription
                            }
                        }
                    } label: {
                        HStack { if busy { ProgressView() }; Text(L(busy ? "jit.setup.checking" : "jit.setup.test")) }
                    }.disabled(busy).accessibilityIdentifier("jit-test-connection")
                    if let result { Text(result).foregroundStyle(successful ? .green : .orange).accessibilityIdentifier("jit-setup-result") }
                    Text(L("jit.setup.automatic")).font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle(L("jit.setup.title"))
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button(L("Готово")) { dismiss() }.disabled(busy) } }
            .interactiveDismissDisabled(busy)
            .fileImporter(isPresented: $importing, allowedContentTypes: [.item]) { selection in
                switch selection {
                case .success(let url):
                    let scoped = url.startAccessingSecurityScopedResource()
                    defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                    do {
                        try LSX4PairingStore.install(from: url)
                        LSX4JITInstallation.recordStatus()
                        revision += 1; successful = false; result = L("jit.setup.import.success")
                    } catch {
                        successful = false; result = L("jit.setup.import.error")
                    }
                case .failure(let error):
                    if (error as NSError).code != NSUserCancelledError { successful = false; result = L("jit.setup.import.error") }
                }
            }
        }.preferredColorScheme(.dark)
    }
}
