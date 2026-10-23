// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The same pairing record is consumed by the existing helper. No device keys
/// are bundled with the app or included in its diagnostics.
enum LSX4PairingStore {
    static let maximumBytes = 1024 * 1024
    static var file: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT/pairingFile.plist")
    }
    enum Failure: Error { case invalid, tooLarge }

    static func validate(_ data: Data) throws {
        guard data.count <= maximumBytes else { throw Failure.tooLarge }
        guard !data.isEmpty,
              let record = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {throw Failure.invalid}
        // idevice/StikJIT remote pairing records use Ed25519/IRK keys rather
        // than the certificate fields of a classic lockdown pairing record.
        if let identifier=record["identifier"] as? String,!identifier.isEmpty,
           let publicKey=record["public_key"] as? Data,publicKey.count==32,
           let privateKey=record["private_key"] as? Data,privateKey.count==32,
           let irk=record["alt_irk"] as? Data,irk.count==16 {return}
        guard
              let host = record["HostID"] as? String, !host.isEmpty,
              let system = record["SystemBUID"] as? String, !system.isEmpty else { throw Failure.invalid }
        for key in ["HostCertificate", "RootCertificate", "HostPrivateKey", "DeviceCertificate"] {
            guard let value = record[key] as? Data, !value.isEmpty else { throw Failure.invalid }
        }
    }

    static func read(from url: URL = file) throws -> Data {
        let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        guard size <= maximumBytes else { throw Failure.tooLarge }
        let data = try Data(contentsOf: url)
        try validate(data)
        return data
    }

    static func install(from source: URL, to destination: URL = file) throws {
        // Validate before touching a working record; a cancelled/invalid import
        // cannot remove the user's previous pairing.
        let data = try read(from: source)
        try FileManager.default.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
#if os(iOS)
        try data.write(to: destination, options: [.atomic, .completeFileProtectionUnlessOpen])
#else
        try data.write(to: destination, options: .atomic)
#endif
        var protected = destination
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? protected.setResourceValues(values)
    }
}
