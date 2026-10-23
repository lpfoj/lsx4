// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

struct ConsoleLanguage: Identifiable {
    let id: String
    let label: String
    var flag:String {["en":"🇬🇧","es":"🇪🇸","fr":"🇫🇷","de":"🇩🇪","pt-BR":"🇧🇷","ru":"🇷🇺","zh-Hans":"🇨🇳"][id] ?? "🌐"}
}
enum ConsoleLocalization {
    // Preserve existing languages. Russian stays immediately before Chinese.
    static let languages = [ConsoleLanguage(id:"en",label:"English"),
        ConsoleLanguage(id:"de",label:"Deutsch"), ConsoleLanguage(id:"es",label:"Español"),
        ConsoleLanguage(id:"fr",label:"Français"), ConsoleLanguage(id:"pt-BR",label:"Português (Brasil)"),
        ConsoleLanguage(id:"ru",label:"Русский"), ConsoleLanguage(id:"zh-Hans",label:"简体中文")]
    static var selected: String {
        let stored=UserDefaults.standard.string(forKey:"ui_language") ?? "en"
        return languages.contains(where:{$0.id == stored}) ? stored : "en"
    }
    static let strings: [String:[String:String]] = {
        guard let url=Bundle.main.url(forResource:"ConsoleStrings",withExtension:"json"),
              let data=try? Data(contentsOf:url),
              let result=try? JSONDecoder().decode([String:[String:String]].self,from:data) else { return [:] }
        return result
    }()
    static func text(_ key: String, _ values: [String:String] = [:]) -> String {
        var result=LSX4AppText.lookup(key) ?? strings[selected]?[key] ?? strings["en"]?[key] ?? key
        for (name,value) in values { result=result.replacingOccurrences(of:"{"+name+"}",with:value) }
        return result
    }
}
func L(_ key: String, _ values: [String:String] = [:]) -> String {
    ConsoleLocalization.text(key,values)
}

enum ConsoleExecutionMode: String, CaseIterable, Identifiable {
    case jit = "jit"
    var id: String { rawValue }
    var label: String { "JIT" }
    static var installed: Self { .jit }
    static var selected: Self { .jit }
}


enum ConsoleRenderResolution: String, CaseIterable, Identifiable {
    case fullHD = "1080p", hd = "720p", compact = "544p", low = "360p"
    var id: String { rawValue }
    var dimensions: (Int, Int) {
        switch self {
        case .fullHD: return (1920,1080)
        case .hd: return (1280,720)
        case .compact: return (960,544)
        case .low: return (640,360)
        }
    }
    var label: String { "\(rawValue) · \(dimensions.0) × \(dimensions.1)" }
    static var selected: Self {
        UserDefaults.standard.string(forKey:"lsx4.renderResolution").flatMap(Self.init(rawValue:)) ?? .compact
    }
}

// One persisted selection drives both the UIKit/Metal surface and the existing
// runtime display configuration. No renderer ABI or guest-memory path changes.
@objc(LSX4RenderResolutionSettings) @MainActor
public final class LSX4RenderResolutionSettings: NSObject {
    @objc public static var width: Int { ConsoleRenderResolution.selected.dimensions.0 }
    @objc public static var height: Int { ConsoleRenderResolution.selected.dimensions.1 }
    @objc public static func prepareForGame() -> String? {
        let selected=ConsoleRenderResolution.selected
        let (width,height)=selected.dimensions
        let directory=URL.documentsDirectory.appendingPathComponent("shared-runtime/runtime-config",isDirectory:true)
        // Version2 is supported by the actual linked runtime. Equal source and
        // output plus upscaler OFF avoids a hidden second resolution step.
        let ini="version=2\nfsr_enabled=0\nsource_width=\(width)\nsource_height=\(height)\noutput_width=\(width)\noutput_height=\(height)\n"
        do {
            try FileManager.default.createDirectory(at:directory,withIntermediateDirectories:true)
            try Data(ini.utf8).write(to:directory.appendingPathComponent("android-render-resolution.ini"),options:.atomic)
            let receipt: [String:Any] = ["preset":selected.rawValue,"outputWidth":width,"outputHeight":height,
                "requestedGameWidth":width,"requestedGameHeight":height,"timestamp":Date().timeIntervalSince1970,
                "bundleVersion":Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "",
                "stage":"configured-before-core"]
            try JSONSerialization.data(withJSONObject:receipt,options:.sortedKeys).write(
                to:URL.documentsDirectory.appendingPathComponent("render-resolution-status.json"),options:.atomic)
            NSLog("[APPLE_RENDER_RESOLUTION] preset=%@ output=%dx%d requestedGame=%dx%d",selected.rawValue,width,height,width,height)
            return nil
        } catch { return L("Не удалось применить разрешение игры.") }
    }
}

struct ConsoleExecutionSettings: View {
    @Environment(\.dismiss) private var dismiss
#if LSX4_DEVICE_LOCAL_JIT
    @State private var showingJITSetup = false
#endif
    @AppStorage("lsx4.executionMode") private var storedMode = ""
    @AppStorage("lsx4.showExecutionMode") private var showExecutionMode = false
    @AppStorage("ui_language") private var language = "ru"
    @AppStorage("lsx4.renderResolution") private var renderResolution = "544p"
    private var renderSelection: Binding<String> {
        Binding(get: { ConsoleRenderResolution(rawValue:renderResolution)?.rawValue ?? "544p" },
                set: { renderResolution = $0 })
    }
    private var selection: Binding<String> {
        Binding(get: { ConsoleExecutionMode(rawValue: storedMode)?.rawValue ?? ConsoleExecutionMode.installed.rawValue },
                set: { storedMode = $0 })
    }
    private var canSwitchMode: Bool { false }
    var body: some View {
        NavigationStack {
            Form {
                Section(L("Общие")) {
                    Picker(L("Язык приложения"),selection:$language) {
                        ForEach(ConsoleLocalization.languages) { language in
                            Text(language.label).tag(language.id).accessibilityIdentifier("language-"+language.id)
                        }
                    }.accessibilityIdentifier("ui-language")
                }
                Section(L("Исполнение CPU")) {
                    if canSwitchMode {
                    Picker(L("Режим"), selection: selection) {
                        ForEach(ConsoleExecutionMode.allCases) { mode in Text(mode.label).tag(mode.rawValue) }
                    }.pickerStyle(.segmented).accessibilityIdentifier("cpu-execution-mode")
                    LabeledContent(L("Активная сборка"), value: ConsoleExecutionMode.installed.label)
                    Text(L("Выбор применяется при следующем запуске игры. При смене режима игра запускается заново; сохранения и библиотека остаются доступны."))
                    Text(L("jit.setup.automatic"))
                    } else {
                        LabeledContent(L("Активная сборка"), value: "JIT")
                        Text(L("jit.setup.automatic"))
                    }
#if LSX4_DEVICE_LOCAL_JIT
                    Button(L("jit.setup.title")) { showingJITSetup = true }
                        .accessibilityIdentifier("settings-jit-setup")
#endif
                }
                Section(L("Графика")) {
                    Picker(L("Разрешение игры и вывода"),selection:renderSelection) {
                        ForEach(ConsoleRenderResolution.allCases) { resolution in
                            Text(resolution.label).tag(resolution.rawValue)
                        }
                    }.accessibilityIdentifier("game-render-resolution")
                    Text(L("Меньшее разрешение снижает нагрузку на графику и чёткость. Применяется при следующем запуске игры."))
                    Text(L("Игры с фиксированным внутренним разрешением могут менять только размер выходного кадра."))
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section(L("Отображение")) {
                    Toggle(L("Показывать JIT"), isOn: $showExecutionMode)
                        .accessibilityIdentifier("show-execution-mode")
                    Text(L("Название LSX4 и FPS остаются видны. Режим исполнения показывается только при включённом переключателе."))
                }
                Section(L("Сравнение")) {
                    Text(L("Сравнивайте одну игру, сохранение и сцену при одинаковых настройках звука и изображения."))
                }
            }.navigationTitle(L("Настройки")).toolbar {
                ToolbarItem(placement: .confirmationAction) { Button(L("Готово")) { dismiss() }.accessibilityIdentifier("settings-done") }
            }
#if LSX4_DEVICE_LOCAL_JIT
            .sheet(isPresented: $showingJITSetup) { LSX4JITSetupView() }
#endif
        }.environment(\.locale,Locale(identifier:language)).preferredColorScheme(.dark)
    }
}

enum LibraryStyle {
    static func color(_ value: UInt32) -> Color {
        Color(red: Double((value >> 16) & 255)/255, green: Double((value >> 8) & 255)/255, blue: Double(value & 255)/255)
    }
    static let background=color(0x111417), card=color(0x1b2026), stroke=color(0x2a313a)
    static let accent=color(0x6fb4ff), text=color(0xeef2f6), secondary=color(0x9aa6b2)
}
struct ConsoleTitle: Codable, Identifiable {
    let id: String
    let title: String
    let androidStatus: String
    let bytes: Int64
    var folder: URL? { LSX4PkgInstaller.existingGameFolder(id:id) }
    var icon: UIImage? {
        if let folder, let image=UIImage(contentsOfFile:folder.appendingPathComponent("sce_sys/icon0.png").path) { return image }
        let url=Bundle.main.bundleURL.appendingPathComponent("GameCatalog/\(id)/icon0.png")
        return UIImage(contentsOfFile: url.path)
    }
}
enum ConsoleGameplayPriority {
    private static let rank: [String:Int] = {
        let url=Bundle.main.bundleURL.appendingPathComponent("GameCatalog/jit-playable-order.json")
        guard let data=try? Data(contentsOf:url),
              let ids=try? JSONDecoder().decode([String].self,from:data) else { return [:] }
        var result: [String:Int]=[:]
        for (index,id) in ids.enumerated() where result[id] == nil { result[id]=index }
        return result
    }()
    static func ordered(_ games: [ConsoleTitle]) -> [ConsoleTitle] {
        games.enumerated().sorted {
            let left=rank[$0.element.id] ?? Int.max
            let right=rank[$1.element.id] ?? Int.max
            return left == right ? $0.offset < $1.offset : left < right
        }.map { $0.element }
    }
}
@MainActor final class ConsoleLibraryModel: ObservableObject {
#if LSX4_DEVICE_LOCAL_JIT
    @Published var showingJITSetup = false
#endif
    private var bundledGames: [ConsoleTitle]=[]
    @Published var games: [ConsoleTitle]=[]
    @Published var pending: String?
    @Published var status=""
    @Published var notice: String?
    @Published var revision=0
    private var pendingSince: Date?
    private var lastHostUpdate: Date?
    private var lastFailureID: String?
    func refresh() {
        if bundledGames.isEmpty, let data=try? Data(contentsOf: Bundle.main.bundleURL.appendingPathComponent("GameCatalog/catalog.json")),
           let list=try? JSONDecoder().decode([ConsoleTitle].self,from:data) { bundledGames=list }
        var merged=bundledGames
        for imported in LSX4PkgInstaller.importedGames() {
            if let index=merged.firstIndex(where:{$0.id == imported.id}) {
                let prior=merged[index]
                merged[index]=ConsoleTitle(id:imported.id,title:imported.title,androidStatus:prior.androidStatus,bytes:imported.bytes>0 ? imported.bytes : prior.bytes)
            } else {
                merged.append(ConsoleTitle(id:imported.id,title:imported.title,androidStatus:"unknown",bytes:imported.bytes))
            }
        }
        games=ConsoleGameplayPriority.ordered(merged)
        let docs=URL.documentsDirectory
        if let data=try? Data(contentsOf: docs.appendingPathComponent("library-status.json")),
           let state=try? JSONSerialization.jsonObject(with:data) as? [String:Any] {
            let requestID=state["requestID"] as? String
            let stamp=Date(timeIntervalSince1970:state["timestamp"] as? Double ?? 0)
            if requestID == pending, pending != nil {
                status=state["message"] as? String ?? status
                lastHostUpdate=stamp
                if state["stage"] as? String == "failed" { notice=status;pending=nil;lastFailureID=requestID }
            } else if pending == nil, state["stage"] as? String == "failed",
                      requestID != lastFailureID, Date().timeIntervalSince(stamp)<120 {
                lastFailureID=requestID;notice=state["message"] as? String
            }
        }
        if pending != nil, let start=pendingSince {
            if lastHostUpdate == nil, Date().timeIntervalSince(start)>30 {
                cancelLaunch()
                notice=L("Mac не ответил на запуск. Убедитесь, что он доступен по Wi-Fi, и повторите.")
            } else if let update=lastHostUpdate, Date().timeIntervalSince(update)>3700 {
                cancelLaunch();notice=L("Запуск не завершился. Повторите попытку.")
            }
        }
        revision += 1
    }
    func launch(_ game: ConsoleTitle) {
        guard pending == nil else { return }
        if game.id.hasPrefix("PPSA") {
            notice="\(game.title): PS5 Current is not connected to this iOS build yet. Android status does not establish iPhone compatibility."
            return
        }
#if LSX4_DEVICE_LOCAL_JIT
        if ConsoleExecutionMode.selected == .jit {
            guard game.folder != nil else { notice=L("Сначала импортируйте файлы игры на iPhone."); return }
            if LSX4JITInstallation.problem != nil { showingJITSetup = true; return }
            guard let launch=LSX4LibraryControllerFactory.localLaunchHandler else { notice=L("Запуск пока недоступен."); return }
            notice=launch(game.id); return
        }
#endif
        do {
            pending=try LSX4LibraryControllerFactory.writeRequest(action:"launch",title:game.id)
            pendingSince=Date();lastHostUpdate=nil
            status=game.folder == nil ? "Copying \(game.title) from Vivo…" : "Launching \(game.title)…"
        } catch { notice=error.localizedDescription }
    }
    func cancelLaunch() {
        _=try? LSX4LibraryControllerFactory.writeRequest(action:"cancel")
        pending=nil;pendingSince=nil;lastHostUpdate=nil;status=""
    }
    func publishInventory() {
        let available=(try? URL.documentsDirectory.resourceValues(forKeys:[.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
        let state:[String:Any]=["timestamp":Date().timeIntervalSince1970,"games":games.count,
            "androidPlayable":games.filter{$0.androidStatus == "supported"}.count,
            "installed":games.filter{$0.folder != nil}.map(\.id),"availableBytes":available,
            "idleTimerDisabled":UIApplication.shared.isIdleTimerDisabled]
        if let data=try? JSONSerialization.data(withJSONObject:state,options:[.prettyPrinted,.sortedKeys]) {
            try? data.write(to:URL.documentsDirectory.appendingPathComponent("library-inventory.json"),options:.atomic)
        }
    }
}
@objc(LSX4LibraryControllerFactory) @MainActor public final class LSX4LibraryControllerFactory: NSObject {
#if LSX4_DEVICE_LOCAL_JIT
    @objc public static var localLaunchHandler: ((String) -> String?)?
#endif
    @objc public static func makeController() -> UIViewController { UIHostingController(rootView:GoldLibraryView()) }
    static func writeRequest(action:String,title:String="") throws -> String {
        let id=UUID().uuidString
        var request:[String:Any]=["id":id,"action":action,"title":title,"timestamp":Date().timeIntervalSince1970]
        if action == "launch" { request["executionMode"] = ConsoleExecutionMode.selected.rawValue }
        try JSONSerialization.data(withJSONObject:request,options:.sortedKeys).write(to:URL.documentsDirectory.appendingPathComponent("library-request.json"),options:.atomic)
        return id
    }
    @objc public static func requestLibrary() {
        do { _=try writeRequest(action:"library") }
        catch { NSLog("LSX4 return to library: %@",error.localizedDescription) }
    }
}
