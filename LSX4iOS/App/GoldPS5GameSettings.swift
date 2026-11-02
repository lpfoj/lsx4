import SwiftUI

struct GoldPS5GameSettings:View {
    let title:String
    @State private var revision=0
    private var scope:String {PS5SettingsStore.gamePrefix(title) ?? PS5SettingsStore.prefix+"game.invalid."}
    private func binding<T>(_ key:String,_ fallback:T)->Binding<T> {
        Binding(get:{PS5SettingsStore.setting(key,title:title) as? T ?? fallback},set:{UserDefaults.standard.set($0,forKey:scope+key);revision+=1})
    }
    var body:some View {
        let _=revision
        return Form {
            Section {
                Label(P("game_preset_auto","Automatic game profile"),systemImage:"checkmark.seal").foregroundStyle(.mint)
                Text(P("game_preset_scope","Recommended compatibility and performance settings apply automatically. Overrides here affect only this game after restarting it.")).font(.footnote)
            }
            Section(P("game_default_preset","Default preset")) {
                LabeledContent(G("output_resolution","Output resolution"),value:PS5SettingsStore.setting("resolution") as? String ?? "544p")
                LabeledContent(G("cpu_preset","CPU preset"),value:[G("compatibility","Compatibility"),G("balanced","Balanced"),G("performance","Performance")][min(2,max(0,PS5SettingsStore.value("cpu.preset",fallback:1)))])
                Text(P("game_defaults_inherit","Unchanged options inherit your global PS5 settings. Reset removes only this game’s overrides.")).font(.footnote).foregroundStyle(.secondary)
            }
            Section(G("category_graphics","Graphics")) {
                Picker(G("output_resolution","Output resolution"),selection:binding("resolution","544p")) {
                    Text("640 × 360").tag("360p");Text("960 × 544").tag("544p");Text("1280 × 720").tag("720p");Text("1920 × 1080").tag("1080p")
                }.accessibilityIdentifier("game-ps5-resolution")
            }
            Section("CPU") {
                Picker(G("cpu_preset","CPU preset"),selection:binding("cpu.preset",1)) {
                    Text(G("compatibility","Compatibility")).tag(0);Text(G("balanced","Balanced")).tag(1);Text(G("performance","Performance")).tag(2)
                }
                Picker(G("guest_workers","Guest workers"),selection:binding("cpu.workers",0)) {Text(G("automatic","Automatic")).tag(0);ForEach(1...8,id:\.self) {Text(String($0)).tag($0)}}
            }
            Section(G("category_managed_optimizations","Managed optimizations")) {
                ForEach(PS5ManagedOption.all) {option in Toggle(G(option.key,option.title),isOn:binding("optimization.\(option.id)",option.defaultEnabled))}
            }
            Button(G("reset","Reset"),role:.destructive) {PS5SettingsStore.resetGame(title);revision+=1}.accessibilityIdentifier("game-ps5-reset")
        }.scrollContentBackground(.hidden).goldCommunityPage().navigationTitle(G("ps5_preset_title","PS5 Settings"))
    }
}
