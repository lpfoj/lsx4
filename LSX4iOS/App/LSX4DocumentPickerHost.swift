// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct LSX4DocumentPickerRequest:Equatable {let id:String;let folder:Bool}

// A stable controller attached to the import page owns one native presentation.
// No document-picker lifetime is tied to a recycled Form row or SwiftUI binding.
struct LSX4DocumentPickerHost:UIViewControllerRepresentable {
    let request:LSX4DocumentPickerRequest?
    let completion:(LSX4DocumentPickerRequest,Result<[URL],Error>)->Void
    func makeUIViewController(context:Context)->Controller {Controller()}
    func updateUIViewController(_ controller:Controller,context:Context) {
        controller.completion=completion;controller.request=request
        DispatchQueue.main.async {controller.presentIfReady()}
    }
    final class Controller:UIViewController,UIDocumentPickerDelegate,UIAdaptivePresentationControllerDelegate {
        var request:LSX4DocumentPickerRequest?
        var completion:((LSX4DocumentPickerRequest,Result<[URL],Error>)->Void)?
        private var active:LSX4DocumentPickerRequest?
        private var completedID=""
        private var observer:NSObjectProtocol?
        private var retries=0
        private var retrying=false
        private var visible=false
        override func viewDidLoad() {
            super.viewDidLoad();view.backgroundColor = .clear
            observer=NotificationCenter.default.addObserver(forName:UIApplication.didBecomeActiveNotification,object:nil,queue:.main) {[weak self] _ in self?.presentIfReady()}
        }
        deinit {if let observer {NotificationCenter.default.removeObserver(observer)}}
        override func viewDidAppear(_ animated:Bool) {super.viewDidAppear(animated);presentIfReady()}
        func presentIfReady() {
            guard let request,request.id != completedID,active==nil,
                UIApplication.shared.applicationState == .active,viewIfLoaded?.window != nil else {return}
            guard !isBeingPresented && !isBeingDismissed && presentedViewController==nil && parent?.isBeingPresented != true else {
                guard !retrying else {return}
                if retries>=20 {finish(request,.failure(NSError(domain:"LSX4PickerPresentation",code:2,userInfo:[NSLocalizedDescriptionKey:"The import page is still transitioning. Please try again."])));return}
                retries+=1;retrying=true
                DispatchQueue.main.asyncAfter(deadline:.now()+0.15) {[weak self] in self?.retrying=false;self?.presentIfReady()};return
            }
            active=request;retries=0;visible=false
            LSX4Diagnostics.shared.record("picker_native_will_create",["kind":request.folder ? "folder":"pkg"])
            var caught:NSError?
            let success=LSX4TryImportPresentation({
                let types:[UTType]=request.folder ? [.folder]:[UTType(filenameExtension:"pkg") ?? .data,.data]
                let picker=VisiblePicker(forOpeningContentTypes:types,asCopy:false)
                picker.delegate=self;picker.allowsMultipleSelection=false;picker.shouldShowFileExtensions=true
                picker.directoryURL=FileManager.default.urls(for:.documentDirectory,in:.userDomainMask).first
                picker.modalPresentationStyle = .fullScreen
                picker.appeared={[weak self] in self?.didShow()}
                picker.disappeared={LSX4Diagnostics.shared.record("picker_native_disappeared")}
                self.present(picker,animated:true) {[weak self] in picker.presentationController?.delegate=self;self?.didShow()}
            },&caught)
            if !success {finish(request,.failure(caught ?? NSError(domain:"LSX4PickerPresentation",code:1)))}
        }
        private func didShow() {
            guard let active,!visible else {return};visible=true
            LSX4Diagnostics.shared.record("picker_did_present",["kind":active.folder ? "folder":"pkg","implementation":"UIDocumentPickerViewController"])
        }
        func documentPicker(_ controller:UIDocumentPickerViewController,didPickDocumentsAt urls:[URL]) {close(controller,.success(urls))}
        func documentPickerWasCancelled(_ controller:UIDocumentPickerViewController) {close(controller,.failure(NSError(domain:NSCocoaErrorDomain,code:NSUserCancelledError)))}
        func presentationControllerDidDismiss(_ presentationController:UIPresentationController) {
            if let active {finish(active,.failure(NSError(domain:NSCocoaErrorDomain,code:NSUserCancelledError)))}
        }
        private func close(_ picker:UIDocumentPickerViewController,_ result:Result<[URL],Error>) {
            guard let active else {return}
            // Dismiss only this picker, never its presenting import page.
            if picker.presentingViewController != nil && !picker.isBeingDismissed {
                picker.dismiss(animated:true) {[weak self] in self?.finish(active,result)}
            } else {DispatchQueue.main.async {[weak self] in self?.finish(active,result)}}
        }
        private func finish(_ request:LSX4DocumentPickerRequest,_ result:Result<[URL],Error>) {
            guard completedID != request.id else {return};completedID=request.id;active=nil
            completion?(request,result)
        }
    }
    private final class VisiblePicker:UIDocumentPickerViewController {
        var appeared:(()->Void)?
        var disappeared:(()->Void)?
        override func viewDidAppear(_ animated:Bool) {super.viewDidAppear(animated);appeared?()}
        override func viewDidDisappear(_ animated:Bool) {super.viewDidDisappear(animated);disappeared?()}
    }
}
