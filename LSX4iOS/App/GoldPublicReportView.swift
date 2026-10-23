// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

extension LSX4ReportedResult {
    var label:String {
        switch self {
        case .playable:return G("gold_result_playable","Playable")
        case .unplayable:return G("gold_result_unplayable","Unplayable")
        }
    }
}
struct GoldPublicReportView:View {
    let game:ConsoleTitle
    @AppStorage(LSX4ReportSharing.key) private var sharing=true
    @State private var result:LSX4ReportedResult?=nil
    @State private var notes=""
    @State private var fps=""
    @State private var error=""
    @State private var consent=false
    @State private var sending=false
    @State private var receipt:LSX4PublicReportReceipt?=nil
    init(game:ConsoleTitle) {self.game=game}
    private var platform:String {game.id.hasPrefix("PPSA") ? "PS5" : "PS4"}
    private var machine:String {
        var value=utsname();uname(&value)
        let capacity=MemoryLayout.size(ofValue:value.machine)
        return withUnsafePointer(to:&value.machine) {$0.withMemoryRebound(to:CChar.self,capacity:capacity) {String(cString:$0)}}
    }
    var body:some View {
        GoldPage(title:G("public_report_title","Report a game result"),dismissDisabled:sending) {
            Form {
                Section {
                    Text(game.title).font(.headline);Text(game.id+" · "+platform)
                    Text("Apple \(machine) · iOS \(UIDevice.current.systemVersion) · LSX4 \(Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "")")
                    LabeledContent("CPU",value:ConsoleExecutionMode.selected.label)
                }
                if let receipt {
                    Section {
                        Label(G("public_report_sent","Report published"),systemImage:"checkmark.circle.fill").foregroundStyle(.green)
                        Text(G("public_report_sent_note","Playable reports appear in the app and on the website. Other results appear only on the website. The private editing key is saved on this phone."))
                        Link(G("public_report_view","View on website"),destination:URL(string:"https://lsx45.app/playable/#ios-reports")!)
                        Button(G("public_report_copy_key","Copy removal key")) {UIPasteboard.general.string=receipt.id+"\n"+receipt.removalKey}
                    }
                } else {
                    Section {
                        Picker(G("game_status","Result"),selection:$result) {
                            Text(G("gold_result_choose","Choose the observed result")).tag(LSX4ReportedResult?.none)
                            ForEach(LSX4ReportedResult.allCases) {Text($0.label).tag(Optional($0))}
                        }.accessibilityIdentifier("public-report-result")
                        TextField("FPS (optional)",text:$fps).keyboardType(.decimalPad).accessibilityIdentifier("public-report-fps")
                        TextField(G("public_report_notes","Describe gameplay or the problem (10–1200 characters)"),text:$notes,axis:.vertical).lineLimit(3...6).accessibilityIdentifier("public-report-notes")
                            .onChange(of:notes) {_,value in if value.count>1200 {notes=String(value.prefix(1200))}}
                        Toggle(G("public_report_consent","Publish this game, device model, CPU, RAM, GPU, screen size, app version, result and comment in the community lists. No logs, saves or unique device identifier are sent."),isOn:$consent).accessibilityIdentifier("public-report-consent")
                        Link(G("public_report_privacy","Read the privacy policy"),destination:URL(string:"https://lsx45.app/privacy/#9-optional-game-compatibility-reports")!)
                    }.disabled(sending)
                    Section {
                        if !sharing {Text(P("sharing_off_message","Sending is off. Marks are saved on this iPhone. Enable report sharing in Settings to publish."))}
                        if !error.isEmpty {Text(error).foregroundStyle(.red).accessibilityIdentifier("public-report-error")}
                        Button {Task {await send()}} label: {
                            if sending {ProgressView(G("public_report_sending","Sending…"))}
                            else {Label(G("public_report_send","Send public report"),systemImage:"paperplane")}
                        }.disabled(sending || !sharing || !consent || result==nil || notes.trimmingCharacters(in:.whitespacesAndNewlines).count<10).accessibilityIdentifier("public-report-send")
                    }
                }
            }.scrollContentBackground(.hidden).interactiveDismissDisabled(sending)
        }.onAppear {
            let status=UserDefaults.standard.string(forKey:"gold.status."+game.id) ?? ""
            result=status=="supported" ? .playable : status=="unsupported" ? .unplayable : nil
            notes=UserDefaults.standard.string(forKey:"gold.notes."+game.id) ?? ""
        }
    }
    @MainActor private func send() async {
        sending=true;error=""
        defer {sending=false}
        do {
            guard let result else {return}
            let draft=GoldReportDevice.draft(game,result:result,notes:notes,fps:fps)
            receipt=try await LSX4PublicReportTransport.shared.send(draft)
            if result == .playable {UserDefaults.standard.set("supported",forKey:"gold.status."+game.id)}
            if result == .unplayable {UserDefaults.standard.set("unsupported",forKey:"gold.status."+game.id)}
            UserDefaults.standard.set(notes,forKey:"gold.notes."+game.id)
        } catch {self.error=error.localizedDescription}
    }
}

struct GoldUserStatusIcon:View {
    @AppStorage private var status:String
    init(id:String) {_status=AppStorage(wrappedValue:"","gold.status."+id)}
    var body:some View {
        if status=="supported" || status=="unsupported" {
            Image(systemName:status=="supported" ? "checkmark.circle.fill" : "xmark.circle.fill")
                .font(.system(size:16,weight:.semibold)).foregroundStyle(status=="supported" ? .green : .red)
                .background(.black.opacity(0.65),in:Circle()).padding(3).allowsHitTesting(false)
        }
    }
}
