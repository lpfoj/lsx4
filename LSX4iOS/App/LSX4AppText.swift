// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
enum LSX4AppText {
    static let languages=["en","es","fr","de","pt-BR","ru","zh-Hans"]
    static var language:String {
        let raw=UserDefaults.standard.string(forKey:"ui_language") ?? "en"
        let value=raw == "zh-CN" ? "zh-Hans" : raw
        return languages.contains(value) ? value : "en"
    }
    static let strings:[String:[String:String]] = {
        guard let file=Bundle.main.url(forResource:"ReleaseStrings",withExtension:"json"),let data=try? Data(contentsOf:file),
              let value=try? JSONDecoder().decode([String:[String:String]].self,from:data) else {return [:]}
        return value
    }()
    static func lookup(_ key:String)->String? {strings[language]?[key] ?? strings["en"]?[key]}
}
func P(_ key:String,_ fallback:String,_ values:[String:String]=[:])->String {
    var value=LSX4AppText.lookup(key) ?? fallback
    for (key,replacement) in values {value=value.replacingOccurrences(of:"{"+key+"}",with:replacement)}
    return value
}
enum LSX4ReportSharing {
    static var key:String {"lsx4.reportSharing.enabled.v1"+(LSX4CommunityAPI.fixture ? ".fixture" : "")}
    static var enabled:Bool {UserDefaults.standard.object(forKey:key) as? Bool ?? true}
    struct Disabled:LocalizedError {
        var errorDescription:String? {P("sharing_off_message","Sending is off. Marks are saved on this iPhone. Enable report sharing in Settings to publish.")}
    }
    static func requireEnabled() throws {if !enabled {throw Disabled()}}
}
