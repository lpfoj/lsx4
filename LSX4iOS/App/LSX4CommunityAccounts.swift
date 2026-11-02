import Foundation
import Security
import SwiftUI
import PhotosUI
import UIKit
import ImageIO

private struct GoldCommunityPageStyle:ViewModifier {
    @AppStorage("gold.theme") private var storedTheme=0
    private var theme:GoldTheme {GoldTheme(rawValue:storedTheme) ?? .azure}
    func body(content:Content)->some View {
        content.background {GoldBackdrop(theme:theme)}.foregroundStyle(.white).preferredColorScheme(.dark)
            .tint(theme.accent).toolbarBackground(.hidden,for:.navigationBar)
    }
}
extension View {func goldCommunityPage()->some View {modifier(GoldCommunityPageStyle())}}

struct LSX4Member:Codable,Identifiable,Equatable {
    let id:String,username:String,joinedAt:String,appSince:String
    var avatar:String?,supporter:Bool,reportCount:Int
    var clientPlatform:String?
    var clientPlatforms:[String]?
    static func platformName(_ value:String)->String? {
        switch value.lowercased() {
        case "ios","iphone","ipad":return "iOS"
        case "android":return "Android"
        case "mac","macos":return "macOS"
        default:return nil
        }
    }
    var registrationSources:[String] {
        let values=clientPlatforms ?? clientPlatform.map {[$0]} ?? []
        var seen=Set<String>()
        return values.compactMap(Self.platformName).filter {seen.insert($0).inserted}
    }
    var registrationSource:String? {registrationSources.first}
}

struct GoldMemberPlatformIcon:View {
    let user:LSX4Member
    var body:some View {
        HStack(spacing:4) {
            if user.registrationSources.isEmpty {
                Image(systemName:"questionmark.circle").font(.system(size:14)).foregroundStyle(.secondary)
                    .frame(width:17,height:17).accessibilityLabel(P("member_platform_unknown","Registration app not recorded"))
            }
            ForEach(user.registrationSources,id:\.self) {platform in
            Group {
                if platform == "Android" {
                    Canvas {context,size in
                        let sx=size.width/24,sy=size.height/24
                        var head=Path();head.move(to:CGPoint(x:3*sx,y:17*sy));head.addLine(to:CGPoint(x:3*sx,y:14*sy))
                        head.addArc(center:CGPoint(x:12*sx,y:14*sy),radius:9*sx,startAngle:.degrees(180),endAngle:.degrees(0),clockwise:false)
                        head.addLine(to:CGPoint(x:21*sx,y:17*sy));head.closeSubpath()
                        context.fill(head,with:.color(.green))
                        var antenna=Path();antenna.move(to:CGPoint(x:7*sx,y:7*sy));antenna.addLine(to:CGPoint(x:4*sx,y:2*sy));antenna.move(to:CGPoint(x:17*sx,y:7*sy));antenna.addLine(to:CGPoint(x:20*sx,y:2*sy))
                        context.stroke(antenna,with:.color(.green),style:StrokeStyle(lineWidth:1.6*sx,lineCap:.round))
                        for x in [8.0,16.0] {context.fill(Path(ellipseIn:CGRect(x:(x-0.9)*sx,y:11*sy,width:1.8*sx,height:1.8*sy)),with:.color(.black))}
                    }
                } else {Image(systemName:"apple.logo").font(.system(size:14))}
            }.frame(width:17,height:17).accessibilityElement(children:.ignore)
                .accessibilityLabel(P("member_registered_from","Registered via {platform}",["platform":platform]))
                .accessibilityIdentifier("member-platform-"+user.id+"-"+platform)
            }
        }.accessibilityElement(children:.contain).accessibilityIdentifier("member-platform-"+user.id)
    }
}

enum LSX4AccountCredentials {
#if targetEnvironment(simulator)
    static var previewValues:[String:String]?
#endif
    private static var service:String {(Bundle.main.bundleIdentifier ?? "LSX4")+".community"+(LSX4CommunityAPI.fixture ? ".fixture" : "")}
    private static var query:[String:Any] {[kSecClass as String:kSecClassGenericPassword,kSecAttrService as String:service,kSecAttrAccount as String:"session"]}
    static var value:[String:String] {
#if targetEnvironment(simulator)
        if let previewValues {return previewValues}
#endif
        var lookup=query;lookup[kSecReturnData as String]=true;lookup[kSecMatchLimit as String]=kSecMatchLimitOne
        var result:CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary,&result) == errSecSuccess,let data=result as? Data,
              let object=try? JSONDecoder().decode([String:String].self,from:data) else {return [:]}
        return object
    }
    static var token:String? {value["token"]}
    static var userID:String? {value["id"]}
    static func save(token:String,user:LSX4Member)throws {
#if targetEnvironment(simulator)
        if previewValues != nil {previewValues=["token":token,"id":user.id];return}
#endif
        let data=try JSONEncoder().encode(["token":token,"id":user.id])
        let result=SecItemUpdate(query as CFDictionary,[kSecValueData as String:data] as CFDictionary)
        if result == errSecItemNotFound {
            var item=query;item[kSecValueData as String]=data;item[kSecAttrAccessible as String]=kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            guard SecItemAdd(item as CFDictionary,nil) == errSecSuccess else {throw secureStorageError}
        } else if result != errSecSuccess {throw secureStorageError}
    }
    private static var secureStorageError:LSX4CommunityAPI.HTTPFailure {.init(status:0,message:P("account_secure_error","Could not save sign-in securely. Please try again."))}
    static func clear() {
#if targetEnvironment(simulator)
        if previewValues != nil {previewValues=[:];return}
#endif
        SecItemDelete(query as CFDictionary)
    }
    static func owns(_ report:LSX4CommunityReport)->Bool {
        if let author=report.author {return author.id == userID}
        return LSX4ReportOwnership.key(report.id) != nil
    }
}

enum LSX4AccountsAPI {
    struct UserEnvelope:Decodable {var user:LSX4Member}
    struct SessionEnvelope:Decodable {var user:LSX4Member,token:String}
    struct Users:Decodable {var users:[LSX4Member],cursor:String?}
    struct Claim:Decodable {var user:LSX4Member,claimed:[String],rejected:[String]}
    static var root:URL {LSX4CommunityAPI.endpoint.deletingLastPathComponent().deletingLastPathComponent()}
    static func url(_ path:String)->URL {
        let split=path.split(separator:"?",maxSplits:1)
        var parts=URLComponents(url:root.appendingPathComponent(String(split[0])),resolvingAgainstBaseURL:false)!
        if split.count>1 {parts.percentEncodedQuery=String(split[1])}
        return parts.url!
    }
    static func avatarURL(_ path:String?)->URL? {
        guard let path,path.range(of:"^/community-avatars/[a-f0-9]{32}\\.jpg$",options:.regularExpression) != nil else {return nil}
        return url(String(path.dropFirst()))
    }
    static func request<T:Decodable>(_ path:String,method:String="GET",body:[String:Any]?=nil,authenticated:Bool=false) async throws->T {
        var query=URLRequest(url:url(path),cachePolicy:.reloadIgnoringLocalCacheData)
        query.httpMethod=method;query.timeoutInterval=15
        query.setValue("https://lsx45.app",forHTTPHeaderField:"Origin")
        query.setValue("no-cache",forHTTPHeaderField:"Cache-Control")
        if authenticated {
            guard let token=LSX4AccountCredentials.token else {throw LSX4CommunityAPI.HTTPFailure(status:401,message:P("account_sign_in_again","Please sign in again."))}
            query.setValue("Bearer "+token,forHTTPHeaderField:"Authorization")
        }
        if let body {query.httpBody=try JSONSerialization.data(withJSONObject:body);query.setValue("application/json",forHTTPHeaderField:"Content-Type")}
        let (data,response)=try await LSX4CommunityAPI.shared.session.data(for:query)
        return try JSONDecoder().decode(T.self,from:LSX4CommunityAPI.shared.checked(data,response))
    }
    static func users() async throws->[LSX4Member] {
        var users=[LSX4Member](),cursor:String?,seen=Set<String>()
        repeat {
            try Task.checkCancellation()
            let page:Users=try await request("api/users"+(cursor.map {"?cursor="+$0} ?? ""))
            users += page.users.filter {seen.insert($0.id).inserted}
            guard page.cursor != cursor || cursor == nil else {throw URLError(.badServerResponse)}
            cursor=page.cursor
        } while cursor != nil
        return users
    }
    static func reports(user:String) async throws->[LSX4CommunityReport] {
        try await LSX4CommunityAPI.shared.reports(user:user)
    }
}

@MainActor final class LSX4AccountSession:ObservableObject {
    static let shared=LSX4AccountSession()
    @Published var user:LSX4Member?
    @Published var busy=false
    @Published var message=""
    private(set) var claimsPending=false
    func refresh() async {
        guard LSX4AccountCredentials.token != nil else {user=nil;return}
        do {
            let result:LSX4AccountsAPI.UserEnvelope=try await LSX4AccountsAPI.request("api/account/me",authenticated:true)
            user=result.user
            try await claimLegacy()
        } catch let failure as LSX4CommunityAPI.HTTPFailure where failure.status == 401 {
            LSX4AccountCredentials.clear();user=nil;message=failure.localizedDescription
        } catch {message=error.localizedDescription}
    }
    func signIn(username:String,password:String,register:Bool) async {
        guard !busy else {return};busy=true;message="";defer {busy=false}
        guard username.range(of:"^[A-Za-z0-9][A-Za-z0-9_.-]{2,31}$",options:.regularExpression) != nil,
              (10...128).contains(password.count) else {message=P("account_rules","Use a login of 3–32 characters and a password of 10–128 characters.");return}
        do {
            var payload:[String:Any]=["username":username,"password":password]
            if register {payload["clientPlatform"]="ios"}
            let result:LSX4AccountsAPI.SessionEnvelope=try await LSX4AccountsAPI.request("api/account/"+(register ? "register" : "login"),method:"POST",body:payload)
            try LSX4AccountCredentials.save(token:result.token,user:result.user);user=result.user
            do {try await claimLegacy()} catch {message=P("account_claim_retry","Signed in. Earlier reports will be linked when you retry.")}
        } catch {message=error.localizedDescription}
    }
    func claimLegacy() async throws {
        let keys=UserDefaults.standard.dictionary(forKey:LSX4ReportOwnership.receiptKey) as? [String:String] ?? [:]
        let entries=Array(keys);claimsPending = !entries.isEmpty
        for offset in stride(from:0,to:entries.count,by:24) {
            let batch=entries[offset..<min(offset+24,entries.count)].map {["id":$0.key,"deleteToken":$0.value]}
            let claim:LSX4AccountsAPI.Claim=try await LSX4AccountsAPI.request("api/account/claim-reports",method:"POST",body:["reports":batch],authenticated:true)
            user=claim.user
        }
        claimsPending=false
    }
    func signOut() async {
        struct Result:Decodable {let signedOut:Bool}
        busy=true;defer {busy=false}
        do {let _:Result=try await LSX4AccountsAPI.request("api/account/logout",method:"POST",body:[:],authenticated:true)
            LSX4AccountCredentials.clear();user=nil;message=""
        } catch {message=error.localizedDescription}
    }
    func avatar(_ item:PhotosPickerItem) async {
        busy=true;message="";defer {busy=false}
        do {
            guard let data=try await item.loadTransferable(type:Data.self) else {throw URLError(.cannotDecodeContentData)}
            try await uploadAvatar(data)
        } catch {message=error.localizedDescription}
    }
    func fixtureAvatar() async {
        guard LSX4CommunityAPI.fixture,!busy else {return}
        busy=true;message="";defer {busy=false}
        do {
            guard let file=Bundle.main.url(forResource:"CommunityFixtureImage",withExtension:"png") else {throw CocoaError(.fileNoSuchFile)}
            try await uploadAvatar(Data(contentsOf:file))
        } catch {message=error.localizedDescription}
    }
    private func uploadAvatar(_ data:Data) async throws {
            guard data.count<=32*1024*1024,let source=CGImageSourceCreateWithData(data as CFData,nil),
                  let thumbnail=CGImageSourceCreateThumbnailAtIndex(source,0,[kCGImageSourceCreateThumbnailFromImageAlways:true,kCGImageSourceCreateThumbnailWithTransform:true,kCGImageSourceThumbnailMaxPixelSize:512] as CFDictionary) else {throw URLError(.cannotDecodeContentData)}
            let clean=UIImage(cgImage:thumbnail)
            guard let jpeg=clean.jpegData(compressionQuality:0.88),jpeg.count<=2*1024*1024,let token=LSX4AccountCredentials.token else {throw URLError(.dataLengthExceedsMaximum)}
            var query=URLRequest(url:LSX4AccountsAPI.url("api/account/avatar"));query.httpMethod="PUT"
            query.setValue("https://lsx45.app",forHTTPHeaderField:"Origin");query.setValue("Bearer "+token,forHTTPHeaderField:"Authorization")
            query.setValue("image/jpeg",forHTTPHeaderField:"Content-Type")
            let (output,response)=try await LSX4CommunityAPI.shared.session.upload(for:query,from:jpeg)
            user=try JSONDecoder().decode(LSX4AccountsAPI.UserEnvelope.self,from:LSX4CommunityAPI.shared.checked(output,response)).user
            message=P("account_avatar_done","Avatar updated. Everyone can see it on your profile.")
    }
}

struct GoldMemberAvatar:View {
    let user:LSX4Member
    var size:CGFloat=52
    var body:some View {
        ZStack(alignment:.bottomTrailing) {
            AsyncImage(url:LSX4AccountsAPI.avatarURL(user.avatar)) {phase in
                if let image=phase.image {image.resizable().scaledToFill()}
                else {ZStack {Color.white.opacity(0.08);Text(String(user.username.prefix(1)).uppercased()).font(.system(size:size*0.42,weight:.medium))}}
            }.frame(width:size,height:size).clipShape(Circle())
            if user.supporter {Image(systemName:"crown.fill").foregroundStyle(.yellow).font(.system(size:size*0.28)).padding(3).background(.black.opacity(0.75),in:Circle()).accessibilityLabel(P("member_supporter","Project supporter"))}
        }
    }
}

struct GoldAccountView:View {
    @ObservedObject private var account=LSX4AccountSession.shared
    @State private var username=""
    @State private var password=""
    @State private var register=false
    @State private var avatar:PhotosPickerItem?
    var body:some View {
        ScrollView {VStack(alignment:.leading,spacing:18) {
            if let user=account.user {
                HStack(spacing:16) {GoldMemberAvatar(user:user,size:76);VStack(alignment:.leading,spacing:5) {Text(user.username).font(.title2);Text(P("account_signed_in","You are signed in")).foregroundStyle(.secondary)}}
                PhotosPicker(selection:$avatar,matching:.images) {Label(P("account_avatar","Change avatar"),systemImage:"person.crop.circle.badge.plus")}.disabled(account.busy).accessibilityIdentifier("account-avatar")
                if LSX4CommunityAPI.fixture {Button("Upload isolated avatar fixture") {Task {await account.fixtureAvatar()}}.disabled(account.busy).accessibilityIdentifier("account-fixture-avatar")}
                NavigationLink {GoldMemberProfile(user:user)} label: {Label(P("account_profile","My public profile"),systemImage:"person.crop.circle")}.accessibilityIdentifier("account-profile")
                NavigationLink {GoldMemberReports(user:user,mine:true)} label: {Label(P("account_reports","My reports"),systemImage:"text.bubble")}.accessibilityIdentifier("account-my-reports")
                Button(P("account_claim","Link earlier reports")) {Task {do {try await account.claimLegacy();account.message=P("account_linked","Earlier reports are linked to this account.")} catch {account.message=error.localizedDescription}}}.disabled(account.busy)
                Button(P("account_sign_out","Sign out"),role:.destructive) {Task {await account.signOut()}}.disabled(account.busy).accessibilityIdentifier("account-sign-out")
            } else {
                Text(P("account_intro","Use a login and password to keep your profile and reports together. Everyone can browse members without signing in.")).font(.body)
                Picker(P("account_title","Account"),selection:$register) {Text(P("account_sign_in","Sign in")).tag(false);Text(P("account_register","Create account")).tag(true)}.pickerStyle(.segmented)
                TextField(P("account_login","Login"),text:$username).textContentType(.username).keyboardType(.asciiCapable).textInputAutocapitalization(.never).autocorrectionDisabled().textFieldStyle(.roundedBorder).accessibilityIdentifier("account-login")
                SecureField(P("account_password","Password"),text:$password).textContentType(register ? .newPassword : .password).keyboardType(.asciiCapable).textInputAutocapitalization(.never).autocorrectionDisabled().textFieldStyle(.roundedBorder).accessibilityIdentifier("account-password")
                if register {Text(P("account_rules","Login: 3–32 letters, numbers, _, . or -. Password: 10–128 characters. Your login and reports are public. No email is required.")).font(.footnote).foregroundStyle(.secondary)}
                Button(register ? P("account_register","Create account") : P("account_sign_in","Sign in")) {Task {await account.signIn(username:username,password:password,register:register);password=""}}.buttonStyle(.borderedProminent).disabled(account.busy || username.isEmpty || password.isEmpty).accessibilityIdentifier("account-submit")
            }
            NavigationLink {GoldMemberDirectory()} label: {Label(P("members_title","All users"),systemImage:"person.2")}.accessibilityIdentifier("account-all-users")
            if account.busy {ProgressView()}
            if !account.message.isEmpty {Text(account.message).font(.footnote).accessibilityIdentifier("account-status")}
        }.padding(20).frame(maxWidth:680,alignment:.leading).frame(maxWidth:.infinity)}
        .goldCommunityPage().navigationTitle(P("account_title","Account")).task {await account.refresh()}
        .onChange(of:avatar) {_,item in if let item {Task {await account.avatar(item);avatar=nil}}}
    }
}

struct GoldMemberDirectory:View {
    private let fetchUsers:() async throws->[LSX4Member]
    init(fetchUsers:@escaping () async throws->[LSX4Member]={try await LSX4AccountsAPI.users()}) {self.fetchUsers=fetchUsers}
    @State private var users=[LSX4Member]()
    @State private var search=""
    @State private var error=""
    @State private var loading=false
    private var filtered:[LSX4Member] {users.filter {search.isEmpty || $0.username.localizedCaseInsensitiveContains(search)}}
    var body:some View {
        ScrollView {LazyVStack(alignment:.leading,spacing:12) {
            Text(P("members_public","All profiles and published reports are visible to everyone.")).font(.footnote).foregroundStyle(.secondary)
            TextField(P("members_search","Search users"),text:$search).padding(12).goldGlass(radius:12)
            if loading {ProgressView()}
            if !error.isEmpty {Text(error).font(.footnote);Button(P("thanks_retry","Try again")) {Task {await refresh()}}}
            if users.isEmpty && !loading && error.isEmpty {Text(P("members_empty","No registered users yet")).foregroundStyle(.secondary).frame(maxWidth:.infinity,minHeight:180).accessibilityIdentifier("members-empty")}
            ForEach(filtered) {user in
                NavigationLink {GoldMemberProfile(user:user)} label: {
                    HStack(spacing:14) {GoldMemberAvatar(user:user);VStack(alignment:.leading,spacing:4) {HStack(spacing:6) {GoldMemberPlatformIcon(user:user);Text(user.username).font(.headline)};Text(P("member_reports_count","{count} reports",["count":String(user.reportCount)])).font(.caption).foregroundStyle(.secondary)};Spacer();Image(systemName:"chevron.right").font(.caption)}.padding(14).goldGlass(radius:18).contentShape(Rectangle())
                }.buttonStyle(.plain).accessibilityIdentifier("member-"+user.id)
            }
        }.padding(20).frame(maxWidth:900).frame(maxWidth:.infinity)}
        .goldCommunityPage().navigationTitle(P("members_title","All users")).task {await refresh()}.refreshable {await refresh()}
    }
    @MainActor private func refresh() async {loading=true;error="";defer {loading=false};do {users=try await fetchUsers()} catch {self.error=error.localizedDescription}}
}

struct GoldMemberProfile:View {
    @State var user:LSX4Member
    @State private var message=""
    var body:some View {
        ScrollView {VStack(alignment:.leading,spacing:20) {
            HStack(spacing:18) {GoldMemberAvatar(user:user,size:88);VStack(alignment:.leading,spacing:6) {HStack(spacing:8) {GoldMemberPlatformIcon(user:user);Text(user.username).font(.title)};if user.supporter {Label(P("member_supporter","Project supporter"),systemImage:"crown.fill").foregroundStyle(.yellow)}}}
            VStack(alignment:.leading,spacing:12) {
                Text(P("member_joined","Joined {date}",["date":date(user.joinedAt)]))
                Text(P("member_since","Reports since {date}",["date":date(user.appSince)]))
                Text(P("member_reports_count","{count} reports",["count":String(user.reportCount)])).font(.title2)
            }.frame(maxWidth:.infinity,alignment:.leading).padding(18).goldGlass(radius:20)
            NavigationLink {GoldMemberReports(user:user,mine:user.id == LSX4AccountCredentials.userID)} label: {Label(P("member_view_reports","View reports"),systemImage:"text.bubble")}
            if !message.isEmpty {Text(message).font(.footnote)}
        }.padding(20).frame(maxWidth:800).frame(maxWidth:.infinity)}
        .goldCommunityPage().navigationTitle(user.username).task {
            do {let result:LSX4AccountsAPI.UserEnvelope=try await LSX4AccountsAPI.request("api/users/"+user.id);user=result.user} catch {message=error.localizedDescription}
        }
    }
    private func date(_ value:String)->String {
        let parser=ISO8601DateFormatter();parser.formatOptions=[.withInternetDateTime,.withFractionalSeconds]
        guard let date=parser.date(from:value) else {return String(value.prefix(10))}
        let formatter=DateFormatter();formatter.locale=Locale(identifier:LSX4AppText.language);formatter.dateStyle = .medium;formatter.timeStyle = .none
        return formatter.string(from:date)
    }
}

struct GoldMemberReports:View {
    let user:LSX4Member
    var mine=false
    @State private var reports=[LSX4CommunityReport]()
    @State private var error=""
    @State private var loading=false
    var body:some View {
        ScrollView {LazyVStack(alignment:.leading,spacing:12) {
            if loading {ProgressView()}
            if !error.isEmpty {Text(error)}
            if reports.isEmpty && !loading {Text(P("reports_empty","No reports yet")).foregroundStyle(.secondary).frame(maxWidth:.infinity,minHeight:180)}
            ForEach(reports) {report in NavigationLink {GoldCommunityReportDetail(report:report)} label: {GoldReportSummary(report:report)}.buttonStyle(.plain)}
        }.padding(20).frame(maxWidth:900).frame(maxWidth:.infinity)}
        .goldCommunityPage().navigationTitle(mine ? P("account_reports","My reports") : user.username)
        .task {await refresh()}.refreshable {await refresh()}
    }
    @MainActor private func refresh() async {loading=true;error="";defer {loading=false};do {reports=try await LSX4AccountsAPI.reports(user:user.id)} catch {self.error=error.localizedDescription}}
}

struct GoldReportSummary:View {
    let report:LSX4CommunityReport
    var body:some View {
        HStack(spacing:12) {
            GoldReportGameIcon(report:report)
            VStack(alignment:.leading,spacing:5) {
                Text(report.gameTitle).font(.headline)
                Text(report.device+" · LSX4 "+report.appVersion).font(.caption).foregroundStyle(.secondary)
                GoldReportStatusNotes(report:report)
                if let fps=report.fps {Text(String(format:"%.1f FPS",fps)).font(.caption)}
                if LSX4AccountCredentials.owns(report) {Text(P("report_mine_short","Your report")).font(.caption).foregroundStyle(.mint)}
            }.frame(maxWidth:.infinity,alignment:.leading)
            Image(systemName:"chevron.right").font(.caption)
        }.padding(14).goldGlass(radius:18,selected:LSX4AccountCredentials.owns(report)).contentShape(Rectangle())
    }
}
