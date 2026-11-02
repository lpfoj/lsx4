// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct LSX4LocalPackage:Identifiable,Sendable,Equatable {
    let url:URL
    let relativePath:String
    let bytes:Int64
    let modified:Date?
    var id:String {relativePath}
}

enum LSX4LocalPackages {
    enum Failure:LocalizedError {
        case changed,limit
        var errorDescription:String? {
            switch self {
            case .changed:return P("local_pkg_changed","The file changed or is no longer available. Finish copying it, then refresh the list.")
            case .limit:return P("local_pkg_limit","Too many folders to scan. Place the PKG directly in the LSX4 Games folder, then refresh.")
            }
        }
    }
    private static let keys:Set<URLResourceKey>=[.isRegularFileKey,.isDirectoryKey,.isSymbolicLinkKey,.fileSizeKey,.contentModificationDateKey]

    static func scan(documents:URL) throws->[LSX4LocalPackage] {
        let fm=FileManager.default,root=documents.resolvingSymlinksInPath().standardizedFileURL
        var result=[LSX4LocalPackage](),visited=0
        func visit(_ directory:URL,_ depth:Int) throws {
            try Task.checkCancellation()
            let values=try directory.resourceValues(forKeys:[.isDirectoryKey,.isSymbolicLinkKey])
            guard values.isDirectory == true,values.isSymbolicLink != true else {return}
            for file in try fm.contentsOfDirectory(at:directory,includingPropertiesForKeys:Array(keys),options:[.skipsHiddenFiles]) {
                try Task.checkCancellation();visited+=1
                guard visited<=50_000 else {throw Failure.limit}
                let value=try file.resourceValues(forKeys:keys)
                guard value.isSymbolicLink != true else {continue}
                if value.isRegularFile == true,file.pathExtension.lowercased()=="pkg" {
                    let path=file.standardizedFileURL.path
                    guard path.hasPrefix(root.path+"/") else {continue}
                    result.append(.init(url:file,relativePath:String(path.dropFirst(root.path.count+1)),bytes:Int64(value.fileSize ?? 0),modified:value.contentModificationDate))
                } else if value.isDirectory == true {
                    if depth==0 && !["Games","Inbox","Imports"].contains(file.lastPathComponent) {continue}
                    if fm.fileExists(atPath:file.appendingPathComponent("eboot.bin").path),fm.fileExists(atPath:file.appendingPathComponent("sce_sys").path) {continue}
                    guard depth<8 else {throw Failure.limit}
                    try visit(file,depth+1)
                }
            }
        }
        try visit(root,0)
        return result.sorted {$0.relativePath.localizedStandardCompare($1.relativePath) == .orderedAscending}
    }

    static func validatedURL(_ package:LSX4LocalPackage,documents:URL) throws->URL {
        let root=documents.resolvingSymlinksInPath().standardizedFileURL
        let file=package.url.standardizedFileURL
        guard file.isFileURL,file.path.hasPrefix(root.path+"/"),file.pathExtension.lowercased()=="pkg",
              file.resolvingSymlinksInPath().path==file.path else {throw Failure.changed}
        let values=try file.resourceValues(forKeys:keys)
        guard values.isRegularFile==true,values.isSymbolicLink != true,
              Int64(values.fileSize ?? -1)==package.bytes,values.contentModificationDate==package.modified,
              FileManager.default.isReadableFile(atPath:file.path) else {throw Failure.changed}
        return file
    }
}
