// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Combine

struct LSX4CommunityReport:Codable,Identifiable,Equatable {
    struct Media:Codable,Equatable,Identifiable {
        var url:String,kind:String,mime:String,size:Int
        var id:String {url}
    }
    struct Hardware:Codable,Equatable {
        var machine:String?,modelName:String?,gpu:String?,cpuCores:Int?,memoryBytes:Int64?,screenWidth:Int?,screenHeight:Int?,renderResolution:String?
    }
    var id:String,gameTitle:String,titleId:String,platform:String,device:String,appVersion:String,result:String,notes:String,createdAt:String
    var operatingSystem:String?,osVersion:String?,androidVersion:String?,executionMode:String?,updatedAt:String?,reportSource:String?,video:String?
    var fps:Double?,hardware:Hardware?,media:[Media]?
    var author:LSX4Member?
    var isIOSPlayable:Bool {operatingSystem == "iOS" && result == "playable"}
}

enum LSX4ReportStatus {
    static var currentVersion:String {
        Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String
            ?? Bundle.main.object(forInfoDictionaryKey:"CFBundleShortVersionString") as? String ?? ""
    }
    private static func components(_ value:String)->[UInt64]? {
        var value=value.trimmingCharacters(in:.whitespacesAndNewlines)
        if value.hasPrefix("v") || value.hasPrefix("V") {value.removeFirst()}
        guard value.range(of:"^[0-9]+(?:\\.[0-9]+)*$",options:.regularExpression) != nil else {return nil}
        let parts=value.split(separator:".").compactMap {UInt64($0)}
        return parts.count == value.split(separator:".").count ? parts : nil
    }
    static func isOlder(_ version:String,than current:String)->Bool {
        guard let left=components(version),let right=components(current) else {return false}
        for i in 0..<max(left.count,right.count) {
            let a=i<left.count ? left[i] : 0,b=i<right.count ? right[i] : 0
            if a != b {return a<b}
        }
        return false
    }
    static func isOldUnplayable(_ report:LSX4CommunityReport,current:String=currentVersion)->Bool {
        report.operatingSystem == "iOS" && report.result == "not-playable" && isOlder(report.appVersion,than:current)
    }
    private static func gameKey(_ report:LSX4CommunityReport)->String? {
        let title=report.titleId.trimmingCharacters(in:.whitespacesAndNewlines).uppercased()
        guard report.operatingSystem == "iOS",!title.isEmpty else {return nil}
        return report.platform.uppercased()+":"+title
    }
    static func conflictingGames(_ reports:[LSX4CommunityReport])->Set<String> {
        var results=[String:UInt8]()
        for report in reports {
            guard let key=gameKey(report) else {continue}
            if report.result == "playable" {results[key,default:0] |= 1}
            else if report.result == "not-playable" {results[key,default:0] |= 2}
        }
        return Set(results.compactMap {$0.value == 3 ? $0.key : nil})
    }
    static func hasConflict(_ report:LSX4CommunityReport,in games:Set<String>)->Bool {
        guard let key=gameKey(report) else {return false};return games.contains(key)
    }
}

enum LSX4ReportOwnership {
    private static var namespace:String {LSX4CommunityAPI.endpoint.path == "/api/community-reports" ? "" : ".fixture"}
    static var receiptKey:String {"lsx4.publicReport.receipts"+namespace}
    static var mappingKey:String {"lsx4.publicReport.games"+namespace}
    static func key(_ id:String)->String? {(UserDefaults.standard.dictionary(forKey:receiptKey) as? [String:String])?[id]}
    static func remember(_ receipt:LSX4PublicReportReceipt,game:String?=nil) {
        var values=UserDefaults.standard.dictionary(forKey:receiptKey) as? [String:String] ?? [:]
        values[receipt.id]=receipt.removalKey;UserDefaults.standard.set(values,forKey:receiptKey)
        if let game {var games=UserDefaults.standard.dictionary(forKey:mappingKey) as? [String:String] ?? [:];games[game]=receipt.id;UserDefaults.standard.set(games,forKey:mappingKey)}
    }
    static func reportID(game:String)->String? {(UserDefaults.standard.dictionary(forKey:mappingKey) as? [String:String])?[game]}
    static func forget(game:String) {var games=UserDefaults.standard.dictionary(forKey:mappingKey) as? [String:String] ?? [:];games.removeValue(forKey:game);UserDefaults.standard.set(games,forKey:mappingKey)}
}

final class LSX4CommunityAPI:NSObject,URLSessionTaskDelegate {
    static let shared=LSX4CommunityAPI()
    // Optional isolated test configuration is never included in a distribution archive.
    static let endpoint:URL = {
        if let file=Bundle.main.url(forResource:"CommunityConfig",withExtension:"plist"),
           let data=try? Data(contentsOf:file),let value=try? PropertyListSerialization.propertyList(from:data,format:nil) as? [String:String],
           let raw=value["endpoint"],let url=URL(string:raw),url.scheme == "https",url.host == "lsx45.app",
           url.path.range(of:"^/_ios-sync-[a-f0-9]{32}/api/community-reports$",options:.regularExpression) != nil {return url}
        return URL(string:"https://lsx45.app/api/community-reports")!
    }()
    static var fixture:Bool {endpoint.path != "/api/community-reports"}
    static var consentKey:String {"lsx4.publicReport.quickConsent.v1"+(fixture ? ".fixture" : "")}
    private let configuration:URLSessionConfiguration
    init(configuration:URLSessionConfiguration = .ephemeral) {self.configuration=configuration;super.init()}
    lazy var session:URLSession={
        let config=configuration;config.httpCookieStorage=nil;config.urlCache=nil
        config.timeoutIntervalForRequest=30;config.timeoutIntervalForResource=360
        return URLSession(configuration:config,delegate:self,delegateQueue:nil)
    }()
    struct Envelope:Decodable {var report:LSX4CommunityReport}
    struct Feed:Decodable {var reports:[LSX4CommunityReport],cursor:String?,enabled:Bool}
    struct HTTPFailure:LocalizedError {
        var status:Int,message:String
        var errorDescription:String? {message}
    }
    func urlSession(_ session:URLSession,task:URLSessionTask,willPerformHTTPRedirection response:HTTPURLResponse,newRequest request:URLRequest,completionHandler:@escaping(URLRequest?)->Void) {completionHandler(nil)}
    private func request(_ path:String="",method:String="GET")->URLRequest {
        var request=URLRequest(url:path.isEmpty ? Self.endpoint : Self.endpoint.appendingPathComponent(path),cachePolicy:.reloadIgnoringLocalCacheData)
        request.httpMethod=method;request.setValue("https://lsx45.app",forHTTPHeaderField:"Origin")
        request.setValue("no-cache",forHTTPHeaderField:"Cache-Control");return request
    }
    func checked(_ data:Data,_ response:URLResponse,maximum:Int=2_000_000)throws->Data {
        guard data.count<=maximum else {throw HTTPFailure(status:0,message:P("report_error_size","The server response is too large. Please retry."))}
        let code=(response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200...299).contains(code) else {
            let message:String
            switch code {
            case 401:message=P("account_credentials_error","Check your login and password, or sign in again.")
            case 403:message=P("report_error_owner","Only the publishing device can change these attachments.")
            case 409 where response.url?.path.hasPrefix("/api/account/") == true:message=P("account_taken","This login is already taken.")
            case 409:message=P("report_error_duplicate","A report already exists. Check the shared list before retrying.")
            case 429:message=P("report_error_rate","Please limit new reports to five per day. Existing reports can still be updated.")
            case 413:message=P("report_error_limits","Use a screenshot up to 8 MB or a video up to 100 MB.")
            case 503:message=P("report_error_unavailable","Community reports are temporarily unavailable.")
            default:message=P("report_error_server","The server could not confirm this change. Try again.")
            }
            throw HTTPFailure(status:code,message:message)
        };return data
    }
    func playable() async throws->[LSX4CommunityReport] {try await reports(result:"playable")}
    func reports(result:String="",user:String="") async throws->[LSX4CommunityReport] {
        var reports=[LSX4CommunityReport](),cursor:String?,seen=Set<String>()
        repeat {
            try Task.checkCancellation()
            var components=URLComponents(url:Self.endpoint,resolvingAgainstBaseURL:false)!
            components.queryItems=[URLQueryItem(name:"limit",value:"24")]
            if user.isEmpty {components.queryItems!.append(URLQueryItem(name:"os",value:"iOS"))}
            else {components.queryItems!.append(URLQueryItem(name:"user",value:user))}
            if !result.isEmpty {components.queryItems!.append(URLQueryItem(name:"result",value:result))}
            if let cursor {components.queryItems!.append(URLQueryItem(name:"cursor",value:cursor))}
            var query=request();query.url=components.url
            let (data,response)=try await session.data(for:query)
            let page=try JSONDecoder().decode(Feed.self,from:checked(data,response))
            guard page.enabled else {throw HTTPFailure(status:503,message:P("report_error_unavailable","Community reports are temporarily unavailable."))}
            reports += page.reports.filter {seen.insert($0.id).inserted}
            if let next=page.cursor,next == cursor {throw HTTPFailure(status:0,message:P("report_error_pages","The shared list could not be loaded. Please retry."))}
            cursor=page.cursor
        } while cursor != nil
        return reports
    }
    func cancelSubmissions() {
        session.getAllTasks {tasks in for task in tasks where ["POST","PUT","DELETE"].contains(task.originalRequest?.httpMethod ?? "") {task.cancel()}}
    }
    func one(_ id:String) async throws->LSX4CommunityReport {
        let (data,response)=try await session.data(for:request(id))
        return try JSONDecoder().decode(Envelope.self,from:checked(data,response,maximum:32768)).report
    }
    func submit(_ draft:LSX4PublicReportDraft,existing:String?=nil) async throws->(LSX4PublicReportReceipt,LSX4CommunityReport) {
        try LSX4ReportSharing.requireEnabled()
        var query=request(existing ?? "",method:existing == nil ? "POST" : "PUT")
        var payload=try JSONSerialization.jsonObject(with:draft.payload()) as! [String:Any]
        if let existing {
            let current=try await one(existing)
            payload["video"]=current.video ?? ""
            guard LSX4AccountCredentials.owns(current) else {throw HTTPFailure(status:403,message:P("report_error_owner","Only the report owner can edit it."))}
            if current.author != nil,let token=LSX4AccountCredentials.token {payload["deleteToken"]=token}
            else {payload["deleteToken"]=LSX4ReportOwnership.key(existing)}
        } else if let token=LSX4AccountCredentials.token {
            query.setValue("Bearer "+token,forHTTPHeaderField:"Authorization")
        }
        query.setValue("application/json; charset=utf-8",forHTTPHeaderField:"Content-Type")
        query.httpBody=try JSONSerialization.data(withJSONObject:payload)
        let (data,response)=try await session.data(for:query)
        let checked=try checked(data,response,maximum:32768)
        let receipt=try LSX4PublicReportReceipt(data:checked)
        let report=try JSONDecoder().decode(Envelope.self,from:checked).report
        if report.author == nil {LSX4ReportOwnership.remember(receipt,game:draft.titleID)}
        return (receipt,report)
    }
    func attachment(_ id:String,kind:String,file:URL?,mime:String="") async throws->LSX4CommunityReport {
        try LSX4ReportSharing.requireEnabled()
        let current=try await one(id)
        let editingKey=current.author == nil ? LSX4ReportOwnership.key(id) : LSX4AccountCredentials.token
        guard LSX4AccountCredentials.owns(current),["screenshot","screencast"].contains(kind),let key=editingKey else {throw HTTPFailure(status:403,message:P("report_error_owner","Only the publishing device can change these attachments."))}
        var query=request(id+"/media/"+kind,method:file == nil ? "DELETE" : "PUT")
        query.setValue("Bearer "+key,forHTTPHeaderField:"Authorization")
        let data:Data,response:URLResponse
        if let file {
            let bytes=try file.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? 0
            guard bytes>=16,bytes <= (kind == "screenshot" ? 8 : 100)*1024*1024 else {throw HTTPFailure(status:413,message:P("report_error_limits","Use a screenshot up to 8 MB or a video up to 100 MB."))}
            query.setValue(mime,forHTTPHeaderField:"Content-Type");query.setValue(String(bytes),forHTTPHeaderField:"Content-Length")
            (data,response)=try await session.upload(for:query,fromFile:file)
        } else {(data,response)=try await session.data(for:query)}
        _=try checked(data,response,maximum:32768)
        return try await one(id)
    }
    static func mediaURL(_ media:LSX4CommunityReport.Media)->URL? {
        guard media.url.range(of:"^/community-media/[a-f0-9]{32}\\.(png|jpg|webp|mp4|webm)$",options:.regularExpression) != nil else {return nil}
        return endpoint.deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent(String(media.url.dropFirst()))
    }
}

@MainActor final class LSX4CommunityCache:ObservableObject {
    static let shared=LSX4CommunityCache()
    @Published private(set) var reports=[LSX4CommunityReport]()
    @Published private(set) var loading=false
    @Published private(set) var error=""
    @Published private(set) var updated:Date?
    private var generation=0
    private var inFlight=Set<String>()
    private let file:URL
    private let fetch:() async throws->[LSX4CommunityReport]
    init(file:URL?=nil,fetch:(() async throws->[LSX4CommunityReport])?=nil) {
        self.fetch=fetch ?? {try await LSX4CommunityAPI.shared.reports()}
        self.file=file ?? FileManager.default.urls(for:.applicationSupportDirectory,in:.userDomainMask)[0].appendingPathComponent(LSX4CommunityAPI.fixture ? "community-ios-fixture.json" : "community-ios-playable.json")
        if let data=try? Data(contentsOf:self.file),let cached=try? JSONDecoder().decode([LSX4CommunityReport].self,from:data) {reports=cached.filter {$0.operatingSystem == "iOS"}}
    }
    func refresh() async {
        generation += 1;let ticket=generation;loading=true;error=""
        do {
            let fetched=try await fetch()
            guard ticket == generation,!Task.isCancelled else {return}
            reports=fetched;updated=Date();save();loading=false
        } catch {
            guard ticket == generation else {return};loading=false
            if !(error is CancellationError) {self.error=P("community_saved","Showing saved reports.")+" "+error.localizedDescription}
        }
    }
    func accept(_ report:LSX4CommunityReport) {
        generation += 1
        let oldIndex=reports.firstIndex {$0.id == report.id}
        var next=reports.filter {$0.id != report.id}
        if report.operatingSystem == "iOS" {next.insert(report,at:min(oldIndex ?? 0,next.count))}
        if loading {loading=false}
        if next != reports {reports=next;updated=Date();save()}
    }
    private func save() {
        try? FileManager.default.createDirectory(at:file.deletingLastPathComponent(),withIntermediateDirectories:true)
        if let data=try? JSONEncoder().encode(reports) {try? data.write(to:file,options:.atomic)}
    }
    func publish(_ draft:LSX4PublicReportDraft) async throws->LSX4PublicReportReceipt {
        guard inFlight.insert(draft.titleID).inserted else {throw LSX4CommunityAPI.HTTPFailure(status:0,message:P("report_error_sending","A report for this game is already being sent."))}
        defer {inFlight.remove(draft.titleID)}
        var existing=reports.first(where:{$0.titleId == draft.titleID && LSX4AccountCredentials.owns($0)})?.id
        if existing == nil,let remembered=LSX4ReportOwnership.reportID(game:draft.titleID) {
            do {let current=try await LSX4CommunityAPI.shared.one(remembered);if LSX4AccountCredentials.owns(current) {existing=remembered}}
            catch let failure as LSX4CommunityAPI.HTTPFailure where failure.status == 404 {LSX4ReportOwnership.forget(game:draft.titleID)}
        }
        do {
            let (receipt,report)=try await LSX4CommunityAPI.shared.submit(draft,existing:existing)
            accept(report);return receipt
        } catch let error as LSX4CommunityAPI.HTTPFailure where error.status == 404 && existing != nil {
            LSX4ReportOwnership.forget(game:draft.titleID)
            let (receipt,report)=try await LSX4CommunityAPI.shared.submit(draft);accept(report);return receipt
        }
    }
}
