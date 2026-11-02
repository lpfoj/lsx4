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
    var body:some View {GoldPage(title:P("public_report_title","Report a game result")) {GoldReportEditor(game:game)}}
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
