// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum LSX4ReportedResult:String,CaseIterable,Identifiable {
    case playable="playable",unplayable="not-playable"
    var id:String {rawValue}
}
struct LSX4PublicReportDraft {
    var title:String,titleID:String,platform:String,device:String,osVersion:String,version:String,executionMode:String
    var result:LSX4ReportedResult?
    var notes="",fps=""
    var consent=false
    var hardware:LSX4CommunityReport.Hardware?=nil
    var source="manual"
    func payload() throws->Data {
        let comment=notes.trimmingCharacters(in:.whitespacesAndNewlines)
        guard consent,let result,comment.count>=10,comment.count<=1200,["PS4","PS5"].contains(platform) else {throw Failure.invalid}
        var value:[String:Any]=["gameTitle":title,"titleId":titleID,"platform":platform,"device":device,
            "operatingSystem":"iOS","osVersion":osVersion,"androidVersion":"","appVersion":version,
            "executionMode":executionMode,"result":result.rawValue,"notes":comment,"publicConsent":true]
        if let hardware,let object=try JSONSerialization.jsonObject(with:JSONEncoder().encode(hardware)) as? [String:Any] {value["hardware"]=object}
        value["reportSource"]=source
        let rate=fps.trimmingCharacters(in:.whitespacesAndNewlines).replacingOccurrences(of:",",with:".")
        if !rate.isEmpty {
            guard rate.range(of:"^\\d{1,3}(?:\\.\\d)?$",options:.regularExpression) != nil,let number=Double(rate),(0...240).contains(number) else {throw Failure.fps}
            value["fps"]=number
        }
        let data=try JSONSerialization.data(withJSONObject:value)
        guard data.count<=8192 else {throw Failure.invalid};return data
    }
    enum Failure:LocalizedError {
        case invalid,fps,receipt,rejected(String)
        var errorDescription:String? {
            switch self {
            case .invalid:return P("report_error_invalid","Choose a result, write 10–1200 characters and agree to publication.")
            case .fps:return P("report_error_fps","FPS must be 0–240, with at most one decimal place.")
            case .receipt:return P("report_error_receipt","The report could not be confirmed. Check the shared list before retrying.")
            case .rejected(let message):return message
            }
        }
    }
}
struct LSX4PublicReportReceipt {
    let id:String,removalKey:String
    init(data:Data) throws {
        guard let value=try JSONSerialization.jsonObject(with:data) as? [String:Any],
              let report=value["report"] as? [String:Any],let id=report["id"] as? String,
              let key=value["deleteToken"] as? String,
              id.range(of:"^\\d{13}-[a-f0-9-]{36}$",options:.regularExpression) != nil,
              key.range(of:"^[a-f0-9]{64}$",options:.regularExpression) != nil else {throw LSX4PublicReportDraft.Failure.receipt}
        self.id=id;removalKey=key
    }
    func save() {LSX4ReportOwnership.remember(self)}
}
final class LSX4PublicReportTransport {
    static let shared=LSX4PublicReportTransport()
    @MainActor func send(_ draft:LSX4PublicReportDraft) async throws->LSX4PublicReportReceipt {
        try await LSX4CommunityCache.shared.publish(draft)
    }
}
