// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct LSX4LocalPackagePicker:View {
    let select:(LSX4LocalPackage)->Void
    @State private var packages=[LSX4LocalPackage]()
    @State private var loading=false
    @State private var error:String?
    var body:some View {
        GoldPage(title:P("local_pkg_title","PKG files in LSX4")) {
            List {
                Section {
                    Button {Task {await refresh()}} label: {
                        HStack {Label(P("local_pkg_refresh","Refresh files"),systemImage:"arrow.clockwise");Spacer();if loading {ProgressView()}}
                            .contentShape(Rectangle())
                    }.disabled(loading).accessibilityIdentifier("local-pkg-refresh")
                }
                if let error {
                    Section {Text(error).foregroundStyle(.orange).accessibilityIdentifier("local-pkg-error")}
                }
                if !loading && packages.isEmpty && error==nil {
                    Section {Text(P("local_pkg_empty","No PKG files found yet.")).frame(maxWidth:.infinity,minHeight:90).foregroundStyle(.secondary).accessibilityIdentifier("local-pkg-empty")}
                }
                ForEach(packages) {package in
                    Button {select(package)} label: {
                        HStack(spacing:12) {
                            Image(systemName:"doc.zipper").font(.title2).frame(width:32)
                            VStack(alignment:.leading,spacing:5) {
                                Text(package.url.lastPathComponent).font(.headline).lineLimit(2)
                                Text(package.relativePath).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                                Text(ByteCountFormatter.string(fromByteCount:package.bytes,countStyle:.file)).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer(minLength:0)
                            Image(systemName:"square.and.arrow.down").font(.title3)
                        }.padding(.vertical,6).contentShape(Rectangle())
                    }.accessibilityIdentifier("local-pkg-"+package.relativePath)
                }
            }.scrollContentBackground(.hidden).refreshable {await refresh()}
        }.task {await refresh()}
    }
    @MainActor private func refresh() async {
        guard !loading else {return};loading=true;error=nil
        let documents=LSX4PkgInstaller.documents
        let job=Task.detached(priority:.userInitiated) {try LSX4LocalPackages.scan(documents:documents)}
        defer {loading=false}
        do {
            let files=try await withTaskCancellationHandler(operation:{try await job.value},onCancel:{job.cancel()})
            guard !Task.isCancelled else {return};packages=files
        } catch is CancellationError {} catch {
            guard !Task.isCancelled else {return};packages=[];self.error=error.localizedDescription
        }
    }
}
