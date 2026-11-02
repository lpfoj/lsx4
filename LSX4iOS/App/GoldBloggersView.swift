import SwiftUI

struct LSX4Credit:Decodable,Identifiable {
    var id:String,name:String
    var url:String?
    var descriptions:[String:String]?
    var description:String {descriptions?[LSX4AppText.language] ?? descriptions?["en"] ?? ""}
    var link:URL? {
        guard let url,let value=URL(string:url),value.scheme == "https",value.host != nil,value.user == nil,value.password == nil else {return nil}
        return value
    }
}
struct GoldBloggersView:View {
    struct Feed:Decodable {let version:Int,people:[LSX4Credit]}
    @State private var people=[LSX4Credit]()
    @State private var loading=false
    @State private var error=""
    var body:some View {
        ScrollView {VStack(alignment:.leading,spacing:20) {
            Text(P("bloggers_intro","Here you can find additional hands-on experience with the emulator to save time testing some games, and discover other emulation communities and related creators. Online mentions are detected automatically. Anyone can request removal from the server list.")).fixedSize(horizontal:false,vertical:true)
            if loading {ProgressView().frame(maxWidth:.infinity)}
            if people.isEmpty && !loading && error.isEmpty {Text(P("bloggers_empty","The list is empty for now")).font(.title3).foregroundStyle(.secondary).frame(maxWidth:.infinity,minHeight:180)}
            ForEach(people) {person in
                VStack(alignment:.leading,spacing:8) {
                    if let url=person.link {Link(destination:url) {Label(person.name,systemImage:"arrow.up.right.square").font(.headline)}}
                    else {Text(person.name).font(.headline)}
                    if !person.description.isEmpty {Text(person.description).font(.subheadline).foregroundStyle(.secondary)}
                }.frame(maxWidth:.infinity,alignment:.leading).padding(18).goldGlass(radius:20)
            }
            if !error.isEmpty {Text(error).font(.footnote);Button(P("thanks_retry","Try again")) {Task {await refresh()}}}
            Text(P("bloggers_android_note","This list is also included in the Android version of the emulator.")).font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
        }.padding(20).frame(maxWidth:900).frame(maxWidth:.infinity)}
        .goldCommunityPage().navigationTitle(P("bloggers_title","Blogers&Additional Information")).task {await refresh()}.refreshable {await refresh()}
    }
    @MainActor private func refresh() async {
        loading=true;error="";defer {loading=false}
        do {
            let feed:Feed=try await LSX4AccountsAPI.request("api/credits")
            guard feed.version == 1,feed.people.count<=1000,Set(feed.people.map(\.id)).count == feed.people.count,
                  feed.people.allSatisfy({!$0.name.isEmpty && $0.name.count<=120 && $0.id.count<=64}) else {throw URLError(.cannotParseResponse)}
            people=feed.people
        } catch {self.error=error.localizedDescription}
    }
}
