// SPDX-License-Identifier: GPL-2.0-or-later
// Development JIT target only. This file is excluded from the Store runtime.
import Foundation
import Darwin

@objc(LSX4LocalJITProvider)
public final class LSX4LocalJITProvider: NSObject {
    private static let lock = NSLock()
    private static var failure: String?

    @objc public static func lastFailure() -> String? {
        lock.lock(); defer { lock.unlock() }
        return failure
    }

    @MainActor @objc public static func begin(_ completion: @escaping (String?) -> Void) {
        lock.lock(); failure = nil; lock.unlock()
        let started = Date()
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        func receipt(_ error: String?, completed: Bool) {
            let state: [String:Any] = ["provider":"local", "requiresMacAtRuntime":false,
                "completed":completed,"success":completed && error == nil,"error":error ?? "",
                "timestamp":Date().timeIntervalSince1970,"seconds":Date().timeIntervalSince(started),
                "bundleVersion":Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? ""]
            if let data = try? JSONSerialization.data(withJSONObject:state,options:.prettyPrinted) {
                try? data.write(to:documents.appendingPathComponent("local-jit-status.json"),options:.atomic)
            }
        }
        receipt(nil,completed:false)
        func finish(_ error: String?) {
            lock.lock(); failure = error; lock.unlock()
            receipt(error,completed:true)
            completion(error)
        }
        guard #available(iOS 26.0, *) else {
            finish("JIT на устройстве требует iOS 26 или новее."); return
        }
        guard Bundle.main.object(forInfoDictionaryKey: "LSX4DeviceLocalJit") as? Bool == true else {
            finish("Эта сборка не содержит JIT-помощник."); return
        }
        if let problem = LSX4JITInstallation.problem { finish(problem); return }
        guard let pairing = try? LSX4PairingStore.read() else {
            finish(L("jit.setup.pairing.error")); return
        }
        guard let scriptURL = Bundle.main.url(forResource: "lsx4-local-jit", withExtension: "js"),
              let script = try? Data(contentsOf: scriptURL) else {
            finish("Скрипт JIT отсутствует в сборке."); return
        }
        let request = LSX4JITRequest.enable(targetPID: getpid(), pairingData: pairing,
                                          scriptBase64: script.base64EncodedString())
        LSX4BuiltInJIT.send(request) { result in
            switch result {
            case .success(let response): finish(response.success ? nil : response.message)
            case .failure(let error): finish(error.localizedDescription)
            }
        }
    }
}
