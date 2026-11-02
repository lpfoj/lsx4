import SwiftUI
import UIKit

struct GoldReportEditor:View {
    let title:String,titleID:String,platform:String
    var existing:LSX4CommunityReport?
    var saved:((LSX4CommunityReport)->Void)?
    @State private var result:LSX4ReportedResult?
    @State private var notes=""
    @State private var fps=""
    @State private var device=""
    @State private var gpu=""
    @State private var version=""
    @State private var error=""
    @State private var consent=false
    @State private var sending=false
    @State private var initialized=false
    @State private var published:LSX4CommunityReport?
    @AppStorage(LSX4ReportSharing.key) private var sharing=true
    private var currentVersion:String {Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? ""}
    init(game:ConsoleTitle) {title=game.title;titleID=game.id;platform=game.id.hasPrefix("PPSA") ? "PS5" : "PS4"}
    init(report:LSX4CommunityReport,saved:((LSX4CommunityReport)->Void)?=nil) {title=report.gameTitle;titleID=report.titleId;platform=report.platform;existing=report;self.saved=saved}
    var body:some View {
        Form {
            Section {Text(title).font(.headline);Text(titleID+" · "+platform).font(.caption).foregroundStyle(.secondary)}
            if let published {
                Section {
                    Label(P("public_report_sent","Report published"),systemImage:"checkmark.circle.fill").foregroundStyle(.mint)
                    Text(P("report_published_both","The result is visible in its community section in the app and on lsx45.app."))
                    NavigationLink {GoldCommunityReportDetail(report:published)} label: {Label(P("report_add_media","Open report · add screenshot or video"),systemImage:"photo.on.rectangle")}.accessibilityIdentifier("report-open-published")
                }
            } else {
                Section(P("report_result_section","Your result")) {
                    Picker(P("game_status","Result"),selection:$result) {
                        Text(P("gold_result_choose","Choose the observed result")).tag(LSX4ReportedResult?.none)
                        ForEach(LSX4ReportedResult.allCases) {Text($0.label).tag(Optional($0))}
                    }.accessibilityIdentifier("public-report-result")
                    TextField(P("report_fps_optional","FPS (optional)"),text:$fps).keyboardType(.decimalPad).accessibilityIdentifier("public-report-fps")
                    TextField(P("public_report_notes","Describe gameplay or the problem (10–1200 characters)"),text:$notes,axis:.vertical).lineLimit(3...8).accessibilityIdentifier("public-report-notes")
                        .onChange(of:notes) {_,value in if value.count>1200 {notes=String(value.prefix(1200))}}
                }
                Section(P("report_device_section","Device and app version")) {
                    LabeledContent(P("device_model","Device")) {TextField(P("device_model","Device"),text:$device).multilineTextAlignment(.trailing)}
                    LabeledContent("GPU") {TextField("GPU",text:$gpu).multilineTextAlignment(.trailing)}
                    LabeledContent(P("version","Version")) {TextField(P("version","Version"),text:$version).textInputAutocapitalization(.never).multilineTextAlignment(.trailing).accessibilityIdentifier("report-app-version")}
                    if version != currentVersion {
                        Button(P("report_use_current","Use current version ({version})",["version":currentVersion])) {version=currentVersion}.accessibilityIdentifier("report-use-current-version")
                    }
                    Text(P("report_edit_metadata","Device and GPU are filled automatically and can be edited. When editing a report, its original app version is kept.")).font(.footnote).foregroundStyle(.secondary)
                }
                Section {
                    Toggle(P("public_report_consent","Publish this game, device model, CPU, RAM, GPU, screen size, app version, result and comment in the community lists. No logs, saves or unique device identifier are sent."),isOn:$consent).accessibilityIdentifier("public-report-consent")
                    Text(P("report_media_after","Screenshots and screencasts are optional. You can add or replace them after publishing." )).font(.footnote).foregroundStyle(.secondary)
                    if !sharing {Text(P("sharing_off_message","Sending is off. Marks are saved on this iPhone. Enable report sharing in Settings to publish."))}
                    if !error.isEmpty {Text(error).foregroundStyle(.orange).accessibilityIdentifier("public-report-error")}
                    Button {Task {await send()}} label: {
                        if sending {ProgressView(P("public_report_sending","Sending…"))}
                        else {Label(existing == nil ? P("public_report_send","Send public report") : P("report_save","Save report"),systemImage:"paperplane")}
                    }.disabled(!sharing || !consent || result == nil || notes.trimmingCharacters(in:.whitespacesAndNewlines).count<10 || device.trimmingCharacters(in:.whitespacesAndNewlines).count<2 || version.isEmpty).accessibilityIdentifier("public-report-send")
                }
            }
        }.scrollContentBackground(.hidden).disabled(sending).interactiveDismissDisabled(sending)
        .navigationTitle(existing == nil ? P("public_report_title","Report a game result") : P("report_edit","Edit report"))
        .task {initialize()}
    }
    @MainActor private func initialize() {
        guard !initialized else {return};initialized=true
        device=existing?.device ?? "Apple "+GoldReportDevice.model
        gpu=existing?.hardware?.gpu ?? GoldReportDevice.hardware.gpu ?? "Apple GPU"
        version=existing?.appVersion ?? currentVersion
        notes=existing?.notes ?? UserDefaults.standard.string(forKey:"gold.notes."+titleID) ?? ""
        fps=existing?.fps.map {String(format:"%.1f",$0)} ?? ""
        result=existing.flatMap {LSX4ReportedResult(rawValue:$0.result)}
        if existing == nil {
            let status=UserDefaults.standard.string(forKey:"gold.status."+titleID)
            result=status == "supported" ? .playable : status == "unsupported" ? .unplayable : nil
        }
    }
    @MainActor private func send() async {
        sending=true;error="";defer {sending=false}
        do {
            guard let result else {return}
            var hardware=existing?.hardware ?? GoldReportDevice.hardware;hardware.gpu=gpu;hardware.modelName=device
            let draft=LSX4PublicReportDraft(title:title,titleID:titleID,platform:platform,device:device,
                osVersion:existing?.osVersion ?? UIDevice.current.systemVersion,version:version,
                executionMode:existing?.executionMode ?? ConsoleExecutionMode.selected.label,result:result,
                notes:notes,fps:fps,consent:consent,hardware:hardware,source:existing?.reportSource ?? "manual")
            let (_,report)=try await LSX4CommunityAPI.shared.submit(draft,existing:existing?.id)
            LSX4CommunityCache.shared.accept(report);published=report;saved?(report)
            UserDefaults.standard.set(result == .playable ? "supported" : "unsupported",forKey:"gold.status."+titleID)
            UserDefaults.standard.set(notes,forKey:"gold.notes."+titleID)
            await LSX4AccountSession.shared.refresh()
        } catch {self.error=error.localizedDescription}
    }
}
