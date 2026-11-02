// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import Metal

struct GoldPage<Content:View>:View {
    let title:String
    var dismissDisabled=false
    var themeOverride:GoldTheme?=nil
    @ViewBuilder let content:Content
    @Environment(\.dismiss) private var dismiss
    @AppStorage("gold.theme") private var storedTheme=0
    private var theme:GoldTheme {themeOverride ?? GoldTheme(rawValue:storedTheme) ?? .azure}
    var body:some View {
        GeometryReader { geometry in
        NavigationStack {
            ZStack {GoldBackdrop(theme:theme);content}
                .navigationTitle(title).navigationBarTitleDisplayMode(.inline)
                .toolbar {ToolbarItem(placement:.confirmationAction) {Button(L("Готово")) {dismiss()}.disabled(dismissDisabled).accessibilityIdentifier("gold-page-done")}}
        }.padding(.top,LSX4WindowChrome.topPadding(safeTop:geometry.safeAreaInsets.top,baseline:0))
        }.frame(maxWidth:.infinity,maxHeight:.infinity).interactiveDismissDisabled(dismissDisabled).preferredColorScheme(.dark).tint(theme.accent).environment(\.goldPreviewTheme,themeOverride)
    }
}
struct GoldThemesView:View {
    @AppStorage("gold.theme") private var applied=0
    @State private var preview=0
    var body:some View {
        GoldPage(title:G("menu_themes_card","Themes"),themeOverride:GoldTheme(rawValue:preview)) {
            GeometryReader { geometry in
                let columns=geometry.size.width<600 ? 2 : 4
                let rows=(GoldTheme.allCases.count+columns-1)/columns
                let padding:CGFloat=geometry.size.height<300 ? 8 : 12
                let gap:CGFloat=geometry.size.height<300 ? 8 : 12
                let gridHeight=max(0,geometry.size.height-padding*2-44-gap)
                let height=max(0,min(420,(gridHeight-CGFloat(rows-1)*gap)/CGFloat(rows)))
                VStack(spacing:gap) {
                    LazyVGrid(columns:Array(repeating:GridItem(.flexible()),count:columns),spacing:gap) {ForEach(GoldTheme.allCases) {theme in
                        Button {preview=theme.rawValue} label: {
                            ZStack(alignment:.bottomLeading) {
                                GoldBackdrop(theme:theme,animated:preview == theme.rawValue)
                                VStack(alignment:.leading,spacing:6) {
                                    if height>=100 {Image(systemName:theme.symbol).font(.system(size:height<150 ? 23 : 28)).foregroundStyle(theme.accent)}
                                    Spacer(minLength:0)
                                    Text(theme.label).font(.system(size:geometry.size.width<500 ? 13 : 18,weight:.medium)).lineLimit(1).minimumScaleFactor(0.7)
                                    if applied == theme.rawValue {Label(G("menu_selected","Selected"),systemImage:"checkmark.circle.fill").font(.caption2).lineLimit(1).minimumScaleFactor(0.7).foregroundStyle(theme.accent)}
                                }.padding(height<150 ? 8 : 12)
                            }.frame(height:height).clipShape(RoundedRectangle(cornerRadius:16))
                                .overlay(RoundedRectangle(cornerRadius:16).stroke(preview == theme.rawValue ? theme.accent : .white.opacity(0.12),lineWidth:preview == theme.rawValue ? 3 : 1).allowsHitTesting(false))
                                .contentShape(RoundedRectangle(cornerRadius:16))
                        }.buttonStyle(.plain).accessibilityIdentifier("gold-theme-\(theme.rawValue)")
                            .accessibilityValue(preview == theme.rawValue ? G("menu_selected","Selected") : "")
                    }}.frame(height:gridHeight)
                    HStack {
                        Text((GoldTheme(rawValue:preview) ?? .azure).label).font(.headline).lineLimit(1).minimumScaleFactor(0.7)
                        Spacer()
                        Button(G("menu_apply","Apply")) {applied=preview}.buttonStyle(.borderedProminent).accessibilityIdentifier("gold-theme-apply")
                    }.frame(height:44)
                }.padding(padding)
            }
        }.onAppear {preview=applied}
    }
}

struct GoldSettingsView:View {
    @ObservedObject var audio:GoldMenuAudio
    @AppStorage("ui_language") private var language="en"
    @AppStorage("gold.music") private var music=true
    @AppStorage("gold.effects") private var effects=true
    @AppStorage("lsx4.renderResolution") private var resolution="544p"
    @AppStorage("lsx4.upscaler") private var upscaler=ConsoleUpscaler.fsr1.rawValue
    @AppStorage("lsx4.showExecutionMode") private var showExecutionMode=false
    @AppStorage("lsx4.executionMode") private var mode=""
    @State private var jitSetup=false
    @State private var showingLanguages=false
    var body:some View {
        GoldPage(title:L("Настройки")) {
            List {
                Section(G("section_general","General")) {
                    Button {showingLanguages=true} label: {
                        HStack {Text(G("language","Language"));Spacer();if let choice=ConsoleLocalization.languages.first(where:{$0.id == language}) {Text(choice.flag+" "+choice.label).foregroundStyle(.secondary)}}.contentShape(Rectangle())
                    }.accessibilityIdentifier("ui-language")
                    NavigationLink {GoldBloggersView()} label: {Label(P("bloggers_title","Blogers&Additional Information"),systemImage:"person.2.wave.2")}.accessibilityIdentifier("settings-bloggers")
                    NavigationLink {GoldThanksView()} label: {Label(P("thanks_title","Thanks"),systemImage:"heart")}.accessibilityIdentifier("settings-thanks")
#if LSX4_DEVICE_LOCAL_JIT
                    Button(L("jit.setup.title")) {jitSetup=true}.accessibilityIdentifier("settings-jit-setup")
#endif
                }
                NavigationLink {GoldAccountView()} label: {Label(P("account_title","Account"),systemImage:"person.crop.circle")}.accessibilityIdentifier("settings-account")
                NavigationLink {GoldMemberDirectory()} label: {Label(P("members_title","All users"),systemImage:"person.2")}.accessibilityIdentifier("settings-all-users")
                NavigationLink {GoldSharingSettings()} label: {Label(P("sharing_title","Send data to server"),systemImage:"network")}.accessibilityIdentifier("settings-data-sharing")
                NavigationLink {GoldGamePatchesView()} label: {category("patches_title","Game patches","wrench.and.screwdriver")}.accessibilityIdentifier("settings-game-patches")
                NavigationLink {graphics} label: {category("category_graphics","Graphics","display")}.accessibilityIdentifier("settings-graphics")
                NavigationLink {GoldManagedOptimizationsView()} label: {category("category_managed_optimizations","Managed optimizations","wand.and.stars")}.accessibilityIdentifier("settings-managed-optimizations")
                NavigationLink {cpu} label: {category("category_cpu_tuning","CPU tuning","cpu")}.accessibilityIdentifier("settings-cpu")
                NavigationLink {audioSettings} label: {category("category_audio","Audio","speaker.wave.2")}.accessibilityIdentifier("settings-audio")
                NavigationLink {GoldControlsView()} label: {category("category_controls","Controls","gamecontroller")}.accessibilityIdentifier("settings-controls")
                NavigationLink {GoldCacheContents(games:[])} label: {category("category_cache","Cache","cube.transparent")}.accessibilityIdentifier("settings-cache")
                NavigationLink {GoldHiddenGamesView()} label: {category("game_hidden_title","Hidden games","eye.slash")}.accessibilityIdentifier("settings-hidden-games")
                NavigationLink {other} label: {category("category_other","Other","ellipsis.circle")}.accessibilityIdentifier("settings-other")
                NavigationLink {PS5SettingsView()} label: {category("ps5_preset_title","PS5 Settings","cpu")}.accessibilityIdentifier("settings-ps5")
            }.scrollContentBackground(.hidden)
                .navigationDestination(isPresented:$showingLanguages) {GoldLanguagePicker(language:$language)}
        }.environment(\.locale,Locale(identifier:language))
#if LSX4_DEVICE_LOCAL_JIT
            .goldModal(isPresented:$jitSetup) {LSX4JITSetupView()}
#endif
    }
    private func category(_ key:String,_ name:String,_ icon:String)->some View {Label(G(key,name),systemImage:icon).padding(.vertical,8)}
    private var graphics:some View {
        Form {
            Section(G("graphics_resolution","Resolution")) {
                Picker(L("Разрешение игры и вывода"),selection:$resolution) {ForEach(ConsoleRenderResolution.allCases) {Text($0.label).tag($0.rawValue)}}.accessibilityIdentifier("game-render-resolution")
                if resolution == ConsoleRenderResolution.low.rawValue {
                    Picker(G("graphics_upscaler","Upscaler"),selection:$upscaler) {ForEach(ConsoleUpscaler.allCases) {Text($0.label).tag($0.rawValue)}}.accessibilityIdentifier("game-upscaler")
                    Text(G("graphics_upscaler_hint","360p renders at 656 × 368. An upscaler enlarges it to 960 × 544, even when FSR is off in optimizations; Off shows 656 × 368 as is.")).font(.footnote).foregroundStyle(.secondary)
                }
                Text(L("Меньшее разрешение снижает нагрузку на графику и чёткость. Применяется при следующем запуске игры."))
                Text(G("graphics_bloodborne_preset","Bloodborne uses 360p with AMD FSR 1.0 until you pick a resolution here.")).font(.footnote).foregroundStyle(.secondary)
                Text(L("Игры с фиксированным внутренним разрешением могут менять только размер выходного кадра.")).font(.footnote).foregroundStyle(.secondary)
            }
            Section {LabeledContent("GPU",value:MTLCreateSystemDefaultDevice()?.name ?? "Apple GPU");LabeledContent(G("graphics_backend","Graphics backend"),value:"Vulkan · MoltenVK · Metal")}
        }.navigationTitle(G("category_graphics","Graphics"))
    }
    private var managed:some View {
        Form {
            Section {Label(G("gold_profiles_automatic","Automatic game profiles"),systemImage:"checkmark.seal.fill").foregroundStyle(.green)
                Text(G("gold_profiles_note","Each game uses its existing compatibility and performance profile. Your working profiles are preserved."))}
            Section {NavigationLink(G("category_graphics","Graphics")) {graphics};NavigationLink(G("category_cpu_tuning","CPU tuning")) {cpu}}
        }.navigationTitle(G("category_managed_optimizations","Managed optimizations"))
    }
    private var cpu:some View {
        Form {
            Section(L("Исполнение CPU")) {
#if LSX4_DEVICE_LOCAL_JIT
                if LSX4JITInstallation.standalone {
                    LabeledContent(L("Активная сборка"),value:"JIT")
                    Text(L("jit.setup.automatic"))
                } else {modePicker}
                Button(L("jit.setup.title")) {jitSetup=true}.accessibilityIdentifier("settings-jit-setup-cpu")
#else
                modePicker
#endif
                Text(L("Выбор применяется при следующем запуске игры. При смене режима игра запускается заново; сохранения и библиотека остаются доступны."))
            }
        }.navigationTitle(G("category_cpu_tuning","CPU tuning"))
    }
    private var modePicker:some View {
        Picker(L("Режим"),selection:Binding(get:{ConsoleExecutionMode(rawValue:mode)?.rawValue ?? ConsoleExecutionMode.installed.rawValue},set:{mode=$0})) {
            ForEach(ConsoleExecutionMode.allCases) {Text($0.label).tag($0.rawValue)}
        }.pickerStyle(.segmented).accessibilityIdentifier("cpu-execution-mode")
    }
    private var audioSettings:some View {
        Form {Section(G("gold_menu_audio","Menu audio")) {
            Toggle(G("home_sound_effects","Sound effects"),isOn:$effects).accessibilityIdentifier("gold-sound-effects")
            Toggle(G("home_music","Music"),isOn:$music).accessibilityIdentifier("gold-music")
        }}.navigationTitle(G("category_audio","Audio"))
    }
    private var other:some View {
        Form {
            Section(G("gold_display","Display")) {Toggle(L("Показывать JIT / No-JIT"),isOn:$showExecutionMode).accessibilityIdentifier("show-execution-mode");Text(L("Название LSX4 и FPS остаются видны. Режим исполнения показывается только при включённом переключателе."))}
            Section("LSX4") {LabeledContent(G("version","Version"),value:Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "")
                Link(G("gold_installation","Installation guide"),destination:URL(string:"https://github.com/lpfoj/lsx4/releases")!)}
        }.navigationTitle(G("category_other","Other"))
    }
}

private struct GoldControllerPreview:UIViewRepresentable {
    let revision:Int
    func makeUIView(context:Context)->DualShockOverlayView {let view=DualShockOverlayView();view.isUserInteractionEnabled=false;return view}
    func updateUIView(_ view:DualShockOverlayView,context:Context) {view.setNeedsLayout();view.setNeedsDisplay()}
}
struct GoldControlsView:View {
    @State private var control="dpad"
    @State private var revision=0
    private let controls=["dpad","face","left_stick","right_stick","l1","l2","r1","r2","options","share","ps","touchpad"]
    private var prefix:String {"overlay_layout_v1_"+control+"_"}
    private func number(_ key:String,_ fallback:Double)->Binding<Double> {
        Binding(get:{UserDefaults.standard.object(forKey:prefix+key) as? Double ?? fallback},set:{UserDefaults.standard.set($0,forKey:prefix+key);revision+=1})
    }
    var body:some View {
        Form {
            GoldControllerPreview(revision:revision).frame(height:220).listRowBackground(Color.black.opacity(0.3))
            Picker(G("gold_control","Control"),selection:$control) {ForEach(controls,id:\.self) {Text($0.replacingOccurrences(of:"_",with:" ").uppercased()).tag($0)}}
            Toggle(G("gold_visible","Visible"),isOn:Binding(get:{UserDefaults.standard.object(forKey:prefix+"visible") as? Bool ?? !["share","ps","touchpad"].contains(control)},set:{UserDefaults.standard.set($0,forKey:prefix+"visible");revision+=1}))
            Section(G("gold_size","Size")) {Slider(value:number("scale",1),in:0.45...2.2)}
            Section(G("gold_position","Position")) {Label("X",systemImage:"arrow.left.and.right");Slider(value:number("x",0),in:-0.5...0.5);Label("Y",systemImage:"arrow.up.and.down");Slider(value:number("y",0),in:-0.5...0.5)}
            Button(G("reset","Reset"),role:.destructive) {for key in ["scale","x","y","visible"] {UserDefaults.standard.removeObject(forKey:prefix+key)};revision+=1}
        }.navigationTitle(G("category_controls","Controls"))
    }
}

struct GoldDeviceView:View {
    @State private var refresh=0
    private var storage:Int64 {let _=refresh;return (try? URL.documentsDirectory.resourceValues(forKeys:[.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0}
    private var machine:String {var system=utsname();uname(&system);let capacity=MemoryLayout.size(ofValue:system.machine);return withUnsafePointer(to:&system.machine) {$0.withMemoryRebound(to:CChar.self,capacity:capacity) {String(cString:$0)}}}
    var body:some View {
        GoldPage(title:G("home_device_title","Device")) {
            Form {
                Section {Label(UIDevice.current.name,systemImage:"iphone").font(.title2);LabeledContent(G("device_model","Model"),value:machine);LabeledContent("iOS",value:UIDevice.current.systemVersion)}
                Section {LabeledContent("CPU",value:"\(ProcessInfo.processInfo.processorCount) cores");LabeledContent("GPU",value:MTLCreateSystemDefaultDevice()?.name ?? "Apple GPU");LabeledContent("RAM",value:ByteCountFormatter.string(fromByteCount:Int64(ProcessInfo.processInfo.physicalMemory),countStyle:.memory));LabeledContent(G("storage_available","Available storage"),value:ByteCountFormatter.string(fromByteCount:storage,countStyle:.file))}
                Section {LabeledContent(G("gold_thermal","Thermal state"),value:[P("thermal_normal","Normal"),P("thermal_fair","Warm"),P("thermal_serious","Hot"),P("thermal_critical","Critical")][min(3,ProcessInfo.processInfo.thermalState.rawValue)]);LabeledContent(G("version","Version"),value:Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "");LabeledContent("CPU",value:ConsoleExecutionMode.selected.label)}
            }.scrollContentBackground(.hidden)
        }.task {while !Task.isCancelled {try? await Task.sleep(for:.seconds(5));if !Task.isCancelled {refresh+=1}}}
    }
}

struct GoldGameDetails:View {
    let game:ConsoleTitle
    var removalAllowed=true
    @State private var removing=false
    @State private var classification=""
    @State private var notes=""
    @State private var showingReport=false
    @State private var initializing=true
    var body:some View {
        GoldPage(title:game.title,dismissDisabled:removing) {
            Form {
                GoldCover(game:game,hero:true,fit:true).frame(height:150).listRowBackground(Color.clear)
                Section {LabeledContent("TITLE_ID",value:game.id);LabeledContent(L("Файлы"),value:game.folder != nil ? L("На iPhone") : L("jit.setup.files.missing"));LabeledContent(G("gold_size","Size"),value:ByteCountFormatter.string(fromByteCount:game.bytes,countStyle:.file))}
                if let observation=GoldObservation.byID[game.id] {
                    Section("iPhone · JIT") {Text(String(format:"%.1f FPS",observation.fps)).font(.title2).foregroundStyle(.green);Text(G("gold_prior_scope","Previous gameplay observation, not a full-game guarantee or a new measurement.")).font(.footnote);LabeledContent(G("version","Version"),value:observation.build)}
                }
                Section {
                    NavigationLink {
                        if game.id.hasPrefix("PPSA") {GoldPS5GameSettings(title:game.id)}
                        else {GoldManagedOptimizationsView(title:game.id,gameTitle:game.title)}
                    } label: {Label(G("category_managed_optimizations","Managed optimizations"),systemImage:"wand.and.stars")}.accessibilityIdentifier("game-managed-optimizations")
                }
                Section(G("gold_your_result","Your result")) {
                    Picker(G("game_status","Status"),selection:$classification) {
                        Text(G("game_status_unclassified","Unclassified")).tag("")
                        Text(G("game_status_supported","Playable")).tag("supported")
                        Text(G("game_status_unsupported","Unplayable")).tag("unsupported")
                    }
                    TextField(G("gold_report_notes","Gameplay notes"),text:$notes,axis:.vertical).lineLimit(3...8)
                    Button(G("public_report_title","Report a game result"),systemImage:"paperplane") {showingReport=true}
                    ShareLink(item:"LSX4 iPhone report\n\(game.title) · \(game.id)\nVersion: \(Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "")\nCPU: \(ConsoleExecutionMode.selected.label)\nResolution: \(ConsoleRenderResolution.selected.rawValue)\nResult: \(classification.isEmpty ? "unclassified" : classification)\n\(notes)") {Label(G("gold_share_report","Share report"),systemImage:"square.and.arrow.up")}
                }
                GoldGameRemovalSection(game:game,allowed:removalAllowed,busy:$removing)
            }.scrollContentBackground(.hidden).disabled(removing)
        }.onAppear {classification=UserDefaults.standard.string(forKey:"gold.status."+game.id) ?? "";notes=UserDefaults.standard.string(forKey:"gold.notes."+game.id) ?? "";DispatchQueue.main.async {initializing=false}}
            .onChange(of:classification) {_,value in
                UserDefaults.standard.set(value,forKey:"gold.status."+game.id)
                if !initializing && !value.isEmpty {showingReport=true}
            }
            .goldModal(isPresented:$showingReport) {GoldPublicReportView(game:game)}
            .onChange(of:notes) {_,value in UserDefaults.standard.set(value,forKey:"gold.notes."+game.id)}
    }
}

struct GoldAndroidEntry:Decodable,Identifiable {
    var id:String {image}
    let title:String,image:String,platform:String,notes:String
}
struct GoldAndroidCatalog:Decodable {
    let games:[GoldAndroidEntry],revision:String,captured:String
    static let bundled:Self? = {
        guard let url=Bundle.main.url(forResource:"compatibility_catalog",withExtension:"json"),let data=try? Data(contentsOf:url) else {return nil}
        return try? JSONDecoder().decode(Self.self,from:data)
    }()
}
struct GoldCompatibilityView:View {
    let games:[ConsoleTitle]
    @ObservedObject private var cache=LSX4CommunityCache.shared
    @State private var search=""
    @State private var selectedReport:LSX4CommunityReport?=nil
    @State private var showingReport=false
    @State private var resultFilter="playable"
    @State private var onlyMine=false
    @ObservedObject private var account=LSX4AccountSession.shared
    private var entries:[GoldObservation] {GoldObservation.all.filter {search.isEmpty || $0.title.localizedCaseInsensitiveContains(search)}}
    private var reports:[LSX4CommunityReport] {cache.reports.filter {$0.result == resultFilter && (!onlyMine || LSX4AccountCredentials.owns($0)) && (search.isEmpty || $0.gameTitle.localizedCaseInsensitiveContains(search) || $0.device.localizedCaseInsensitiveContains(search))}}
    init(games:[ConsoleTitle],cache:LSX4CommunityCache = .shared) {self.games=games;self._cache=ObservedObject(wrappedValue:cache)}
    var body:some View {
        let conflicts=LSX4ReportStatus.conflictingGames(cache.reports)
        return GoldPage(title:G("menu_compatibility_card","Tested compatible games")) {
            VStack(spacing:8) {
                Text(P("community_current_version","Current version: {version}",["version":LSX4ReportStatus.currentVersion]))
                    .font(.subheadline).frame(maxWidth:.infinity,alignment:.leading).padding(.horizontal)
                    .accessibilityIdentifier("community-current-version")
                HStack {Image(systemName:"magnifyingglass");TextField(L("Поиск игры"),text:$search).textFieldStyle(.plain)}.padding(12).goldGlass(radius:12).padding(.horizontal)
                HStack {
                    Picker(P("game_status","Result"),selection:$resultFilter) {Text(LSX4ReportedResult.playable.label).tag("playable");Text(LSX4ReportedResult.unplayable.label).tag("not-playable")}.pickerStyle(.segmented).accessibilityIdentifier("community-result-filter")
                    Toggle(P("reports_mine","Mine"),isOn:$onlyMine).fixedSize().accessibilityIdentifier("community-mine-filter")
                }.padding(.horizontal)
                ScrollView {
                    LazyVStack(alignment:.leading,spacing:10) {
                        NavigationLink {GoldMemberDirectory()} label: {Label(P("members_title","All users"),systemImage:"person.2")}
                        HStack {
                            Text(P("community_all_title","iOS community reports")).font(.headline)
                            Spacer()
                            if cache.loading {ProgressView()}
                            Button {Task {await cache.refresh()}} label: {Image(systemName:"arrow.clockwise")}.accessibilityLabel(P("community_refresh","Refresh community reports")).accessibilityIdentifier("community-refresh")
                        }
                        Text(P("community_scope_both","User submitted, unverified. Reports include the tested app version.")).font(.caption).foregroundStyle(.secondary)
                        if !cache.error.isEmpty {Text(cache.error).font(.caption).foregroundStyle(.orange).accessibilityIdentifier("community-refresh-error")}
                        if reports.isEmpty && !cache.loading {Text(search.isEmpty ? P("reports_empty","No reports yet") : P("community_no_matches","No matching community reports.")).font(.footnote)}
                        ForEach(reports) {report in
                            let mine=LSX4AccountCredentials.owns(report)
                            Button {selectedReport=report;showingReport=true} label: {
                                HStack(spacing:12) {
                                    GoldReportGameIcon(report:report)
                                    VStack(alignment:.leading,spacing:5) {
                                        Text(report.gameTitle).font(.headline)
                                        Text([report.device,"LSX4 "+report.appVersion,report.executionMode ?? ""].joined(separator:" · ")).font(.caption).foregroundStyle(.secondary)
                                        GoldReportStatusNotes(report:report,conflict:LSX4ReportStatus.hasConflict(report,in:conflicts))
                                        if mine {Text(P("community_mine","Your report · add screenshot or video")).font(.caption).foregroundStyle(.mint)}
                                    }.frame(maxWidth:.infinity,alignment:.leading)
                                    Image(systemName:"chevron.right").font(.caption)
                                }.padding(12).contentShape(Rectangle()).goldGlass(radius:14,selected:mine)
                                    .overlay(RoundedRectangle(cornerRadius:14).strokeBorder(mine ? Color.mint.opacity(0.8) : .clear,lineWidth:1.5).allowsHitTesting(false))
                            }.buttonStyle(.plain).accessibilityIdentifier("community-report-"+report.titleId)
                        }
                        if resultFilter == "playable" && !onlyMine {
                        Divider().padding(.vertical,8)
                        Text(P("community_previous","Previous iPhone gameplay observations")).font(.headline)
                        Text(G("gold_prior_scope","Previous gameplay observation, not a full-game guarantee or a new measurement.")).font(.caption).foregroundStyle(.secondary)
                        ForEach(entries) {entry in
                            NavigationLink {observationDetail(entry)} label: {observationRow(entry)}
                                .buttonStyle(.plain).accessibilityIdentifier("gold-compatible-\(entry.id)")
                        }
                        }
                    }.padding()
                }.refreshable {await cache.refresh()}
            }.navigationDestination(isPresented:$showingReport) {
                if let selectedReport {GoldCommunityReportDetail(report:selectedReport)}
            }
        }.task {await cache.refresh()}
    }
    private func observationDetail(_ entry:GoldObservation)->some View {
        ScrollView {VStack(alignment:.leading,spacing:14) {
            Text(entry.title).font(.title2)
            if let game=games.first(where:{$0.id == entry.id}) {GoldCover(game:game,hero:true,fit:true).frame(height:190)}
            Text("iPhone · JIT").font(.caption)
            Text(entry.fps > 0 ? String(format:"%.1f FPS",entry.fps) : "FPS —").font(.title).foregroundStyle(entry.fps > 0 ? Color.green : Color.secondary)
            Text(G("gold_prior_scope","Previous gameplay observation, not a full-game guarantee or a new measurement.")).font(.footnote)
            Text(entry.id+" · "+entry.build).font(.caption).foregroundStyle(.secondary)
        }.padding()}
    }
    private func observationRow(_ entry:GoldObservation)->some View {
        HStack(spacing:12) {
            if let game=games.first(where:{$0.id == entry.id}) {GoldCover(game:game).frame(width:44,height:44).clipShape(RoundedRectangle(cornerRadius:6))}
            Text(entry.title).frame(maxWidth:.infinity,alignment:.leading)
            VStack(alignment:.trailing) {Text(entry.fps > 0 ? String(format:"%.1f FPS",entry.fps) : "FPS —").foregroundStyle(entry.fps > 0 ? Color.green : Color.secondary);Text("iPhone · JIT").font(.caption2).foregroundStyle(.secondary)}
        }.padding(10).contentShape(Rectangle()).goldGlass(radius:12)
    }

}
struct GoldAndroidReferenceView:View {
    @State private var search=""
    var body:some View {
        List {
            Section {Text(G("gold_android_scope","These results and screenshots are from Android. They do not establish iPhone compatibility.")).font(.footnote)}
            ForEach((GoldAndroidCatalog.bundled?.games ?? []).filter {search.isEmpty || $0.title.localizedCaseInsensitiveContains(search)}) {entry in
                NavigationLink(entry.title) {
                    ScrollView {VStack(alignment:.leading,spacing:16) {
                        Text(entry.title).font(.title2);Text("Android · "+entry.platform).foregroundStyle(.secondary)
                        if let catalog=GoldAndroidCatalog.bundled,let url=URL(string:"https://raw.githubusercontent.com/llayerlinux/lsx4/\(catalog.revision)/\(entry.image)") {
                            AsyncImage(url:url) {phase in
                                switch phase {
                                case .success(let image):image.resizable().scaledToFit()
                                case .failure:ContentUnavailableView(G("menu_screenshot_unavailable","Screenshot unavailable"),systemImage:"photo")
                                default:ProgressView().frame(height:140)
                                }
                            }.frame(maxHeight:240)
                            Text(G("menu_screenshot_source","Gameplay screenshot from LSX4 on an Android device · GitHub")).font(.caption)
                            Link("GitHub · "+catalog.captured,destination:URL(string:"https://github.com/llayerlinux/lsx4/tree/\(catalog.revision)#playable-games")!)
                        }
                        GoldAndroidPerformance(image:entry.image)
                        if !entry.notes.isEmpty {Text(entry.notes)}
                        Link(G("menu_catalog_video","YouTube · Video"),destination:URL(string:"https://www.youtube.com/watch?v=jRe0-FwPfY0")!)
                    }.padding()}.navigationTitle(entry.title).navigationBarTitleDisplayMode(.inline)
                }
            }
        }.searchable(text:$search).navigationTitle(G("gold_android_reference","Android reference catalogue"))
    }
}
private struct GoldAndroidPerformance:View {
    let image:String
    private static let data:[String:Any] = {
        guard let url=Bundle.main.url(forResource:"compatibility_performance",withExtension:"json"),let data=try? Data(contentsOf:url) else {return [:]}
        return (try? JSONSerialization.jsonObject(with:data) as? [String:Any]) ?? [:]
    }()
    private var samples:[[String:Any]] {(Self.data["samples"] as? [String:[[String:Any]]])?[URL(fileURLWithPath:image).lastPathComponent] ?? []}
    var body:some View {
        VStack(alignment:.leading,spacing:8) {
            Text("Snapdragon 8 Gen 5 · Adreno 840").font(.headline)
            if let low=samples.compactMap({$0["fps"] as? Int}).min(),let high=samples.compactMap({$0["fps"] as? Int}).max() {
                Text("\(low)–\(high) FPS").foregroundStyle(.green)
                Text(G("menu_fps_sample_scope","Individual gameplay HUD readings, not an average or a stable range.")).font(.caption)
                ForEach(Array(samples.enumerated()),id:\.offset) {_,sample in
                    if let video=sample["video"] as? String,let second=sample["second"] as? Int,let fps=sample["fps"] as? Int,let url=URL(string:"https://www.youtube.com/watch?v=\(video)&t=\(second)s") {Link("\(second / 60):\(String(format:"%02d",second % 60)) · \(fps) FPS",destination:url).font(.caption)}
                }
            } else {Text(G("menu_fps_unpublished","Not published in the source")).font(.caption)}
            if let estimate=Self.data["gen3_user_estimate"] as? [String:Any] {
                let file=URL(fileURLWithPath:image).lastPathComponent
                let small=(estimate["small_indie"] as? [String] ?? []).contains(file)
                let aaa=(estimate["aaa"] as? [String] ?? []).contains(file)
                let values=samples.compactMap {$0["fps"] as? Int}
                if small,let range=estimate["small_indie_fps"] as? [Int],range.count==2 {
                    Text("Snapdragon 8 Gen 3 · Adreno 750").font(.headline)
                    Text("≈ \(range[0])–\(range[1]) FPS · "+G("menu_fps_estimate","Tester estimate"))
                    Text(G("menu_fps_indie_policy","Tester guidance for small indie games; not a per-title measurement on this device.")).font(.caption)
                } else if aaa,let low=values.min(),let high=values.max() {
                    let delta=estimate["aaa_delta_fps"] as? Int ?? -10
                    Text("Snapdragon 8 Gen 3 · Adreno 750").font(.headline)
                    Text("≈ \(max(0,low+delta))–\(max(0,high+delta)) FPS · "+G("menu_fps_estimate","Tester estimate"))
                    Text(G("menu_fps_aaa_policy","Tester estimate: %1$s FPS adjustment to the video samples; not a measurement on this device.").replacingOccurrences(of:"%1$s",with:String(delta))).font(.caption)
                }
            }
        }
    }
}

struct GoldCacheEntry:Identifiable,Sendable {
    let id:String,urls:[URL],bytes:Int64,files:Int
}
enum GoldCacheStore {
    static var roots:[URL] {
        let base=URL.documentsDirectory.appendingPathComponent("shared-runtime",isDirectory:true)
        return [base.appendingPathComponent("cache/jit",isDirectory:true),base.appendingPathComponent("native/cache",isDirectory:true)]
    }
    static func scan()->[GoldCacheEntry] {
        var entries:[String:(urls:[URL],bytes:Int64,files:Int)]=[:]
        for root in roots {
            guard (try? root.resourceValues(forKeys:[.isSymbolicLinkKey])).map({$0.isSymbolicLink != true}) == true else {continue}
            for child in (try? FileManager.default.contentsOfDirectory(at:root,includingPropertiesForKeys:[.isSymbolicLinkKey])) ?? [] {
                guard valid(child,under:root) else {continue}
                var bytes:Int64=0,files=0,nodes=0
                if let value=try? child.resourceValues(forKeys:[.isRegularFileKey,.fileSizeKey]),value.isRegularFile == true {bytes=Int64(value.fileSize ?? 0);files=1}
                else if let items=FileManager.default.enumerator(at:child,includingPropertiesForKeys:[.isRegularFileKey,.isSymbolicLinkKey,.fileSizeKey],options:[.skipsPackageDescendants]) {
                    for case let file as URL in items {
                        nodes+=1;if nodes>200_000 {break}
                        guard let value=try? file.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey,.fileSizeKey]) else {continue}
                        if value.isSymbolicLink == true {items.skipDescendants();continue}
                        if value.isRegularFile == true {bytes+=Int64(value.fileSize ?? 0);files+=1}
                    }
                }
                let id=child.lastPathComponent
                var entry=entries[id] ?? ([],0,0);entry.urls.append(child);entry.bytes+=bytes;entry.files+=files;entries[id]=entry
            }
        }
        return entries.map {GoldCacheEntry(id:$0.key,urls:$0.value.urls,bytes:$0.value.bytes,files:$0.value.files)}.sorted {$0.bytes>$1.bytes}
    }
    static func valid(_ child:URL,under root:URL)->Bool {
        let expected=root.standardizedFileURL
        guard roots.map(\.standardizedFileURL).contains(expected),
              expected.resolvingSymlinksInPath().path.hasPrefix(URL.documentsDirectory.resolvingSymlinksInPath().path+"/"),
              child.standardizedFileURL.deletingLastPathComponent() == expected,
              child.resolvingSymlinksInPath().deletingLastPathComponent() == expected.resolvingSymlinksInPath(),
              child.lastPathComponent.range(of:"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$",options:.regularExpression) != nil,
              let values=try? child.resourceValues(forKeys:[.isSymbolicLinkKey]),values.isSymbolicLink != true else {return false}
        return true
    }
    static func remove(_ entry:GoldCacheEntry) throws {
        for child in entry.urls {
            let root=child.deletingLastPathComponent()
            guard valid(child,under:root) else {throw CocoaError(.fileWriteNoPermission)}
        }
        for child in entry.urls {try FileManager.default.removeItem(at:child)}
    }
}
struct GoldCacheView:View {
    let games:[ConsoleTitle]
    var body:some View {GoldPage(title:G("menu_render_cache","Render cache")) {GoldCacheContents(games:games)}}
}
private struct GoldCacheCube:View {
    let game:ConsoleTitle
    var selected:Bool
    @State private var image:UIImage?
    @State private var visible=false
    @State private var applicationActive=false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body:some View {
        GoldCacheCubeSurface(image:image,selected:selected,animated:visible && applicationActive && !reduceMotion)
            .frame(height:112).allowsHitTesting(false).accessibilityHidden(true)
            .onAppear {visible=true;applicationActive=UIApplication.shared.applicationState == .active}.onDisappear {visible=false}
            .onReceive(NotificationCenter.default.publisher(for:UIApplication.didBecomeActiveNotification)) {_ in applicationActive=true}
            .onReceive(NotificationCenter.default.publisher(for:UIScene.didActivateNotification)) {_ in applicationActive=true}
            .onReceive(NotificationCenter.default.publisher(for:UIScene.willDeactivateNotification)) {_ in applicationActive=false}
            .onReceive(NotificationCenter.default.publisher(for:UIApplication.willResignActiveNotification)) {_ in applicationActive=false}
            .task(id:GoldArtwork.url(game)?.path) {
                guard let url=GoldArtwork.url(game) else {image=nil;return}
                let loaded=await Task.detached(priority:.utility) {GoldArtwork.load(url,large:false)}.value
                if !Task.isCancelled {image=loaded}
            }
    }
}
private struct GoldCacheCubeSurface:UIViewRepresentable {
    let image:UIImage?
    let selected:Bool,animated:Bool
    func makeUIView(context:Context)->GoldCacheCubeLayerView {GoldCacheCubeLayerView()}
    func updateUIView(_ view:GoldCacheCubeLayerView,context:Context) {view.configure(image:image,selected:selected,animated:animated)}
}
private final class GoldCacheCubeLayerView:UIView {
    private let camera=CALayer(),cube=CATransformLayer(),shadow=CAShapeLayer()
    private var faces=[CALayer]()
    override init(frame:CGRect) {
        super.init(frame:frame);isUserInteractionEnabled=false
        layer.addSublayer(shadow);layer.addSublayer(camera);camera.addSublayer(cube)
        for i in 0..<6 {
            let face=CALayer();face.isDoubleSided=false;face.borderWidth=1
            face.borderColor=UIColor(red:0.51,green:0.69,blue:0.88,alpha:0.4).cgColor
            face.backgroundColor=UIColor(red:i<2 ? 0.15 : 0.09,green:i<2 ? 0.29 : 0.19,blue:i<2 ? 0.46 : 0.31,alpha:1).cgColor
            face.contentsGravity = .resizeAspectFill;face.masksToBounds=true;cube.addSublayer(face);faces.append(face)
        }
    }
    required init?(coder:NSCoder){fatalError("init(coder:) has not been implemented")}
    func configure(image:UIImage?,selected:Bool,animated:Bool) {
        CATransaction.begin();CATransaction.setDisableActions(true)
        let artwork:CGImage?
        if let image {artwork=image.cgImage}
        else {
            artwork=UIGraphicsImageRenderer(size:CGSize(width:128,height:128)).image {_ in
                UIImage(systemName:"cube.transparent.fill")?.withTintColor(.white).draw(in:CGRect(x:26,y:26,width:76,height:76))
            }.cgImage
        }
        faces[0].contents=artwork;faces[1].contents=artwork
        shadow.fillColor=(selected ? UIColor.systemBlue.withAlphaComponent(0.3) : UIColor.black.withAlphaComponent(0.25)).cgColor
        if animated {
            if cube.animation(forKey:"cache-turn")==nil {
                let motion=CAKeyframeAnimation(keyPath:"transform")
                motion.values=(0...90).map {step -> NSValue in
                    let angle=Double(step)/90*Double.pi*2
                    var transform=CATransform3DMakeRotation(CGFloat(angle),0,1,0)
                    transform=CATransform3DRotate(transform,CGFloat(0.17*sin(angle)),1,0,0)
                    return NSValue(caTransform3D:transform)
                }
                motion.duration=10;motion.repeatCount = .infinity;motion.calculationMode = .linear
                cube.add(motion,forKey:"cache-turn")
            }
        } else {cube.removeAnimation(forKey:"cache-turn")}
        cube.transform=CATransform3DRotate(CATransform3DMakeRotation(-0.35,0,1,0),0.12,1,0,0)
        CATransaction.commit();setNeedsLayout()
    }
    override func layoutSubviews() {
        super.layoutSubviews();CATransaction.begin();CATransaction.setDisableActions(true)
        camera.frame=bounds;var perspective=CATransform3DIdentity;perspective.m34 = -1/500;camera.sublayerTransform=perspective
        cube.position=CGPoint(x:bounds.midX,y:bounds.height*0.45)
        let side=min(94,bounds.width*0.64,bounds.height*0.78),depth=side*0.36
        shadow.path=UIBezierPath(ovalIn:CGRect(x:bounds.midX-side*0.5,y:bounds.height-13,width:side,height:10)).cgPath
        for i in 0..<6 {
            let width=(2...3).contains(i) ? depth : side,height=i>=4 ? depth : side
            faces[i].bounds=CGRect(x:0,y:0,width:width,height:height);faces[i].position = .zero
            var transform:CATransform3D
            switch i {
            case 0:transform=CATransform3DMakeTranslation(0,0,depth/2)
            case 1:transform=CATransform3DRotate(CATransform3DMakeTranslation(0,0,-depth/2),.pi,0,1,0)
            case 2:transform=CATransform3DRotate(CATransform3DMakeTranslation(-side/2,0,0),-.pi/2,0,1,0)
            case 3:transform=CATransform3DRotate(CATransform3DMakeTranslation(side/2,0,0),.pi/2,0,1,0)
            case 4:transform=CATransform3DRotate(CATransform3DMakeTranslation(0,-side/2,0),.pi/2,1,0,0)
            default:transform=CATransform3DRotate(CATransform3DMakeTranslation(0,side/2,0),-.pi/2,1,0,0)
            }
            faces[i].transform=transform
        }
        CATransaction.commit()
    }
}
struct GoldCacheContents:View {
    let games:[ConsoleTitle]
    @State private var entries:[GoldCacheEntry]=[]
    @State private var selected:GoldCacheEntry?
    @State private var confirm=false
    @State private var busy=false
    @State private var error:String?
    var body:some View {
        ScrollView {
            VStack(alignment:.leading,spacing:16) {
                Text(ByteCountFormatter.string(fromByteCount:entries.reduce(0) {$0+$1.bytes},countStyle:.file)).font(.title2)
                Text(G("gold_cache_scope","Compiled render and CPU caches. Game files and saves are kept.")).font(.caption).foregroundStyle(.secondary)
                if busy {ProgressView()}
                if entries.isEmpty && !busy {ContentUnavailableView(G("gold_cache_empty","No render cache yet"),systemImage:"cube.transparent")}
                LazyVGrid(columns:[GridItem(.adaptive(minimum:130))],spacing:14) {ForEach(entries) {entry in
                    Button {selected=entry} label: {
                        VStack(spacing:8) {GoldCacheCube(game:games.first(where:{$0.id == entry.id}) ?? ConsoleTitle(id:entry.id,title:entry.id,androidStatus:"",bytes:0),selected:selected?.id == entry.id);Text(games.first(where:{$0.id == entry.id})?.title ?? entry.id).font(.caption).lineLimit(2);Text(ByteCountFormatter.string(fromByteCount:entry.bytes,countStyle:.file)).font(.caption2)}
                            .frame(maxWidth:.infinity,minHeight:120).padding(10).goldGlass(radius:14,selected:selected?.id == entry.id)
                            .overlay(alignment:.topTrailing) {
                                if selected?.id == entry.id {Image(systemName:"checkmark.circle.fill").padding(10).accessibilityHidden(true).allowsHitTesting(false)}
                            }.contentShape(RoundedRectangle(cornerRadius:14))
                    }.buttonStyle(.plain).disabled(busy).accessibilityIdentifier("cache-entry-"+entry.id)
                        .accessibilityValue(selected?.id == entry.id ? G("menu_selected","Selected") : "")
                }}
                if let error {Text(error).foregroundStyle(.red).font(.caption)}
            }.padding(18)
        }.navigationTitle(G("menu_render_cache","Render cache"))
            .safeAreaInset(edge:.bottom,spacing:0) {
                if let selected {
                    VStack(alignment:.leading,spacing:8) {
                        Text(selected.id).font(.headline).accessibilityIdentifier("cache-selection")
                        Text(P("cache_files","{count} files",["count":String(selected.files)])).font(.caption)
                        Button(G("cache_clear","Clear selected cache"),role:.destructive) {confirm=true}.buttonStyle(.bordered).disabled(busy).accessibilityIdentifier("cache-clear-selected")
                            .confirmationDialog(G("gold_cache_confirm","Clear the selected compiled cache?"),isPresented:$confirm,titleVisibility:.visible) {
                                Button(G("cache_clear","Clear selected cache"),role:.destructive) {clearSelected()}.accessibilityIdentifier("cache-confirm-clear")
                            }
                    }.frame(maxWidth:.infinity,alignment:.leading).padding(14).goldGlass(radius:16).padding(.horizontal,18).padding(.bottom,8)
                }
            }.task {await reload()}
    }
    private func clearSelected() {
        guard let selected,!busy else {return};busy=true;error=nil
        Task {do {try await Task.detached(priority:.utility) {try GoldCacheStore.remove(selected)}.value;self.selected=nil;await reload()} catch {self.error=error.localizedDescription;busy=false}}
    }
    @MainActor private func reload() async {busy=true;entries=await Task.detached(priority:.utility) {GoldCacheStore.scan()}.value;busy=false}
}
