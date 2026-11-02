// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

struct GoldGameDetailsControl:UIViewRepresentable {
    var accent:UIColor
    var label:String
    var identifier:String
    var action:()->Void
    func makeCoordinator()->Coordinator {Coordinator(action)}
    func makeUIView(context:Context)->UIButton {
        let button=UIButton(type:.system)
        button.setImage(UIImage(systemName:"ellipsis",withConfiguration:UIImage.SymbolConfiguration(pointSize:19,weight:.semibold)),for:.normal)
        button.addTarget(context.coordinator,action:#selector(Coordinator.open),for:.touchUpInside)
        button.accessibilityTraits = .button
        updateUIView(button,context:context)
        return button
    }
    func updateUIView(_ button:UIButton,context:Context) {
        context.coordinator.action=action
        button.tintColor=accent
        button.accessibilityLabel=label
        button.accessibilityIdentifier=identifier
    }
    final class Coordinator:NSObject {
        var action:()->Void
        init(_ action:@escaping ()->Void) {self.action=action}
        @objc func open() {action()}
    }
}
