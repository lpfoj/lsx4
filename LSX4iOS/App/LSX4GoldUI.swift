// SPDX-License-Identifier: GPL-2.0-or-later
// Presentation port of the current local goldoptimized launcher. Runtime and
// per-title launch profiles remain owned by the existing iOS host.
import SwiftUI
import UIKit
import ImageIO
import AVFoundation
import Metal

enum GoldText {
    static let strings: [String:[String:String]] = {
        guard let url=Bundle.main.url(forResource:"GoldStrings",withExtension:"json"),
              let data=try? Data(contentsOf:url),
              let value=try? JSONDecoder().decode([String:[String:String]].self,from:data) else {return [:]}
        return value
    }()
    static func get(_ key:String,_ fallback:String)->String {
        LSX4AppText.lookup(key) ?? strings[ConsoleLocalization.selected]?[key] ?? strings["en"]?[key] ?? fallback
    }
}
func G(_ key:String,_ fallback:String)->String {GoldText.get(key,fallback)}

enum GoldLayout:Int,CaseIterable,Identifiable {
    case list,ribbonHorizontal,ribbonVertical,libraryHorizontal,libraryVertical,tilesHorizontal,tilesVertical
    var id:Int {rawValue}
    var horizontal:Bool {[1,3,5].contains(rawValue)}
    var label:String {
        let keys=["list","horizontal","vertical","library_horizontal","library_vertical","tiles_horizontal","tiles_vertical"]
        let names=["List","Ribbon · horizontal","Ribbon · vertical","Library · horizontal","Library · vertical","Tiles · horizontal","Tiles · vertical"]
        return G("home_layout_"+keys[rawValue],names[rawValue])
    }
    var symbol:String {
        ["list.bullet","rectangle.split.3x1","rectangle.split.1x2","books.vertical","rectangle.grid.1x2","rectangle.grid.2x2","square.grid.2x2"][rawValue]
    }
}
enum GoldTheme:Int,CaseIterable,Identifiable {
    case azure,classic,sakura,autumn
    var id:Int {rawValue}
    var label:String {G(["home_theme_playstation","home_theme_classic","home_theme_sakura","home_theme_autumn"][rawValue],["Azure","Classic","Sakura","Autumn"][rawValue])}
    var colors:[Color] {
        let palettes:[[UInt32]]=[[0x0f2448,0x0a1a35,0x03070f],[0x0f0f1a,0x0a0a14,0x050508],[0x35102a,0x1f0815,0x12050d],[0x2a1a0f,0x1a0f0a,0x0a0805]]
        return palettes[rawValue].map(LibraryStyle.color)
    }
    var accent:Color {LibraryStyle.color([0x6fb4ff,0x6abba0,0xff91a4,0xff8c00][rawValue])}
    var symbol:String {["apple.logo","apple.logo","camera.macro","leaf"][rawValue]}
}

// Clear Liquid Glass keeps the background's color instead of a gray material fill.
struct GoldGlassSurface:View {
    var radius:CGFloat=14
    var selected=false
    @AppStorage("gold.theme") private var storedTheme=0
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.colorSchemeContrast) private var contrast
    private var accent:Color {(GoldTheme(rawValue:storedTheme) ?? .azure).accent}
    var body:some View {
        let shape=RoundedRectangle(cornerRadius:radius,style:.continuous)
        ZStack {
            if reduceTransparency {shape.fill(Color(white:0.1))}
            else if #available(iOS 26.0,*) {
                shape.fill(.black.opacity(contrast == .increased ? 0.32 : 0.12))
                    .glassEffect(.clear,in:shape)
            } else {shape.fill(.ultraThinMaterial)}
            shape.fill(accent.opacity(selected ? 0.075 : 0.025))
            shape.strokeBorder(LinearGradient(colors:[.white.opacity(selected ? 0.55 : 0.14),accent.opacity(selected ? 0.5 : 0.07),.white.opacity(0.06)],startPoint:.topLeading,endPoint:.bottomTrailing),lineWidth:selected ? 1.4 : 0.5)
        }.allowsHitTesting(false).accessibilityHidden(true)
    }
}
extension View {
    func goldGlass(radius:CGFloat=14,selected:Bool=false)->some View {background {GoldGlassSurface(radius:radius,selected:selected)}}
}

struct GoldBackdrop:View {
    let theme:GoldTheme
    var animated=true
    @State private var applicationActive=false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body:some View {
        TimelineView(.animation(minimumInterval:1.0/24,paused:!animated || !applicationActive || reduceMotion)) { context in
            let time=reduceMotion ? 0 : context.date.timeIntervalSinceReferenceDate
            Canvas { canvas,size in
                let bounds=CGRect(origin:.zero,size:size)
                var apple=canvas.resolve(Image(systemName:"apple.logo"))
                apple.shading = .color(theme.accent)
                canvas.fill(Path(bounds),with:.linearGradient(Gradient(colors:theme.colors),startPoint:.zero,endPoint:CGPoint(x:size.width,y:size.height)))
                // Broad pools of theme light make the frosted foreground legible.
                for i in 0..<3 {
                    let center=CGPoint(x:size.width*[0.12,0.65,0.94][i],y:size.height*[0.26,0.78,0.08][i])
                    canvas.fill(Path(bounds),with:.radialGradient(Gradient(colors:[theme.accent.opacity([0.23,0.16,0.10][i]),.clear]),center:center,startRadius:0,endRadius:size.width*[0.38,0.32,0.24][i]))
                }
                for i in 0..<3 {
                    let baseline=size.height*(0.39+Double(i)*0.11)
                    var path=Path();path.move(to:CGPoint(x:-20,y:baseline))
                    path.addCurve(to:CGPoint(x:size.width+20,y:baseline+50),control1:CGPoint(x:size.width*0.3,y:baseline+sin(time*0.4+Double(i))*75),control2:CGPoint(x:size.width*0.68,y:baseline-85))
                    canvas.stroke(path,with:.color(theme.accent.opacity(0.12-Double(i)*0.025)),lineWidth:2+Double(i)*9)
                }
                for i in 0..<48 {
                    let seed=Double(i)
                    let x=size.width*(Double((i*37)%97)/97)+sin(time*0.25+seed)*18
                    let y=(size.height*Double((i*29)%101)/101+time*Double(12+i%5*4)).truncatingRemainder(dividingBy:size.height+40)-20
                    let radius=Double(3+i%5)
                    var layer=canvas
                    layer.opacity=0.2+Double(i%3)*0.08
                    if theme == .azure || theme == .classic || i % 5 == 0 {
                        layer.translateBy(x:x,y:y);layer.rotate(by:.radians(sin(time*0.2+seed)*0.35))
                        layer.draw(apple,in:CGRect(x:-radius*1.2,y:-radius*1.5,width:radius*2.4,height:radius*3))
                    } else if theme == .sakura || theme == .autumn {
                        layer.translateBy(x:x,y:y);layer.rotate(by:.radians(time*0.15+seed))
                        var shape=Path()
                        if theme == .sakura {
                            shape.move(to:CGPoint(x:0,y:-radius*1.8))
                            shape.addQuadCurve(to:CGPoint(x:0,y:radius*0.9),control:CGPoint(x:radius*1.4,y:-radius*0.5))
                            shape.addQuadCurve(to:CGPoint(x:0,y:-radius*1.8),control:CGPoint(x:-radius*1.4,y:-radius*0.5))
                        } else {
                            let points:[CGPoint]=[.init(x:0,y:-1),.init(x:0.25,y:-0.6),.init(x:0.7,y:-0.5),.init(x:0.4,y:-0.2),.init(x:0.8,y:0.1),.init(x:0.3,y:0.4),.init(x:0.1,y:0.7),.init(x:0,y:0.9),.init(x:-0.1,y:0.7),.init(x:-0.3,y:0.4),.init(x:-0.8,y:0.1),.init(x:-0.4,y:-0.2),.init(x:-0.7,y:-0.5),.init(x:-0.25,y:-0.6)]
                            shape.addLines(points.map {CGPoint(x:$0.x*radius*1.8,y:$0.y*radius*1.8)})
                        }
                        shape.closeSubpath()
                        layer.fill(shape,with:.color(theme.accent))
                    } else {
                        layer.fill(Path(ellipseIn:CGRect(x:x,y:y,width:radius,height:radius)),with:.color(theme.accent))
                    }
                }
            }
        }.allowsHitTesting(false).accessibilityHidden(true).ignoresSafeArea()
            .onAppear {applicationActive=UIApplication.shared.applicationState == .active}
            .onReceive(NotificationCenter.default.publisher(for:UIApplication.didBecomeActiveNotification)) {_ in applicationActive=true}
            .onReceive(NotificationCenter.default.publisher(for:UIApplication.willResignActiveNotification)) {_ in applicationActive=false}
    }
}

enum GoldArtwork {
    static let cache:NSCache<NSString,UIImage> = {
        let value=NSCache<NSString,UIImage>();value.totalCostLimit=32*1024*1024;return value
    }()
    static func url(_ game:ConsoleTitle,hero:Bool=false)->URL? {
        if let root=game.folder {
            for name in hero ? ["pic1.png","pic0.png","icon0.png"] : ["icon0.png"] {
                let file=root.appendingPathComponent("sce_sys/"+name)
                if (try? file.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey])).map({$0.isRegularFile == true && $0.isSymbolicLink != true}) == true {return file}
            }
        }
        let file=Bundle.main.bundleURL.appendingPathComponent("GameCatalog/\(game.id)/icon0.png")
        return FileManager.default.fileExists(atPath:file.path) ? file : nil
    }
    static func load(_ url:URL,large:Bool)->UIImage? {
        let key=(url.path+(large ? ":hero" : ":icon")) as NSString
        if let image=cache.object(forKey:key) {return image}
        guard let source=CGImageSourceCreateWithURL(url as CFURL,nil),
              let cg=CGImageSourceCreateThumbnailAtIndex(source,0,[kCGImageSourceCreateThumbnailFromImageAlways:true,kCGImageSourceThumbnailMaxPixelSize:large ? 1024 : 256,kCGImageSourceCreateThumbnailWithTransform:true] as CFDictionary) else {return nil}
        let image=UIImage(cgImage:cg);cache.setObject(image,forKey:key,cost:cg.bytesPerRow*cg.height);return image
    }
}
struct GoldCover:View {
    let game:ConsoleTitle
    var hero=false
    var fit=false
    @State private var image:UIImage?
    var body:some View {
        GeometryReader { geometry in
            Group {
                if let image {Image(uiImage:image).resizable().aspectRatio(contentMode:fit ? .fit : .fill)}
                else {ZStack {Color.white.opacity(0.04);Image(systemName:"gamecontroller.fill").font(.system(size:30)).foregroundStyle(.white.opacity(0.45))}}
            }.frame(width:geometry.size.width,height:geometry.size.height).clipped()
        }.task(id:GoldArtwork.url(game,hero:hero)?.path) {
            guard let url=GoldArtwork.url(game,hero:hero) else {image=nil;return}
            let large=hero
            let loaded=await Task.detached(priority:.utility) {GoldArtwork.load(url,large:large)}.value
            if !Task.isCancelled {image=loaded}
        }.overlay(alignment:.topTrailing) {GoldUserStatusIcon(id:game.id)}.accessibilityHidden(true)
    }
}

struct GoldObservation:Decodable,Identifiable {
    let id:String,title:String,fps:Double,build:String,scope:String
    static let all:[Self] = {
        guard let url=Bundle.main.url(forResource:"ios-gameplay-observations",withExtension:"json"),let data=try? Data(contentsOf:url) else {return []}
        return (try? JSONDecoder().decode([Self].self,from:data)) ?? []
    }()
    static let byID=Dictionary(uniqueKeysWithValues:all.map {($0.id,$0)})
}

@MainActor final class GoldMenuAudio:ObservableObject {
    private var ambient:AVAudioPlayer?
    private var tick:AVAudioPlayer?
    private var visible=false
    private var originalSession:(AVAudioSession.Category,AVAudioSession.Mode,AVAudioSession.CategoryOptions)?
    private var lastError=""
    private func prepareSession() {
        guard originalSession == nil else {return}
        let session=AVAudioSession.sharedInstance()
        let previous=(session.category,session.mode,session.categoryOptions)
        do {
            try session.setCategory(.playback,mode:.default,options:[.mixWithOthers])
            try session.setActive(true)
            originalSession=previous;lastError=""
        } catch {lastError=error.localizedDescription;try? session.setCategory(previous.0,mode:previous.1,options:previous.2)}
    }
    private func releaseSession() {
        guard let previous=originalSession else {return}
        let session=AVAudioSession.sharedInstance()
        try? session.setActive(false,options:.notifyOthersOnDeactivation)
        try? session.setCategory(previous.0,mode:previous.1,options:previous.2)
        originalSession=nil
    }
    func update(visible:Bool) {
        self.visible=visible
        let preferences=UserDefaults.standard
        let music=preferences.object(forKey:"gold.music") as? Bool != false
        let effects=preferences.object(forKey:"gold.effects") as? Bool != false
        if visible && (music || effects) {prepareSession()}
        if visible && music {
            if ambient == nil,let url=Bundle.main.url(forResource:"menu_ambient",withExtension:"wav") {
                do {ambient=try AVAudioPlayer(contentsOf:url);ambient?.numberOfLoops = -1;ambient?.volume=0.5;ambient?.isMeteringEnabled=true}
                catch {lastError=error.localizedDescription}
            }
            if ambient?.isPlaying != true {ambient?.play()}
        } else {ambient?.pause()}
        if !visible || (!music && !effects) {tick?.stop();releaseSession()}
        record()
        Task {try? await Task.sleep(for:.seconds(2));if !Task.isCancelled {record()}}
    }
    func select() {
        guard visible,UserDefaults.standard.object(forKey:"gold.effects") as? Bool != false else {return}
        if tick == nil,let url=Bundle.main.url(forResource:"menu_select",withExtension:"wav") {tick=try? AVAudioPlayer(contentsOf:url);tick?.volume=0.3}
        tick?.currentTime=0;tick?.play()
    }
    func stop() {visible=false;ambient?.stop();tick?.stop();releaseSession();record()}
    private func record() {
        ambient?.updateMeters()
        let status:[String:Any]=["version":Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "",
            "timestamp":Date().timeIntervalSince1970,"visible":visible,"playing":ambient?.isPlaying ?? false,
            "currentTime":ambient?.currentTime ?? 0,"averagePowerDB":ambient?.averagePower(forChannel:0) ?? -160,
            "sessionCategory":AVAudioSession.sharedInstance().category.rawValue,"sessionOwned":originalSession != nil,
            "volume":ambient?.volume ?? 0,"applicationState":UIApplication.shared.applicationState.rawValue,"reduceMotion":UIAccessibility.isReduceMotionEnabled,"error":lastError]
        if let data=try? JSONSerialization.data(withJSONObject:status,options:.sortedKeys) {try? data.write(to:URL.documentsDirectory.appendingPathComponent("library-menu-audio-status.json"),options:.atomic)}
    }
}

enum GoldRoute:Identifiable {
    case settings,themes,compatibility,device,cache,importGame,game(String),report(String),quickMark(String,Bool)
    var id:String {
        switch self {
        case .settings:return "settings"
        case .themes:return "themes"
        case .compatibility:return "compatibility"
        case .device:return "device"
        case .cache:return "cache"
        case .importGame:return "import"
        case .game(let id):return id
        case .report(let id):return "report:"+id
        case .quickMark(let id,let playable):return "quick:"+id+String(playable)
        }
    }
}

enum GoldSection:String,CaseIterable,Identifiable {
    case device,compatibility,themes,cache
    var id:String {"menu:"+rawValue}
    var title:String {
        switch self {
        case .device:return G("home_device_title","Device")
        case .compatibility:return G("menu_compatibility_card","Tested compatible games")
        case .themes:return G("menu_themes_card","Themes")
        case .cache:return G("menu_render_cache","Render cache")
        }
    }
    var symbol:String {
        switch self {case .device:return "iphone";case .compatibility:return "checkmark.seal";case .themes:return "paintpalette";case .cache:return "cube.transparent"}
    }
    var route:GoldRoute {
        switch self {case .device:return .device;case .compatibility:return .compatibility;case .themes:return .themes;case .cache:return .cache}
    }
    var colors:[Color] {
        let values:[UInt32]
        switch self {case .device:values=[0x5d438f,0x21133f];case .compatibility:values=[0x257b94,0x123b53];case .themes:values=[0xec407a,0x7c1652];case .cache:values=[0x426bb4,0x142e54]}
        return values.map(LibraryStyle.color)
    }
}
enum GoldHomeItem:Identifiable {
    case section(GoldSection),game(ConsoleTitle)
    var id:String {switch self {case .section(let section):return section.id;case .game(let game):return game.id}}
    var title:String {switch self {case .section(let section):return section.title;case .game(let game):return game.title}}
    var game:ConsoleTitle? {if case .game(let game)=self {return game};return nil}
    var section:GoldSection? {if case .section(let section)=self {return section};return nil}
}
struct GoldSectionArtwork:View {
    let section:GoldSection
    var body:some View {
        GeometryReader { geometry in
            ZStack {
                LinearGradient(colors:section.colors.map {$0.opacity(0.55)},startPoint:.topLeading,endPoint:.bottomTrailing)
                Image(systemName:section.symbol).font(.system(size:min(70,min(geometry.size.width,geometry.size.height)*0.43),weight:.light)).foregroundStyle(.white)
            }.frame(width:geometry.size.width,height:geometry.size.height)
        }.accessibilityHidden(true)
    }
}

struct GoldLibraryView:View {
    @State private var applicationActive=false
    @AppStorage("ui_language") private var language="en"
    @AppStorage("gold.layout") private var storedLayout=0
    @AppStorage("gold.theme") private var storedTheme=0
    @AppStorage("gold.sort") private var sort=0
    @AppStorage("gold.reverse") private var reversed=false
    @AppStorage("gold.manualOrder") private var manualOrder=""
    @AppStorage("gold.music") private var music=true
    @AppStorage("gold.effects") private var effects=true
    @StateObject private var model=ConsoleLibraryModel()
    @StateObject private var audio=GoldMenuAudio()
    @State private var search=""
    @State private var showingSearch=false
    @State private var selectedID:String?
    @State private var route:GoldRoute?
    @State private var libraryActive=true
    @State private var dragging:String?
    @FocusState private var searchFocused:Bool
    private var layout:GoldLayout {GoldLayout(rawValue:storedLayout) ?? .list}
    private var theme:GoldTheme {GoldTheme(rawValue:storedTheme) ?? .azure}
    private var visible:[ConsoleTitle] {
        let manual=manualOrder.split(separator:",").map(String.init)
        let ranks=Dictionary(uniqueKeysWithValues:manual.enumerated().map {($0.element,$0.offset)})
        var result=model.games.filter {search.isEmpty || $0.title.localizedCaseInsensitiveContains(search) || $0.id.localizedCaseInsensitiveContains(search)}
        if sort == 0 && !manual.isEmpty {
            let indices=Dictionary(uniqueKeysWithValues:model.games.enumerated().map {($0.element.id,$0.offset)})
            result.sort {(ranks[$0.id] ?? (manual.count+(indices[$0.id] ?? 0))) < (ranks[$1.id] ?? (manual.count+(indices[$1.id] ?? 0)))}
        } else if sort == 1 || sort == 2 {
            let indices=Dictionary(uniqueKeysWithValues:model.games.enumerated().map {($0.element.id,$0.offset)})
            result.sort {
                let left=statusPriority($0),right=statusPriority($1)
                if left != right {return left<right}
                return (indices[$0.id] ?? 0)<(indices[$1.id] ?? 0)
            }
        } else if sort == 3 {result.sort {$0.title.localizedStandardCompare($1.title) == .orderedAscending}}
        if reversed {result.reverse()}
        return result
    }
    private var entries:[GoldHomeItem] {(search.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty ? GoldSection.allCases.map(GoldHomeItem.section) : [])+visible.map(GoldHomeItem.game)}
    private var selected:GoldHomeItem? {entries.first(where:{$0.id == selectedID}) ?? entries.first}
    var body:some View {
        NavigationStack {
            GeometryReader { geometry in
                ZStack {
                    GoldBackdrop(theme:theme,animated:libraryActive && route == nil)
                    if layout != .list,let game=selected?.game {
                        GoldCover(game:game,hero:true).id(game.id).opacity(0.15).blur(radius:12).ignoresSafeArea().allowsHitTesting(false).transition(.opacity)
                    }
                    VStack(spacing:geometry.size.height<430 ? 8 : 14) {
                        header
                        if showingSearch {searchBar.transition(.move(edge:.top).combined(with:.opacity))}
                        if model.pending != nil {pending}
                        content(geometry.size).frame(maxWidth:.infinity,maxHeight:.infinity)
                        footer
                    }.padding(.horizontal,20).padding(.top,12).padding(.bottom,8).frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.top)
                }
            }.toolbar(.hidden,for:.navigationBar)
                .sheet(item:$route,onDismiss:{model.refresh();model.publishInventory();audio.update(visible:applicationActive)}) { destination in
                    sheet(destination)
                }
                .alert("LSX4",isPresented:Binding(get:{model.notice != nil},set:{if !$0 {model.notice=nil}})) {
                    Button("OK") {model.notice=nil}
                } message: {Text(model.notice ?? "")}
#if LSX4_DEVICE_LOCAL_JIT
                .sheet(isPresented:$model.showingJITSetup) {LSX4JITSetupView()}
#endif
        }.tint(theme.accent).foregroundStyle(.white).preferredColorScheme(.dark)
            .environment(\.locale,Locale(identifier:language))
            .onChange(of:music) {_,_ in audio.update(visible:applicationActive && libraryActive)}
            .onChange(of:effects) {_,_ in audio.update(visible:applicationActive && libraryActive)}
            .onReceive(NotificationCenter.default.publisher(for:UIApplication.didBecomeActiveNotification)) {_ in applicationActive=true;audio.update(visible:libraryActive);if libraryActive {model.refresh();model.publishInventory()}}
            .onReceive(NotificationCenter.default.publisher(for:UIApplication.willResignActiveNotification)) {_ in applicationActive=false;audio.update(visible:false)}
            .onDisappear {if route == nil {libraryActive=false;audio.stop()}}
            .onAppear {libraryActive=true;applicationActive=UIApplication.shared.applicationState == .active;audio.update(visible:applicationActive)}
            .task {
                UIApplication.shared.isIdleTimerDisabled=true
                model.refresh();model.publishInventory()
#if LSX4_DEVICE_LOCAL_JIT
                LSX4JITInstallation.recordStatus()
                if LSX4JITInstallation.standalone,(try? LSX4PairingStore.read()) == nil {model.showingJITSetup=true}
#endif
                while !Task.isCancelled {
                    try? await Task.sleep(for:.seconds(model.pending == nil ? 10 : 2))
                    if !Task.isCancelled && applicationActive && libraryActive {model.refresh()}
                }
            }
    }
    private var header:some View {
        HStack(spacing:14) {
            Text("LSX4").font(.system(size:25,weight:.light,design:.rounded)).tracking(3)
            Spacer(minLength:8)
            Button {
                audio.select()
                withAnimation(.easeInOut(duration:0.2)) {showingSearch.toggle();if !showingSearch {search="";searchFocused=false;selectedID=nil}}
            } label: {Image(systemName:showingSearch ? "xmark" : "magnifyingglass").frame(width:42,height:42)}
                .accessibilityLabel(showingSearch ? G("close","Close search") : L("Поиск игры")).accessibilityIdentifier("gold-search-toggle")
            Menu {
                ForEach(GoldLayout.allCases) { style in
                    Button {storedLayout=style.rawValue;audio.select()} label: {Label(style.label,systemImage:layout == style ? "checkmark" : style.symbol)}
                        .accessibilityIdentifier("gold-layout-option-\(style.rawValue)")
                }
            } label: {Image(systemName:layout.symbol).frame(width:42,height:42)}
                .accessibilityLabel(G("home_layout_action","Library view")).accessibilityIdentifier("gold-layout")
            Menu {
                Button(G("library_sort_manual","Manual order")) {sort=0;reversed=false}
                Button(G("library_sort_supported_first","Playable first")) {sort=1;reversed=false}
                Button(G("library_sort_unsupported_first","Unplayable first")) {sort=2;reversed=false}
                Button(G("library_sort_name","Title A–Z")) {sort=3;reversed=false}
                Toggle(G("library_sort_reverse","Reverse order"),isOn:$reversed)
                Button("JIT FPS ↓") {sort=0;reversed=false;manualOrder=""}
            } label: {Image(systemName:"arrow.up.arrow.down").frame(width:42,height:42)}
                .accessibilityLabel(G("library_sort","Sort games")).accessibilityIdentifier("gold-sort")
            Button {audio.select();route = .importGame} label: {Image(systemName:"plus").frame(width:42,height:42)}
                .accessibilityLabel(L("pkg.import.title")).accessibilityIdentifier("library-import-pkg")
            Button {audio.select();route = .settings} label: {Image(systemName:"gearshape").frame(width:42,height:42)}
                .accessibilityLabel(L("Настройки")).accessibilityIdentifier("library-settings")
        }.padding(.horizontal,12).goldGlass(radius:16)
    }
    private var searchBar:some View {
        HStack {
            Image(systemName:"magnifyingglass").foregroundStyle(theme.accent)
            TextField(L("Поиск игры"),text:$search).autocorrectionDisabled().focused($searchFocused).submitLabel(.search).onSubmit {searchFocused=false}
                .accessibilityIdentifier("gold-search")
            if !search.isEmpty {Button {search="";searchFocused=false} label: {Image(systemName:"xmark.circle.fill")}.accessibilityLabel(G("clear","Clear"))}
        }.padding(.horizontal,12).frame(minHeight:36).goldGlass(radius:12).onAppear {searchFocused=true}
    }
    private var pending:some View {
        HStack {ProgressView();Text(model.status).font(.caption);Spacer();Button(L("Отмена")) {model.cancelLaunch()}}.padding(10).goldGlass(radius:12)
    }
    private var footer:some View {
        HStack {
            Text(layout.label).font(.caption2).foregroundStyle(.secondary)
            Spacer()
            if layout != .list,let item=selected {
                if let game=item.game {
                    Button {route = .game(game.id)} label: {Label(L("Сведения"),systemImage:"slider.horizontal.3")}.font(.caption).accessibilityIdentifier("gold-selected-details")
                    Button {launch(game)} label: {Label(G("start_game","Play"),systemImage:"play.fill")}.font(.caption.bold()).buttonStyle(.borderedProminent).accessibilityIdentifier("gold-play-selected")
                } else {
                    Button {activate(item)} label: {Label(G("menu_section_open","Open"),systemImage:"arrow.up.right")}.font(.caption.bold()).buttonStyle(.borderedProminent).accessibilityIdentifier("gold-open-selected")
                }
            } else {Text("iOS · JIT").font(.caption2).foregroundStyle(.secondary)}
        }.frame(minHeight:layout == .list ? 18 : 34)
    }
    @ViewBuilder private func content(_ size:CGSize)->some View {
        if entries.isEmpty {ContentUnavailableView.search(text:search).frame(maxHeight:.infinity)}
        else {
            switch layout {
            case .list:list
            case .ribbonHorizontal:ribbon(horizontal:true,size:size)
            case .ribbonVertical:ribbon(horizontal:false,size:size)
            case .libraryHorizontal:libraryHorizontal(size:size)
            case .libraryVertical:libraryVertical
            case .tilesHorizontal:tiles(horizontal:true,size:size)
            case .tilesVertical:tiles(horizontal:false,size:size)
            }
        }
    }
    @ViewBuilder private func artwork(_ item:GoldHomeItem,hero:Bool=false,fit:Bool=false)->some View {
        if let game=item.game {GoldCover(game:game,hero:hero,fit:fit)}
        else if let section=item.section {GoldSectionArtwork(section:section)}
    }
    @ViewBuilder private func homeRow(_ item:GoldHomeItem)->some View {
        if let game=item.game {row(game)}
        else if let section=item.section {
            HStack(spacing:14) {
                GoldSectionArtwork(section:section).frame(width:44,height:44).clipShape(RoundedRectangle(cornerRadius:9))
                VStack(alignment:.leading,spacing:3) {
                    Text(section.title).font(.system(size:16,weight:.medium)).lineLimit(2)
                    Text(G("menu_section_open","Open")).font(.caption2).foregroundStyle(.secondary)
                }.frame(maxWidth:.infinity,alignment:.leading)
                Image(systemName:"chevron.right").foregroundStyle(theme.accent)
            }.contentShape(Rectangle())
        }
    }
    private func identifier(_ item:GoldHomeItem,list:Bool=false)->String {
        item.section.map {"gold-"+$0.rawValue} ?? ((list ? "launch-" : "gold-select-")+item.id)
    }
    private func select(_ item:GoldHomeItem) {
        if selectedID != item.id {audio.select()}
        withAnimation(.spring(response:0.36,dampingFraction:0.82)) {selectedID=item.id}
    }
    private func activate(_ item:GoldHomeItem) {
        if let game=item.game {launch(game)}
        else if let section=item.section {audio.select();route=section.route}
    }
    private func choose(_ item:GoldHomeItem) {select(item);if item.section != nil {activate(item)}}
    @ViewBuilder private func itemContext(_ item:GoldHomeItem)->some View {
        if let game=item.game {
            Button(G("start_game","Play")) {launch(game)}
            Button(L("Сведения")) {route = .game(game.id)}
            Button(LSX4ReportedResult.playable.label,systemImage:"checkmark.circle") {mark(game,"supported")}
            Button(LSX4ReportedResult.unplayable.label,systemImage:"xmark.circle") {mark(game,"unsupported")}
            Button(G("public_report_title","Report a game result"),systemImage:"paperplane") {route = .report(game.id)}
            Button(G("library_move_to_top","Move to top")) {reorderToTop(game.id)}
        } else {Button(G("menu_section_open","Open")) {activate(item)}}
    }
    private var list:some View {
        ScrollView {
            LazyVStack(spacing:8) {ForEach(entries) {item in
                if let game=item.game {
                    HStack(spacing:12) {
                        GoldReportableRow(tap:{launch(game)},mark:{mark(game,$0 ? "supported" : "unsupported")},identifier:identifier(item,list:true)) {row(game)}
                        gameMenu(game)
                    }.padding(8).goldGlass(radius:16)
                        .dropDestination(for:String.self) {ids,_ in reorder(ids.first,before:game.id)}
                } else {
                    Button {activate(item)} label: {homeRow(item).padding(8).goldGlass(radius:16)}
                        .buttonStyle(.plain).accessibilityIdentifier(identifier(item,list:true))
                }
            }}.padding(.bottom,4)
        }.refreshable {model.refresh();model.publishInventory()}
    }
    private func row(_ game:ConsoleTitle)->some View {
        HStack(spacing:14) {
            GoldCover(game:game).frame(width:52,height:52).clipShape(RoundedRectangle(cornerRadius:9))
            VStack(alignment:.leading,spacing:5) {
                Text(game.title).font(.system(size:17,weight:.medium)).lineLimit(2)
                HStack(spacing:8) {Text(game.id);if let observation=GoldObservation.byID[game.id] {Text(String(format:"JIT · %.1f FPS",observation.fps)).foregroundStyle(.green)}}.font(.caption2)
                Text(game.folder != nil ? ByteCountFormatter.string(fromByteCount:game.bytes,countStyle:.file) : L("jit.setup.files.missing")).font(.caption2).foregroundStyle(.secondary)
            }.frame(maxWidth:.infinity,alignment:.leading)
            Image(systemName:game.folder != nil ? "play.fill" : "square.and.arrow.down").foregroundStyle(theme.accent)
        }.contentShape(Rectangle())
    }
    private func gameMenu(_ game:ConsoleTitle)->some View {
        Menu {
            Button(G("start_game","Play"),systemImage:"play.fill") {launch(game)}
            Button(L("Сведения"),systemImage:"info.circle") {route = .game(game.id)}
            Button(LSX4ReportedResult.playable.label,systemImage:"checkmark.circle") {mark(game,"supported")}
            Button(LSX4ReportedResult.unplayable.label,systemImage:"xmark.circle") {mark(game,"unsupported")}
            Button(G("public_report_title","Report a game result"),systemImage:"paperplane") {route = .report(game.id)}
            Button(G("library_move_to_top","Move to top"),systemImage:"arrow.up.to.line") {reorderToTop(game.id)}
        } label: {Image(systemName:"ellipsis").frame(width:36,height:44)}.accessibilityLabel("Menu \(game.title)")
    }
    private func mark(_ game:ConsoleTitle,_ status:String) {
        if LSX4ReportSharing.enabled {route = .quickMark(game.id,status == "supported")}
        else {UserDefaults.standard.set(status,forKey:"gold.status."+game.id);model.notice=P("sharing_off_message","Sending is off. Marks are saved on this iPhone. Enable report sharing in Settings to publish.")}
    }
    private func statusPriority(_ game:ConsoleTitle)->Int {
        let stored=UserDefaults.standard.string(forKey:"gold.status."+game.id) ?? ""
        let supported=stored == "supported" || (stored.isEmpty && GoldObservation.byID[game.id] != nil)
        let unsupported=stored == "unsupported"
        if !supported && !unsupported {return 2}
        return (sort == 1 ? supported : unsupported) ? 0 : 1
    }
    private func launch(_ game:ConsoleTitle) {
        libraryActive=false;audio.stop();searchFocused=false;model.launch(game)
        var setup=false
#if LSX4_DEVICE_LOCAL_JIT
        setup=model.showingJITSetup
#endif
        if model.notice != nil || game.folder == nil || setup {libraryActive=true;audio.update(visible:applicationActive)}
    }
    private func reorderToTop(_ id:String) {var ids=visible.map(\.id);ids.removeAll {$0 == id};ids.insert(id,at:0);manualOrder=ids.joined(separator:",");sort=0;reversed=false}
    private func reorder(_ source:String?,before:String)->Bool {
        guard let source,source != before,model.games.contains(where:{$0.id == source}) else {return false}
        var ids=visible.map(\.id);ids.removeAll {$0 == source}
        guard let index=ids.firstIndex(of:before) else {return false}
        ids.insert(source,at:index);manualOrder=ids.joined(separator:",");sort=0;reversed=false;return true
    }
    private func selectedHeading(_ item:GoldHomeItem)->some View {
        VStack(alignment:.leading,spacing:4) {
            Text(item.title).font(.system(size:22,weight:.light)).lineLimit(2)
            if let game=item.game {Text(game.id+(GoldObservation.byID[game.id].map {String(format:" · JIT %.1f FPS",$0.fps)} ?? "")).font(.caption).foregroundStyle(theme.accent)}
            else {Text(G("menu_section_open","Open")).font(.caption).foregroundStyle(theme.accent)}
        }.frame(maxWidth:.infinity,alignment:.leading).accessibilityIdentifier("gold-selection-heading")
    }
    private func ribbon(horizontal:Bool,size:CGSize)->some View {
        VStack(alignment:.leading,spacing:12) {
            if let selected {selectedHeading(selected)}
            ScrollView(horizontal ? .horizontal : .vertical) {
                if horizontal {
                    LazyHStack(alignment:.center,spacing:18) {ForEach(entries) {item in
                        Button {choose(item)} label: {
                            artwork(item,hero:true).frame(width:selected?.id == item.id ? 220 : 180,height:min(150,max(96,size.height*0.28)))
                                .clipShape(RoundedRectangle(cornerRadius:10)).padding(5).goldGlass(radius:15,selected:selected?.id == item.id)
                        }.buttonStyle(.plain).accessibilityLabel(item.title).accessibilityIdentifier(identifier(item))
                            .contextMenu {itemContext(item)}
                    }}.scrollTargetLayout().padding(6)
                } else {
                    LazyVStack(spacing:10) {ForEach(entries) {item in
                        Button {choose(item)} label: {homeRow(item).padding(8).goldGlass(radius:14,selected:selected?.id == item.id)}
                            .buttonStyle(.plain).accessibilityIdentifier(identifier(item)).contextMenu {itemContext(item)}
                    }}.scrollTargetLayout()
                }
            }.scrollIndicators(.hidden).scrollTargetBehavior(.viewAligned).scrollPosition(id:$selectedID,anchor:horizontal ? .center : .top)
        }.animation(.easeOut(duration:0.18),value:selectedID)
    }
    private func libraryHorizontal(size:CGSize)->some View {
        HStack(spacing:14) {
            VStack(spacing:16) {Image(systemName:"books.vertical").font(.largeTitle);Text("LSX4").font(.caption).tracking(2);Spacer()}.foregroundStyle(theme.accent).frame(width:58).padding(.top,14)
            VStack(alignment:.leading,spacing:14) {
                if let selected {selectedHeading(selected)}
                ScrollView(.horizontal) {LazyHStack(spacing:12) {ForEach(entries) {item in tile(item,width:148,height:min(210,max(130,size.height*0.40)))}}.padding(4)}.scrollIndicators(.hidden)
            }
        }
    }
    private var libraryVertical:some View {
        HStack(spacing:16) {
            if let selected {VStack(alignment:.leading,spacing:12) {artwork(selected,hero:true,fit:true).frame(maxHeight:.infinity).clipShape(RoundedRectangle(cornerRadius:14));selectedHeading(selected)}.frame(maxWidth:.infinity)}
            ScrollView {LazyVStack(spacing:8) {ForEach(entries) {item in
                Button {choose(item)} label: {homeRow(item).padding(8).goldGlass(radius:14,selected:selected?.id == item.id)}
                    .buttonStyle(.plain).accessibilityIdentifier(identifier(item)).contextMenu {itemContext(item)}
            }}}.frame(maxWidth:.infinity)
        }
    }
    private func tiles(horizontal:Bool,size:CGSize)->some View {
        GeometryReader {available in
            let height=max(48,min(154,(available.size.height-20)/2))
            ScrollView(horizontal ? .horizontal : .vertical) {
                if horizontal {
                    LazyHGrid(rows:[GridItem(.fixed(height)),GridItem(.fixed(height))],spacing:12) {ForEach(entries) {item in tile(item,width:190,height:height)}}.padding(4)
                } else {
                    LazyVGrid(columns:[GridItem(.flexible()),GridItem(.flexible())],spacing:12) {ForEach(entries) {item in tile(item,width:nil,height:128)}}.padding(4)
                }
            }.scrollIndicators(.hidden).frame(width:available.size.width,height:available.size.height,alignment:.topLeading)
        }
    }
    private func tile(_ item:GoldHomeItem,width:CGFloat?,height:CGFloat)->some View {
        Button {choose(item)} label: {
            ZStack(alignment:.bottomLeading) {
                artwork(item,hero:layout == .tilesHorizontal || layout == .tilesVertical).clipShape(RoundedRectangle(cornerRadius:14,style:.continuous))
                VStack(alignment:.leading,spacing:3) {
                    Text(item.title).font(.system(size:13,weight:.medium)).lineLimit(2)
                    if let game=item.game {Text(game.id).font(.system(size:9)).foregroundStyle(.white.opacity(0.75))}
                }.shadow(color:.black.opacity(0.9),radius:2,y:1).frame(maxWidth:.infinity,alignment:.leading).padding(8).goldGlass(radius:10,selected:selected?.id == item.id).padding(5)
            }.frame(width:width,height:height).frame(maxWidth:width == nil ? .infinity : nil).goldGlass(radius:14,selected:selected?.id == item.id)
                .overlay(RoundedRectangle(cornerRadius:14,style:.continuous).strokeBorder(selected?.id == item.id ? theme.accent.opacity(0.8) : .white.opacity(0.18),lineWidth:selected?.id == item.id ? 1.6 : 0.7))
        }.buttonStyle(.plain).accessibilityLabel(item.title).accessibilityIdentifier(identifier(item)).contextMenu {itemContext(item)}
    }
    @ViewBuilder private func sheet(_ destination:GoldRoute)->some View {
        switch destination {
        case .importGame:LSX4PkgImportView()
        case .settings:GoldSettingsView(audio:audio)
        case .themes:GoldThemesView()
        case .device:GoldDeviceView()
        case .cache:GoldCacheView(games:model.games)
        case .compatibility:GoldCompatibilityView(games:model.games)
        case .game(let id):if let game=model.games.first(where:{$0.id == id}) {GoldGameDetails(game:game)}
        case .report(let id):if let game=model.games.first(where:{$0.id == id}) {GoldPublicReportView(game:game)}
        case .quickMark(let id,let playable):if let game=model.games.first(where:{$0.id == id}) {GoldQuickMarkView(game:game,result:playable ? .playable : .unplayable)}
        }
    }
}
