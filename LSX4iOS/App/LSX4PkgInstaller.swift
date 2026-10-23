// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

typealias LSX4PkgCallback = @convention(c) (UnsafeMutableRawPointer?, Int32, UInt64, UInt64, UInt64, UInt64) -> Int32
@_silgen_name("lsx4_pkg_extract_v1")
private func extractPackage(_ input: UnsafePointer<CChar>, _ output: UnsafePointer<CChar>, _ budget: UInt64,
                           _ callback: LSX4PkgCallback?, _ context: UnsafeMutableRawPointer?,
                           _ error: UnsafeMutablePointer<CChar>, _ capacity: Int) -> Int32

struct LSX4ImportedGame: Codable, Sendable {
    let id: String
    let title: String
    let bytes: Int64
    let fileCount: Int
    let installer: String
}

final class LSX4PkgProgress: @unchecked Sendable {
    struct Snapshot: Sendable { let phase: Int32; let bytes: UInt64; let total: UInt64; let files: UInt64; let fileTotal: UInt64 }
    private let lock = NSLock()
    private var cancelled = false
    private var committing = false
    private var lastUpdate = Date.distantPast
    private var emittedBytes = false
    var update: (@Sendable (Snapshot) -> Void)?
    func cancel() { lock.lock(); if !committing { cancelled = true }; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    func beginCommit() throws {
        lock.lock(); defer { lock.unlock() }
        if cancelled { throw LSX4PkgInstaller.Failure.cancelled }
        committing = true
    }
    func receive(_ phase: Int32, _ bytes: UInt64, _ total: UInt64, _ files: UInt64, _ fileTotal: UInt64) -> Int32 {
        lock.lock()
        let stop = cancelled
        let now = Date()
        let firstBytes = phase == 2 && bytes > 0 && !emittedBytes
        let emit = phase >= 0 && (phase != 2 || firstBytes || now.timeIntervalSince(lastUpdate) >= 0.1)
        if phase == 2 && bytes > 0 {emittedBytes = true}
        if emit { lastUpdate = now }
        lock.unlock()
        if emit { update?(.init(phase: phase, bytes: bytes, total: total, files: files, fileTotal: fileTotal)) }
        return stop ? 1 : 0
    }
}

/// File/transaction layer shared by the iOS UI and the executable host tests.
/// The extractor knows neither JIT nor library widgets; its C ABI is portable.
enum LSX4PkgInstaller {
    enum Failure: LocalizedError {
        case invalid(String), alreadyInstalled(String), storage, cancelled, extraction(String)
        var errorDescription: String? {
            switch self {
            case .invalid(let detail): return P("pkg_error_invalid","Invalid or unsupported PS4 PKG: {detail}",["detail":detail])
            case .alreadyInstalled(let id): return P("pkg_error_existing","{id} is already installed. Its files and saves have not been changed.",["id":id])
            case .storage: return P("pkg_error_storage","Not enough free storage to extract this PKG.")
            case .cancelled: return P("pkg_error_cancelled","Import cancelled.")
            case .extraction(let detail): return detail
            }
        }
    }
    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    private static let reserve: Int64 = 128 * 1024 * 1024

    static func validID(_ id: String) -> Bool {
        id.range(of: "^CUSA[0-9]{5}$", options: .regularExpression) != nil
    }
    private static func be(_ data: Data, _ offset: Int, _ count: Int) -> UInt64 {
        data[offset..<offset+count].reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
    }
    private static func header(_ source: URL) throws -> String {
        let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        guard values.isRegularFile == true, let length = values.fileSize, length >= 0x1000 else { throw Failure.invalid("file is too small") }
        let handle = try FileHandle(forReadingFrom: source); defer { try? handle.close() }
        let data = try handle.read(upToCount: 0x1000) ?? Data()
        guard data.count == 0x1000, Array(data.prefix(4)) == [0x7f, 0x43, 0x4e, 0x54] else { throw Failure.invalid("not a PS4 package") }
        let count = be(data,0x10,4), table = be(data,0x18,4), pfs = be(data,0x410,8), size = be(data,0x418,8)
        let fileSize = UInt64(length), declared = be(data,0x430,8)
        guard count > 0, count <= 8192, table >= 0x1000, table <= fileSize, count*32 <= fileSize-table,
              pfs <= fileSize, size > 0, size <= fileSize-pfs, declared <= fileSize else { throw Failure.invalid("truncated package or invalid file table") }
        let id = String(data: data[0x47..<0x50], encoding: .utf8) ?? ""
        guard validID(id) else { throw Failure.invalid("only PS4 CUSA packages are supported") }
        return id
    }
    private static func sfo(_ data: Data) throws -> [String:String] {
        guard data.count >= 20, data.count <= 2*1024*1024, Array(data.prefix(4)) == [0,0x50,0x53,0x46] else { throw Failure.invalid("missing or invalid param.sfo") }
        func le(_ p: Int, _ n: Int) -> Int {
            data[p..<p+n].enumerated().reduce(0) { $0 | Int($1.element) << ($1.offset*8) }
        }
        let keyTable=le(8,4), valueTable=le(12,4), count=le(16,4)
        guard count <= 4096, count*16 <= data.count-20, keyTable >= 20+count*16,
              keyTable <= data.count, valueTable >= keyTable, valueTable <= data.count else { throw Failure.invalid("invalid SFO table") }
        var result: [String:String] = [:]
        for i in 0..<count {
            let p=20+i*16, key=keyTable+le(p,2), length=le(p+4,4), value=valueTable+le(p+12,4)
            guard key >= keyTable, key < valueTable, value >= valueTable, value <= data.count,
                  length <= data.count-value, let end=data[key..<valueTable].firstIndex(of: 0), end-key <= 128 else { throw Failure.invalid("SFO value is out of range") }
            guard let name=String(data:data[key..<end],encoding:.utf8) else { continue }
            if name == "TITLE_ID" || name == "TITLE" || name == "CATEGORY" {
                guard let value=String(data:data[value..<value+length].prefix { $0 != 0 },encoding:.utf8) else { throw Failure.invalid("invalid SFO text") }
                result[name]=value.trimmingCharacters(in:.whitespacesAndNewlines)
            }
        }
        return result
    }
    private static func regular(_ file: URL, within root: URL) -> Bool {
        let prefix=root.standardizedFileURL.path+"/"
        guard file.standardizedFileURL.path.hasPrefix(prefix), file.resolvingSymlinksInPath().path.hasPrefix(root.resolvingSymlinksInPath().path+"/"),
              let values=try? file.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey]) else { return false }
        return values.isRegularFile == true && values.isSymbolicLink != true
    }

    static func folderMetadata(_ folder: URL) throws -> (id:String,title:String) {
        let executable=folder.appendingPathComponent("eboot.bin"),param=folder.appendingPathComponent("sce_sys/param.sfo")
        guard regular(executable,within:folder),regular(param,within:folder),
              let size=try param.resourceValues(forKeys:[.fileSizeKey]).fileSize,size<=2*1024*1024 else {
            throw Failure.invalid("choose the game folder containing eboot.bin and sce_sys/param.sfo")
        }
        let handle=try FileHandle(forReadingFrom:executable);defer {try? handle.close()}
        let magic=Array(try handle.read(upToCount:4) ?? Data())
        guard magic == [0x7f,0x45,0x4c,0x46] || magic == [0x4f,0x15,0x3d,0x1d] else {throw Failure.invalid("eboot.bin is not a PS4 executable")}
        let fields=try sfo(Data(contentsOf:param))
        // A complete extracted installation may already have an update merged
        // into it; its param.sfo then legitimately retains CATEGORY=gp.
        // Standalone update PKGs remain rejected by install(source:).
        guard let id=fields["TITLE_ID"],validID(id),fields["CATEGORY"] != "ac" else {
            throw Failure.invalid("a PS4 base game with a valid CUSA TITLE_ID is required")
        }
        return (id,String((fields["TITLE"] ?? id).prefix(512)))
    }

    static func existingGameFolder(id:String,documents:URL=documents) -> URL? {
        guard validID(id) else {return nil}
        for root in [documents.appendingPathComponent("Games",isDirectory:true),documents] {
            let folder=root.appendingPathComponent(id,isDirectory:true)
            guard (try? folder.resourceValues(forKeys:[.isSymbolicLinkKey]).isSymbolicLink) != true,
                  let metadata=try? folderMetadata(folder),metadata.id==id else {continue}
            return folder
        }
        return nil
    }

    /// Copies an already-extracted folder into the same atomic installation
    /// layout as PKG imports. Its display name need not be a CUSA title ID.
    static func installFolder(source:URL,documents requestedDocuments:URL=documents,
                              progress:LSX4PkgProgress = .init(),storageBudget:UInt64?=nil) throws -> LSX4ImportedGame {
        let fm=FileManager.default,documents=requestedDocuments.resolvingSymlinksInPath().standardizedFileURL
        let source=source.resolvingSymlinksInPath().standardizedFileURL
        guard source != documents,source != documents.appendingPathComponent("Games") else {
            throw Failure.invalid("select a folder containing only one game, not the LSX4 library folder")
        }
        let metadata=try folderMetadata(source)
        let games=documents.appendingPathComponent("Games",isDirectory:true),destination=games.appendingPathComponent(metadata.id,isDirectory:true)
        guard !fm.fileExists(atPath:destination.path),!fm.fileExists(atPath:documents.appendingPathComponent(metadata.id).path) else {throw Failure.alreadyInstalled(metadata.id)}
        guard !documents.path.hasPrefix(source.path+"/") else {throw Failure.invalid("the game folder cannot contain the destination library")}
        let available=(try documents.resourceValues(forKeys:[.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
        let budget=storageBudget ?? UInt64(max(0,available-reserve))
        let keys:Set<URLResourceKey>=[.isRegularFileKey,.isDirectoryKey,.isSymbolicLinkKey,.fileSizeKey]
        var enumerationError:Error?
        guard let enumerator=fm.enumerator(at:source,includingPropertiesForKeys:Array(keys),options:[],errorHandler:{_,error in enumerationError=error;return false}) else {throw Failure.invalid("cannot read game folder")}
        var files:[(URL,String,UInt64)]=[],directories:[String]=[],total:UInt64=0,nodes=0
        for case let file as URL in enumerator {
            if progress.isCancelled {throw Failure.cancelled}
            nodes+=1
            let values=try file.resourceValues(forKeys:keys),path=file.standardizedFileURL.path
            guard nodes<=1_000_000,path.hasPrefix(source.path+"/"),values.isSymbolicLink != true else {throw Failure.invalid("unsupported link or game folder tree")}
            let relative=String(path.dropFirst(source.path.count+1))
            guard relative.split(separator:"/").count<=128 else {throw Failure.invalid("game folder is too deep")}
            if relative==".lsx4-complete.json" {continue}
            if values.isDirectory == true {directories.append(relative)}
            else if values.isRegularFile == true {
                let size=UInt64(max(0,values.fileSize ?? 0))
                guard size<=budget,total<=budget-size else {throw Failure.storage}
                total+=size;files.append((file,relative,size))
            } else {throw Failure.invalid("unsupported special file in game folder")}
        }
        if let enumerationError {throw enumerationError}
        let staging=documents.appendingPathComponent(".pkg-import-staging",isDirectory:true)
        try fm.createDirectory(at:staging,withIntermediateDirectories:true)
        guard staging.resolvingSymlinksInPath().path==staging.path else {throw Failure.invalid("invalid import directory")}
        let transaction=staging.appendingPathComponent(UUID().uuidString,isDirectory:true),output=transaction.appendingPathComponent("game",isDirectory:true)
        try fm.createDirectory(at:output,withIntermediateDirectories:true)
        defer {try? fm.removeItem(at:transaction)}
        for relative in directories {try fm.createDirectory(at:output.appendingPathComponent(relative),withIntermediateDirectories:true)}
        var written:UInt64=0,completed:UInt64=0
        _=progress.receive(1,0,total,0,UInt64(files.count))
        for (file,relative,expected) in files {
            if progress.isCancelled {throw Failure.cancelled}
            guard regular(file,within:source) else {throw Failure.invalid("game folder changed during import")}
            let target=output.appendingPathComponent(relative)
            try fm.createDirectory(at:target.deletingLastPathComponent(),withIntermediateDirectories:true)
            guard fm.createFile(atPath:target.path,contents:nil) else {throw Failure.invalid("cannot create imported file")}
            let input=try FileHandle(forReadingFrom:file),dest=try FileHandle(forWritingTo:target)
            do {
                defer {try? input.close();try? dest.close()}
                var copied:UInt64=0
                while let chunk=try input.read(upToCount:1024*1024),!chunk.isEmpty {
                    if progress.isCancelled {throw Failure.cancelled}
                    guard UInt64(chunk.count)<=expected-copied else {throw Failure.invalid("game file changed during import")}
                    try dest.write(contentsOf:chunk);copied+=UInt64(chunk.count);written+=UInt64(chunk.count)
                    _=progress.receive(2,written,total,completed,UInt64(files.count))
                }
                guard copied==expected else {throw Failure.invalid("game file changed during import")}
            }
            completed+=1
        }
        if progress.isCancelled {throw Failure.cancelled}
        _=progress.receive(3,written,total,completed,UInt64(files.count))
        let copiedMetadata=try folderMetadata(output)
        guard copiedMetadata.id==metadata.id else {throw Failure.invalid("game metadata changed during import")}
        let result=LSX4ImportedGame(id:metadata.id,title:metadata.title,bytes:Int64(clamping:total),fileCount:files.count,installer:"folder-v1")
        try JSONEncoder().encode(result).write(to:output.appendingPathComponent(".lsx4-complete.json"),options:.atomic)
        try fm.createDirectory(at:games,withIntermediateDirectories:true)
        guard games.resolvingSymlinksInPath().path==games.path else {throw Failure.invalid("invalid game directory")}
        try progress.beginCommit();try fm.moveItem(at:output,to:destination)
        return result
    }

    static func install(source: URL, documents requestedDocuments: URL = documents, progress: LSX4PkgProgress = .init(),
                        storageBudget: UInt64? = nil) throws -> LSX4ImportedGame {
        let fm=FileManager.default
        let documents=requestedDocuments.resolvingSymlinksInPath().standardizedFileURL
        let id=try header(source)
        let games=documents.appendingPathComponent("Games",isDirectory:true)
        let destination=games.appendingPathComponent(id,isDirectory:true)
        guard !fm.fileExists(atPath:destination.path), !fm.fileExists(atPath:documents.appendingPathComponent(id).path) else { throw Failure.alreadyInstalled(id) }
        let available=(try documents.resourceValues(forKeys:[.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
        let budget=storageBudget ?? UInt64(max(0,available-reserve))
        guard budget > 0 else { throw Failure.storage }
        if progress.isCancelled { throw Failure.cancelled }
        let staging=documents.appendingPathComponent(".pkg-import-staging",isDirectory:true)
        try fm.createDirectory(at:staging,withIntermediateDirectories:true)
        guard staging.resolvingSymlinksInPath().path == staging.standardizedFileURL.path else { throw Failure.invalid("invalid import directory") }
        let transaction=staging.appendingPathComponent(UUID().uuidString,isDirectory:true)
        try fm.createDirectory(at:transaction,withIntermediateDirectories:false)
        // A transaction owns only this freshly created directory. Cleanup never
        // traverses a game's installed directory or its savedata tree.
        defer { try? fm.removeItem(at:transaction) }
        var excluded=transaction;var attributes=URLResourceValues();attributes.isExcludedFromBackup=true
        try? excluded.setResourceValues(attributes)
        let output=transaction.appendingPathComponent("game",isDirectory:true)
        let context=Unmanaged.passRetained(progress).toOpaque();defer { Unmanaged<LSX4PkgProgress>.fromOpaque(context).release() }
        let callback: LSX4PkgCallback = { context,phase,bytes,total,files,fileTotal in
            guard let context else { return 1 }
            return Unmanaged<LSX4PkgProgress>.fromOpaque(context).takeUnretainedValue().receive(phase,bytes,total,files,fileTotal)
        }
        var error=[CChar](repeating:0,count:4096)
        let status=source.path.withCString { input in output.path.withCString { output in
            extractPackage(input,output,budget,callback,context,&error,error.count)
        } }
        if progress.isCancelled { throw Failure.cancelled }
        guard status == 0 else { throw Failure.extraction(String(cString:error)) }
        let eboot=output.appendingPathComponent("eboot.bin"), param=output.appendingPathComponent("sce_sys/param.sfo")
        guard regular(eboot,within:output), regular(param,within:output) else { throw Failure.invalid("no complete executable and metadata were extracted") }
        let handle=try FileHandle(forReadingFrom:eboot);defer {try? handle.close()}
        let magic=Array(try handle.read(upToCount:4) ?? Data())
        guard magic == [0x7f,0x45,0x4c,0x46] || magic == [0x4f,0x15,0x3d,0x1d] else { throw Failure.invalid("eboot.bin is not a PS4 executable") }
        let metadata=try sfo(Data(contentsOf:param))
        guard metadata["TITLE_ID"] == id else { throw Failure.invalid("package and extracted TITLE_ID do not match") }
        if ["gp","ac"].contains(metadata["CATEGORY"] ?? "") { throw Failure.invalid("updates and DLC require a base-game installer and are not supported yet") }
        guard let files=fm.enumerator(at:output,includingPropertiesForKeys:[.isRegularFileKey,.isSymbolicLinkKey,.fileSizeKey],options:[]) else { throw Failure.invalid("cannot verify extracted files") }
        var count=0;var bytes:Int64=0
        for case let file as URL in files {
            let values=try file.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey,.fileSizeKey])
            guard values.isSymbolicLink != true else {throw Failure.invalid("unexpected symbolic link")}
            if values.isRegularFile == true {
                guard regular(file,within:output), count < 1_000_000 else {throw Failure.invalid("invalid extracted tree")}
                count += 1; bytes += Int64(values.fileSize ?? 0)
            }
        }
        if progress.isCancelled {throw Failure.cancelled}
        let title=String((metadata["TITLE"] ?? id).prefix(512))
        let result=LSX4ImportedGame(id:id,title:title,bytes:bytes,fileCount:count,installer:"pkg-pfs-v1")
        try JSONEncoder().encode(result).write(to:output.appendingPathComponent(".lsx4-complete.json"),options:.atomic)
        try fm.createDirectory(at:games,withIntermediateDirectories:true)
        guard games.resolvingSymlinksInPath().path == games.standardizedFileURL.path else {throw Failure.invalid("invalid game directory")}
        try progress.beginCommit()
        try fm.moveItem(at:output,to:destination)
        return result
    }

    static func importedGames(documents: URL = documents) -> [LSX4ImportedGame] {
        var result:[LSX4ImportedGame]=[]
        for root in [documents.appendingPathComponent("Games",isDirectory:true),documents] {
          for folder in ((try? FileManager.default.contentsOfDirectory(at:root,includingPropertiesForKeys:nil)) ?? []) {
            guard validID(folder.lastPathComponent),!result.contains(where:{$0.id==folder.lastPathComponent}),
                  let metadata=try? folderMetadata(folder),metadata.id==folder.lastPathComponent else {continue}
            var record:LSX4ImportedGame?
            if (try? folder.resourceValues(forKeys:[.isSymbolicLinkKey]).isSymbolicLink) != true,
                  regular(folder.appendingPathComponent("eboot.bin"),within:folder),
                  let size=try? folder.appendingPathComponent(".lsx4-complete.json").resourceValues(forKeys:[.fileSizeKey]).fileSize,
                  size <= 16384, let data=try? Data(contentsOf:folder.appendingPathComponent(".lsx4-complete.json")),
                  let saved=try? JSONDecoder().decode(LSX4ImportedGame.self,from:data),saved.id == folder.lastPathComponent {record=saved}
            if existingGameFolder(id:metadata.id,documents:documents) != nil {
                result.append(record ?? LSX4ImportedGame(id:metadata.id,title:metadata.title,bytes:0,fileCount:0,installer:"detected-folder"))
            }
          }
        }
        return result
    }
}
