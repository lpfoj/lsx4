// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct GoldSharingSettings:View {
    @AppStorage(LSX4ReportSharing.key) private var enabled=true
    var body:some View {
        Form {
            Section {
                Toggle(P("sharing_toggle","Send Playable / Unplayable reports"),isOn:$enabled).accessibilityIdentifier("report-sharing-toggle")
                Text(P("sharing_instructions","Choose Library view → List. Hold a game for 4 seconds and release for Unplayable, or hold for 5 seconds for Playable. You can also use the game menu. The first public report asks for confirmation.")).accessibilityIdentifier("report-sharing-instructions")
            }
            Section {
                Text(P("sharing_scope","Playable appears in the app and on lsx45.app; Unplayable appears only on the website. Reports include the game, result, device model and specifications, iOS/app version and JIT mode. Optional screenshots and videos are public only when you select them."))
                Text(P("sharing_off_explanation","When sending is off, marks stay on this iPhone and report or attachment changes are not sent. You can still read the shared list. Existing public reports remain available."))
                Text(P("sharing_no_diagnostics","This build does not upload crash reports, diagnostic events, system metrics, logs, game files, saves or pairing files."))
            }.font(.footnote)
        }.accessibilityIdentifier("report-sharing-form").navigationTitle(P("sharing_title","Send data to server"))
            .onChange(of:enabled) {_,value in if !value {LSX4CommunityAPI.shared.cancelSubmissions()}}
    }
}

struct GoldLanguagePicker:View {
    @Binding var language:String
    var body:some View {
        List {
            ForEach(ConsoleLocalization.languages) {choice in
                Button {language=choice.id} label: {
                    HStack(spacing:12) {
                        Text(choice.flag).font(.title2)
                        Text(choice.label).foregroundStyle(.primary)
                        Spacer()
                        if language == choice.id {Image(systemName:"checkmark").foregroundStyle(.tint)}
                    }.contentShape(Rectangle()).padding(.vertical,4)
                }.accessibilityIdentifier("language-choice-"+choice.id)
                    .accessibilityAddTraits(language == choice.id ? .isSelected : [])
            }
        }.accessibilityIdentifier("language-options").navigationTitle(G("language","Language"))
    }
}
