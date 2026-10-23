// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import PhotosUI
import CoreTransferable
import UniformTypeIdentifiers
import AVFoundation
import AVKit
import Metal
import ImageIO

enum GoldReportDevice {
    static var machine:String {
        var value=utsname();uname(&value)
        let count=MemoryLayout.size(ofValue:value.machine)
        return withUnsafePointer(to:&value.machine) {$0.withMemoryRebound(to:CChar.self,capacity:count) {String(cString:$0)}}
    }
    static var model:String {machine == "iPhone18,2" ? "iPhone 17 Pro Max" : "iPhone (\(machine))"}
    @MainActor static var hardware:LSX4CommunityReport.Hardware {
        let bounds=UIScreen.main.nativeBounds
        return .init(machine:machine,modelName:model,gpu:MTLCreateSystemDefaultDevice()?.name,
                     cpuCores:ProcessInfo.processInfo.processorCount,memoryBytes:Int64(ProcessInfo.processInfo.physicalMemory),
                     screenWidth:Int(bounds.width),screenHeight:Int(bounds.height))
    }
    @MainActor static func draft(_ game:ConsoleTitle,result:LSX4ReportedResult,notes:String="",fps:String="",quick:Bool=false)->LSX4PublicReportDraft {
        LSX4PublicReportDraft(title:game.title,titleID:game.id,platform:game.id.hasPrefix("PPSA") ? "PS5" : "PS4",device:"Apple "+model,
            osVersion:UIDevice.current.systemVersion,version:Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "",
            executionMode:ConsoleExecutionMode.selected.label,result:result,
            notes:quick ? P("report_auto_note","Marked {result} from the LSX4 iOS library.",["result":result.label]) : notes,fps:fps,consent:true,
            hardware:hardware,source:quick ? "ios-quick-mark" : "manual")
    }
}

struct GoldQuickMarkView:View {
    let game:ConsoleTitle,result:LSX4ReportedResult
    @AppStorage(LSX4CommunityAPI.consentKey) private var accepted=false
    @State private var sending=false
    @State private var sent=false
    @State private var error=""
    var body:some View {
        GoldPage(title:result.label,dismissDisabled:sending) {
            ScrollView {VStack(alignment:.leading,spacing:16) {
                Text(game.title).font(.title2)
                Text("\(GoldReportDevice.model) · iOS \(UIDevice.current.systemVersion) · \(ConsoleExecutionMode.selected.label)").font(.subheadline)
                if sent {
                    Label(P("public_report_sent","Report published"),systemImage:"checkmark.circle.fill").foregroundStyle(.green).accessibilityIdentifier("quick-report-success")
                    Text(result == .playable ? P("report_positive_done","Published in the app and on lsx45.app. Open your entry in Tested compatible games to add a screenshot or video.") : P("report_negative_done","Published on lsx45.app. Unplayable reports are excluded from the app’s shared Playable list."))
                } else {
                    Text(P("report_metadata","Public: game, result, device model, CPU, RAM, GPU, screen size, iOS/app version and JIT mode. No name, unique device ID, saves or logs."))
                    Text(P("report_scope","Playable appears in the app and on lsx45.app; Unplayable appears only on the website. Community reports are unverified.")).font(.footnote)
                    Text(P("report_consent_note","After the first confirmation, marks update your public report immediately. You can disable sending in Settings. Optional media is uploaded only when selected.")).font(.footnote)
                    Link(P("privacy_policy","Privacy policy"),destination:URL(string:"https://lsx45.app/privacy/#9-optional-game-compatibility-reports")!)
                    if !error.isEmpty {Text(error).foregroundStyle(.red).accessibilityIdentifier("quick-report-error")}
                    Button {Task {await send()}} label: {
                        if sending {ProgressView(P("public_report_sending","Sending…"))}
                        else {Label(accepted ? P("report_retry","Retry publication") : P("report_agree","Agree and publish"),systemImage:"paperplane")}
                    }.buttonStyle(.borderedProminent).disabled(sending).accessibilityIdentifier("quick-report-publish")
                }
                if LSX4CommunityAPI.fixture {Text("ISOLATED TEST SERVICE · Nothing is posted to the public feed").font(.caption).foregroundStyle(.orange)}
            }.frame(maxWidth:.infinity,alignment:.leading).padding()}
        }.interactiveDismissDisabled(sending).task {if accepted && !sent {await send()}}
    }
    @MainActor private func send() async {
        guard !sending else {return};accepted=true;sending=true;error=""
        defer {sending=false}
        do {
            _=try await LSX4CommunityCache.shared.publish(GoldReportDevice.draft(game,result:result,quick:true))
            UserDefaults.standard.set(result == .playable ? "supported" : "unsupported",forKey:"gold.status."+game.id)
            sent=true
        } catch {self.error=error.localizedDescription}
    }
}

/// Transfer provider URLs are temporary: copy before returning from the import callback.
struct GoldReportMovie:Transferable {
    let url:URL
    static var transferRepresentation:some TransferRepresentation {
        FileRepresentation(importedContentType:.movie) {received in
            let destination=FileManager.default.temporaryDirectory.appendingPathComponent("report-"+UUID().uuidString).appendingPathExtension(received.file.pathExtension)
            try FileManager.default.copyItem(at:received.file,to:destination)
            return Self(url:destination)
        }
    }
}

enum GoldReportMedia {
    struct Prepared {let url:URL,mime:String}
    static func screenshot(_ data:Data)throws->Prepared {
        guard data.count<=40*1024*1024,let source=CGImageSourceCreateWithData(data as CFData,nil),
            let image=CGImageSourceCreateThumbnailAtIndex(source,0,[kCGImageSourceCreateThumbnailFromImageAlways:true,kCGImageSourceThumbnailMaxPixelSize:4096,kCGImageSourceCreateThumbnailWithTransform:true] as CFDictionary),
            let jpeg=UIImage(cgImage:image).jpegData(compressionQuality:0.9),jpeg.count<=8*1024*1024 else {
            throw LSX4CommunityAPI.HTTPFailure(status:413,message:P("report_image_limit","Choose a screenshot up to 8 MB."))
        }
        let url=FileManager.default.temporaryDirectory.appendingPathComponent("report-"+UUID().uuidString+".jpg")
        try jpeg.write(to:url,options:.atomic);return Prepared(url:url,mime:"image/jpeg")
    }
    static func video(_ input:URL) async throws->Prepared {
        let count=try input.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? 0
        guard count>0,count<=100*1024*1024 else {throw LSX4CommunityAPI.HTTPFailure(status:413,message:P("report_video_limit","Choose a video up to 100 MB. Trim it in Photos first if needed."))}
        let asset=AVURLAsset(url:input)
        let presets=AVAssetExportSession.exportPresets(compatibleWith:asset)
        let preset=presets.contains(AVAssetExportPresetPassthrough) ? AVAssetExportPresetPassthrough : AVAssetExportPresetHighestQuality
        guard let exporter=AVAssetExportSession(asset:asset,presetName:preset),exporter.supportedFileTypes.contains(.mp4) else {
            throw LSX4CommunityAPI.HTTPFailure(status:400,message:P("report_video_convert","This video cannot be converted to MP4. Choose another clip."))
        }
        let output=FileManager.default.temporaryDirectory.appendingPathComponent("report-"+UUID().uuidString+".mp4")
        exporter.outputURL=output;exporter.outputFileType = .mp4;exporter.shouldOptimizeForNetworkUse=true;exporter.metadata=[]
        do {
            try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation {(continuation:CheckedContinuation<Void,Error>) in
                    exporter.exportAsynchronously {
                        if exporter.status == .completed {continuation.resume()}
                        else {continuation.resume(throwing:exporter.error ?? CancellationError())}
                    }
                }
            } onCancel: {exporter.cancelExport()}
            let bytes=try output.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? 0
            guard bytes>=16,bytes<=100*1024*1024 else {throw LSX4CommunityAPI.HTTPFailure(status:413,message:P("report_mp4_limit","The MP4 must be no larger than 100 MB. Trim the clip and try again."))}
            return Prepared(url:output,mime:"video/mp4")
        } catch {try? FileManager.default.removeItem(at:output);throw error}
    }
}

struct GoldCommunityReportDetail:View {
    @State var report:LSX4CommunityReport
    @ObservedObject private var cache=LSX4CommunityCache.shared
    @AppStorage(LSX4ReportSharing.key) private var sharing=true
    @State private var photo:PhotosPickerItem?=nil
    @State private var movie:PhotosPickerItem?=nil
    @State private var busy=false
    @State private var message=""
    private var mine:Bool {LSX4ReportOwnership.key(report.id) != nil}
    var body:some View {
        ScrollView {VStack(alignment:.leading,spacing:16) {
            Text(report.gameTitle).font(.title2)
            if mine {Label(P("report_mine","Your report · this device"),systemImage:"person.crop.circle.badge.checkmark").foregroundStyle(.mint).accessibilityIdentifier("community-your-report")}
            Text(P("report_unverified","User submitted · unverified")).font(.caption).foregroundStyle(.secondary)
            Text([report.device,"iOS "+(report.osVersion ?? ""),report.executionMode ?? "","LSX4 "+report.appVersion,report.titleId].filter {!$0.isEmpty}.joined(separator:" · "))
            if let specs=report.hardware {
                Text([specs.machine,specs.cpuCores.map {P("device_cores","{count} CPU cores",["count":String($0)])},specs.memoryBytes.map {P("device_ram","{size} GB RAM",["size":String(format:"%.1f",Double($0)/1_073_741_824)])},specs.gpu,
                      specs.screenWidth.flatMap {width in specs.screenHeight.map {"\(width) × \($0) px"}}].compactMap {$0}.joined(separator:" · ")).font(.footnote).foregroundStyle(.secondary)
            }
            if let fps=report.fps {Text(P("report_fps","{fps} FPS · self-reported",["fps":String(format:"%.1f",fps)])).foregroundStyle(.green)}
            Text(report.notes)
            ForEach(report.media ?? []) {media in
                if let url=LSX4CommunityAPI.mediaURL(media) {
                    if media.kind == "screenshot" {
                        AsyncImage(url:url) {phase in
                            if let image=phase.image {image.resizable().scaledToFit()}
                            else if phase.error != nil {Label(P("report_image_unavailable","Image unavailable. Refresh to retry."),systemImage:"photo")}
                            else {ProgressView()}
                        }.frame(maxHeight:280).accessibilityIdentifier("community-screenshot")
                    } else if media.kind == "screencast" {GoldCommunityVideo(url:url).frame(height:230).accessibilityIdentifier("community-screencast")}
                    if mine {Button(P("report_remove_media","Remove {kind}",["kind":media.kind == "screenshot" ? P("media_image","screenshot") : P("media_video","screencast")]),role:.destructive) {Task {await remove(media.kind)}}.disabled(busy || !sharing)}
                }
            }
            if mine {
                Divider()
                if !sharing {Text(P("sharing_off_message","Sending is off. Marks are saved on this iPhone. Enable report sharing in Settings to publish."))}
                Text(P("report_evidence","Add or replace public evidence (optional)")).font(.headline)
                Text(P("report_media_notice","One screenshot (8 MB) and one video (100 MB). Selecting starts public upload to the app and website. Check names, notifications and voices first. The previous file is kept until its replacement succeeds.")).font(.footnote)
                PhotosPicker(selection:$photo,matching:.images) {Label(P("report_choose_image","Choose screenshot"),systemImage:"photo")}.accessibilityIdentifier("community-choose-screenshot").disabled(busy || !sharing)
                PhotosPicker(selection:$movie,matching:.videos) {Label(P("report_choose_video","Choose screencast"),systemImage:"video")}.accessibilityIdentifier("community-choose-screencast").disabled(busy || !sharing)
                if LSX4CommunityAPI.fixture {
                    Button("Upload isolated screenshot fixture") {Task {await fixture("screenshot")}}.accessibilityIdentifier("community-fixture-screenshot").disabled(busy || !sharing)
                    Button("Upload isolated video fixture") {Task {await fixture("screencast")}}.accessibilityIdentifier("community-fixture-screencast").disabled(busy || !sharing)
                }
            }
            if busy {ProgressView(P("report_media_busy","Preparing / uploading… Keep LSX4 open."))}
            if !message.isEmpty {Text(message).accessibilityIdentifier("community-media-status")}
            Button(P("report_refresh","Refresh report")) {Task {await refresh()}}.disabled(busy).accessibilityIdentifier("community-report-refresh")
            Link(P("report_website","View iOS reports on lsx45.app"),destination:URL(string:"https://lsx45.app/playable/#ios-reports")!)
        }.frame(maxWidth:.infinity,alignment:.leading).padding()}
        .navigationTitle(report.gameTitle).navigationBarTitleDisplayMode(.inline)
        .interactiveDismissDisabled(busy)
        .task {await refresh()}
        .onChange(of:photo) {_,item in guard let item else {return};Task {await selectedPhoto(item)}}
        .onChange(of:movie) {_,item in guard let item else {return};Task {await selectedMovie(item)}}
    }
    @MainActor private func refresh() async {
        do {report=try await LSX4CommunityAPI.shared.one(report.id);cache.accept(report)} catch {message=error.localizedDescription}
    }
    @MainActor private func upload(_ prepared:GoldReportMedia.Prepared,kind:String) async throws {
        defer {try? FileManager.default.removeItem(at:prepared.url)}
        report=try await LSX4CommunityAPI.shared.attachment(report.id,kind:kind,file:prepared.url,mime:prepared.mime)
        cache.accept(report);message=P("report_media_done","Published. Everyone can see your updated {kind} after refreshing.",["kind":kind == "screenshot" ? P("media_image","screenshot") : P("media_video","screencast")])
    }
    @MainActor private func selectedPhoto(_ item:PhotosPickerItem) async {
        guard !busy else {return};busy=true;message="";defer {busy=false;photo=nil}
        do {
            guard let data=try await item.loadTransferable(type:Data.self) else {throw CocoaError(.fileReadUnknown)}
            let prepared=try await Task.detached {try GoldReportMedia.screenshot(data)}.value
            try await upload(prepared,kind:"screenshot")
        } catch {message=error.localizedDescription}
    }
    @MainActor private func selectedMovie(_ item:PhotosPickerItem) async {
        guard !busy else {return};busy=true;message="";defer {busy=false;movie=nil}
        do {
            guard let file=try await item.loadTransferable(type:GoldReportMovie.self) else {throw CocoaError(.fileReadUnknown)}
            defer {try? FileManager.default.removeItem(at:file.url)}
            try await upload(GoldReportMedia.video(file.url),kind:"screencast")
        } catch {message=error.localizedDescription}
    }
    @MainActor private func remove(_ kind:String) async {
        busy=true;defer {busy=false}
        do {report=try await LSX4CommunityAPI.shared.attachment(report.id,kind:kind,file:nil);cache.accept(report);message=P("report_removed","Attachment removed.")}
        catch {message=error.localizedDescription}
    }
    @MainActor private func fixture(_ kind:String) async {
        busy=true;message="";defer {busy=false}
        do {
            guard let file=Bundle.main.url(forResource:kind == "screenshot" ? "CommunityFixtureImage" : "CommunityFixtureMovie",withExtension:kind == "screenshot" ? "png" : "mov") else {throw CocoaError(.fileNoSuchFile)}
            let prepared:GoldReportMedia.Prepared
            if kind == "screenshot" {prepared=try GoldReportMedia.screenshot(Data(contentsOf:file))}
            else {prepared=try await GoldReportMedia.video(file)}
            try await upload(prepared,kind:kind)
        } catch {message=error.localizedDescription}
    }
}

private struct GoldCommunityVideo:View {
    let url:URL
    @State private var player:AVPlayer?
    var body:some View {VideoPlayer(player:player).onAppear {player=AVPlayer(url:url)}.onDisappear {player?.pause();player=nil}}
}
