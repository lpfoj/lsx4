// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// Android's mark-on-release thresholds. Scrolling and interrupted touches never publish.
struct GoldReportHold:UIViewRepresentable {
    var tap:()->Void
    var mark:(Bool)->Void
    var progress:(Double)->Void
    func makeUIView(context:Context)->Surface {let view=Surface();view.configure(self);return view}
    func updateUIView(_ view:Surface,context:Context) {view.configure(self)}
    static func dismantleUIView(_ view:Surface,coordinator:()) {view.stop()}
    final class Surface:UIView {
        private var action:GoldReportHold?
        private var timer:Timer?,started:TimeInterval=0,lastThreshold=0
        private lazy var press=UILongPressGestureRecognizer(target:self,action:#selector(held(_:)))
        override init(frame:CGRect) {
            super.init(frame:frame);backgroundColor = .clear;isAccessibilityElement=false;accessibilityElementsHidden=true
            press.minimumPressDuration=0.6;press.allowableMovement=12;addGestureRecognizer(press)
            let short=UITapGestureRecognizer(target:self,action:#selector(tapped));short.require(toFail:press);addGestureRecognizer(short)
        }
        required init?(coder:NSCoder) {fatalError("init(coder:) has not been implemented")}
        func configure(_ action:GoldReportHold) {self.action=action}
        @objc private func tapped() {action?.tap()}
        @objc private func held(_ recognizer:UILongPressGestureRecognizer) {
            switch recognizer.state {
            case .began:
                started=ProcessInfo.processInfo.systemUptime-0.6;lastThreshold=0
                timer=Timer.scheduledTimer(withTimeInterval:0.08,repeats:true) {[weak self] _ in self?.tick()};tick()
            case .changed:
                let point=recognizer.location(in:self)
                if !bounds.insetBy(dx:-12,dy:-12).contains(point) {recognizer.isEnabled=false;recognizer.isEnabled=true;stop()}
            case .ended:
                let duration=ProcessInfo.processInfo.systemUptime-started;stop()
                if duration>=4 {action?.mark(duration>=5)}
            case .cancelled,.failed:stop()
            default:break
            }
        }
        private func tick() {
            let elapsed=ProcessInfo.processInfo.systemUptime-started;action?.progress(elapsed)
            let threshold=elapsed>=5 ? 5 : elapsed>=4 ? 4 : 0
            if threshold>lastThreshold {lastThreshold=threshold;UISelectionFeedbackGenerator().selectionChanged()}
        }
        func stop() {timer?.invalidate();timer=nil;action?.progress(0)}
        override func didMoveToWindow() {if window == nil {stop()}}
    }
}

struct GoldReportableRow<Content:View>:View {
    var tap:()->Void,mark:(Bool)->Void
    var identifier:String
    @ViewBuilder var content:()->Content
    @State private var held:Double=0
    var body:some View {
        Button(action:tap) {
            VStack(alignment:.leading,spacing:4) {
                content()
                if held>0 {
                    ProgressView(value:min(held,5),total:5).tint(held>=5 ? .green : .orange)
                    Text(held>=5 ? P("hold_positive","Release: Playable") : held>=4 ? P("hold_negative","Release: Unplayable · hold to 5s for Playable") : P("hold_help","Hold: 4s Unplayable · 5s Playable")).font(.caption2)
                }
            }
        }.buttonStyle(.plain).accessibilityIdentifier(identifier)
            .accessibilityHint(P("hold_accessibility","Hold 4 seconds and release for Unplayable, or 5 seconds for Playable. Also available in the game menu."))
            .overlay {GoldReportHold(tap:tap,mark:mark,progress:{held=$0})}
    }
}
