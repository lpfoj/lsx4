// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers
import UIKit

@MainActor private final class LSX4PkgImportModel: ObservableObject {
    @Published var busy=false
    @Published var preparing=false
    @Published var pickerRequest:LSX4DocumentPickerRequest?
    private var operation=""
    var pickerOpen=false
    var diagnosticOpen=false
    @Published var fraction: Double?
    @Published var detail=""
    @Published var successful=false
    private var progress: LSX4PkgProgress?
    private var backgroundTask=UIBackgroundTaskIdentifier.invalid

    func start(_ source: URL,folder:Bool=false) {
        guard !busy else {return}
        LSX4Diagnostics.shared.record("import_start",["kind":folder ? "folder" : "pkg"])
        busy=true;successful=false;fraction=nil;detail=L("pkg.import.opening")
        let box=LSX4PkgProgress();progress=box
        box.update = { [weak self] snapshot in
            Task { @MainActor in
                guard let self,self.busy else {return}
                if snapshot.phase == 3 {
                    self.fraction=0.99;self.detail=L("pkg.import.validating")
                } else {
                    self.fraction=snapshot.total > 0 ? min(0.99,Double(snapshot.bytes)/Double(snapshot.total)) : nil
                    let bytes=ByteCountFormatter.string(fromByteCount:Int64(clamping:snapshot.bytes),countStyle:.file)
                    let total=ByteCountFormatter.string(fromByteCount:Int64(clamping:snapshot.total),countStyle:.file)
                    self.detail=snapshot.total > 0 ? L("pkg.import.progress",["bytes":bytes,"total":total,"files":String(snapshot.files),"count":String(snapshot.fileTotal)]) : L("pkg.import.extracting")
                }
            }
        }
        backgroundTask=UIApplication.shared.beginBackgroundTask(withName:"LSX4 PKG import") { box.cancel() }
        LSX4Diagnostics.shared.record("security_scope_will_start")
        let scoped=source.startAccessingSecurityScopedResource()
        LSX4Diagnostics.shared.record("security_scope_started",["granted":scoped])
        Task.detached(priority:.userInitiated) { [weak self,box] in
            defer {if scoped {source.stopAccessingSecurityScopedResource()}}
            var coordinationError:NSError?
            var result:Result<LSX4ImportedGame,Error>?
            LSX4Diagnostics.shared.record("file_coordination_will_start")
            NSFileCoordinator(filePresenter:nil).coordinate(readingItemAt:source,options:[],error:&coordinationError) { url in
                LSX4Diagnostics.shared.record("file_coordination_entered")
                result=Result {try folder ? LSX4PkgInstaller.installFolder(source:url,progress:box) : LSX4PkgInstaller.install(source:url,progress:box)}
            }
            let answer=result ?? .failure(coordinationError ?? LSX4PkgInstaller.Failure.invalid("the file provider did not open the PKG"))
            await self?.finish(answer)
        }
    }
    func pickerFailed(_ error:Error) {
        let value=error as NSError
        LSX4Diagnostics.shared.record("picker_error",["domain":value.domain,"code":value.code,"message":value.localizedDescription,
            "exceptionName":value.userInfo["exceptionName"] as? String ?? "","stack":value.userInfo["stack"] as? [String] ?? []])
        if value.domain==NSCocoaErrorDomain && value.code==NSUserCancelledError {return}
        finish(.failure(error))
    }
    func closeDiagnosticScope() {
        guard diagnosticOpen && !pickerOpen && !preparing && !busy else {return}
        diagnosticOpen=false;operation="";pickerRequest=nil;LSX4Diagnostics.shared.endOperation()
    }
    func diagnosticFailure() {preparing=false;detail=P("picker_open_error","The file picker could not be opened. Close the sheet and try again.")}
    func choose(folder:Bool) {
        guard !busy && !preparing && !pickerOpen else {return}
        preparing=true;diagnosticOpen=true;detail=""
        operation=LSX4Diagnostics.shared.startOperation(folder ? "folder":"pkg")
        let request=LSX4DocumentPickerRequest(id:operation,folder:folder)
        LSX4Diagnostics.shared.record("picker_button_pressed",["kind":folder ? "folder":"pkg"])
        LSX4Diagnostics.shared.beforePicker(folder:folder) {[weak self] confirmed in
            guard let self,self.diagnosticOpen,self.operation==request.id else {return}
            self.preparing=false
            if confirmed {self.pickerOpen=true;self.pickerRequest=request}
            else {self.diagnosticFailure()}
        }
    }
    func selected(_ request:LSX4DocumentPickerRequest,_ result:Result<[URL],Error>) {
        guard diagnosticOpen && operation==request.id else {return}
        pickerOpen=false;pickerRequest=nil
        switch result {
        case .success(let urls):
            guard let source=urls.first else {LSX4Diagnostics.shared.record("picker_empty_selection");return}
            preparing=true
            LSX4Diagnostics.shared.checkpoint("picker_selected",details:["kind":request.folder ? "folder":"pkg","isFileURL":source.isFileURL]) {[weak self] confirmed in
                guard let self,self.diagnosticOpen,self.operation==request.id else {return}
                self.preparing=false
                if confirmed {self.start(source,folder:request.folder)} else {self.diagnosticFailure()}
            }
        case .failure(let error):pickerFailed(error)
        }
    }
    func cancel() {LSX4Diagnostics.shared.record("import_cancel_requested");progress?.cancel();detail=L("pkg.import.cancelling")}
    private func finish(_ result: Result<LSX4ImportedGame,Error>) {
        busy=false;progress=nil
        if backgroundTask != .invalid {UIApplication.shared.endBackgroundTask(backgroundTask);backgroundTask = .invalid}
        switch result {
        case .success(let game):LSX4Diagnostics.shared.record("import_succeeded");successful=true;fraction=1;detail=L("pkg.import.done",["title":game.title])
        case .failure(let error):
            let value=error as NSError
            LSX4Diagnostics.shared.record("import_failed",["domain":value.domain,"code":value.code,"message":value.localizedDescription])
            successful=false;fraction=nil;detail=error.localizedDescription
        }
    }
}

struct LSX4PkgImportView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model=LSX4PkgImportModel()
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text(L("pkg.import.instructions"))
                    LSX4GameImportButton(folder:false,model:model)
                    LSX4GameImportButton(folder:true,model:model)
                    Text(L("pkg.import.supported")).font(.footnote).foregroundStyle(.secondary)
                }
                if model.preparing {Section {ProgressView(P("picker_preparing","Opening file picker…"))}}
                if model.busy || !model.detail.isEmpty {
                    Section {
                        if model.busy {
                            if let fraction=model.fraction {ProgressView(value:fraction)} else {ProgressView()}
                        }
                        Text(model.detail).foregroundStyle(model.successful ? .green : .primary)
                            .accessibilityIdentifier("pkg-import-status")
                        if model.busy {Button(L("Отмена"),role:.cancel) {model.cancel()}.accessibilityIdentifier("pkg-cancel-import")}
                    }
                }
            }
            .navigationTitle(L("pkg.import.title"))
            .toolbar {ToolbarItem(placement:.confirmationAction) {Button(L("Готово")) {model.closeDiagnosticScope();dismiss()}.disabled(model.busy || model.preparing || model.pickerOpen)}}
            .interactiveDismissDisabled(model.busy || model.preparing)
            .onDisappear {model.closeDiagnosticScope()}

        }.background(LSX4DocumentPickerHost(request:model.pickerRequest,completion:model.selected).frame(width:0,height:0))
        .preferredColorScheme(.dark)
    }
}

private struct LSX4GameImportButton:View {
    let folder:Bool
    @ObservedObject var model:LSX4PkgImportModel
    var body:some View {
        Button(L(folder ? "pkg.import.folder" : "pkg.import.choose")) {model.choose(folder:folder)}
            .disabled(model.busy || model.preparing || model.pickerOpen).accessibilityIdentifier(folder ? "pkg-choose-folder" : "pkg-choose-file")
    }
}
