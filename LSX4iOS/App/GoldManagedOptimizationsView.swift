// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct GoldManagedOptimizationsView: View {
    @StateObject private var store: IOSManagedOptimizationStore
    let gameTitle: String?
    init(title: String? = nil, gameTitle: String? = nil) {
        _store = StateObject(wrappedValue:IOSManagedOptimizationStore(title:title))
        self.gameTitle = gameTitle
    }
    private func row(_ option: IOSManagedOption) -> some View {
        VStack(alignment:.leading,spacing:6) {
            if option.available {
                Toggle(G(option.labelKey,option.label),isOn:Binding(
                    get:{store.checked(option)},set:{store.setChecked($0,option:option)}))
                    .accessibilityIdentifier("managed-"+option.key)
                let hint=G(option.hintKey,"")
                if !hint.isEmpty {Text(hint).font(.footnote).foregroundStyle(.secondary)}
            } else {
                LabeledContent(G(option.labelKey,option.label),value:G("managed_ios_unavailable","Android only"))
                    .foregroundStyle(.secondary)
            }
            if store.title != nil {
                Text(store.hasOverride(option) ? G("managed_custom_value","Custom") : G("managed_inherited","Use global setting"))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }.padding(.vertical,4)
    }
    var body: some View {
        Form {
            Section {
                if let gameTitle {Text(gameTitle).font(.headline)}
                Text(G("managed_next_launch","Changes apply when the game is restarted."))
                Text(G("managed_capabilities","Options depend on the selected CPU backend and device capabilities. Unsupported features keep their fallback."))
                    .font(.footnote).foregroundStyle(.secondary)
                if store.title != nil {Text(G("managed_scope_game","Game-specific changes override global settings."))}
            }
            ForEach(["graphics","cpu","audio","platform"],id:\.self) {category in
                Section(G("managed_group_"+category,category.capitalized)) {
                    ForEach(store.options.filter {$0.category == category}) {row($0)}
                    if category == "graphics" {
                        Picker(G("optimization_anisotropy_mode","Texture filtering"),selection:Binding(
                            get:{store.anisotropy},set:{store.anisotropy=$0})) {
                            Text(G("optimization_anisotropy_guest","Game default")).tag(0)
                            Text("1×").tag(1)
                            Text("2×").tag(2)
                        }.accessibilityIdentifier("managed-anisotropy")
                    }
                }
            }
            Section {
                Button(G("reset","Reset"),role:.destructive) {store.reset()}
                    .accessibilityIdentifier("managed-reset")
                Text(G("managed_reset_note","Reset restores defaults for this scope. Games and saves are preserved."))
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }.navigationTitle(G("category_managed_optimizations","Managed optimizations"))
    }
}
