// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Leviidev/Husk built-in JIT helper; original license retained.
import Foundation

/// Codable messages shared by LSX4 and its JIT helper extension. Compiled into
/// both targets. Each travels as JSON in an extension request's item
/// (userInfo[itemKey]), to the helper and back.
struct LSX4JITRequest: Codable, Sendable {
    static let itemKey = "lsx4-jit-request"

    enum Operation: String, Codable, Sendable {
        /// Check LocalDevVPN and download, mount and verify the Developer Disk Image.
        case prepare
        /// Attach to `targetPID` and run the JIT script until it detaches.
        case enable
        case resetDDI
    }

    let operation: Operation
    let targetPID: Int32?
    let pairingData: Data?
    let scriptBase64: String?

    static func prepare(pairingData: Data) -> LSX4JITRequest {
        LSX4JITRequest(operation: .prepare, targetPID: nil,
                       pairingData: pairingData, scriptBase64: nil)
    }

    static func enable(targetPID: Int32, pairingData: Data,
                       scriptBase64: String) -> LSX4JITRequest {
        LSX4JITRequest(operation: .enable, targetPID: targetPID,
                       pairingData: pairingData, scriptBase64: scriptBase64)
    }

    static var resetDDI: LSX4JITRequest {
        LSX4JITRequest(operation: .resetDDI, targetPID: nil,
                       pairingData: nil, scriptBase64: nil)
    }

    struct Response: Codable, Sendable {
        static let itemKey = "lsx4-jit-response"
        let success: Bool
        let message: String
        let txmPresent: Bool?
    }
}
