// SPDX-License-Identifier: GPL-2.0-or-later
// Geometry, colours and control behaviour follow MainActivity.DualShockOverlayView.
import UIKit
#if !LSX4_FULL_CONSOLE_UI
import SwiftUI
#endif

private func padColor(_ argb: UInt32) -> UIColor {
    UIColor(red: CGFloat((argb >> 16) & 255) / 255, green: CGFloat((argb >> 8) & 255) / 255,
            blue: CGFloat(argb & 255) / 255, alpha: CGFloat(argb >> 24) / 255)
}
#if !LSX4_FULL_CONSOLE_UI
struct TouchController: UIViewRepresentable {
    func makeUIView(context: Context) -> DualShockOverlayView { DualShockOverlayView() }
    func updateUIView(_ view: DualShockOverlayView, context: Context) {}
}
#endif
@objc(LSX4DualShockOverlayView)
public final class DualShockOverlayView: UIView {
    // Both app hosts draw the same controller. The full runtime supplies its
    // own input sink; the existing app keeps its native bridge by default.
    @objc public var inputHandler: ((UInt32, Data, Bool, UInt64, Bool, UInt16, UInt16) -> Void)?
    @objc public func setInputReady(_ ready: Bool) {
        inputReady=ready
        foreground=UIApplication.shared.applicationState == .active
        if !ready {releaseInputs(connected:window != nil)}
        isUserInteractionEnabled=ready
        external.setEnabled(ready && foreground && window != nil)
    }
    @objc public func releaseAllButtons() {
        foreground=false;external.setEnabled(false);releaseInputs(connected:window != nil)
    }
    private struct Contact {
        var point: CGPoint
        let origin: CGPoint
        let stick: String?
        var eligible = true
        var held = false
        var hold: DispatchWorkItem?
    }
    private let bits: [String: UInt32] = ["share": 1, "l3": 2, "r3": 4, "options": 8,
        "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
        "l2": 0x100, "r2": 0x200, "l1": 0x400, "r1": 0x800,
        "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
        "ps": 0x10000, "touchpad": 0x100000]
    private var boxes: [String: CGRect] = [:]
    private var contacts: [UITouch: Contact] = [:]
    private var activeMask: UInt32 = 0
    private var axes = [UInt8](arrayLiteral: 128, 128, 128, 128, 0, 0)
    private var expanded = false
    private var stroke: CGFloat = 1
    private var fontSize: CGFloat = 8
    private var backgroundToken: NSObjectProtocol?
    private var activeToken: NSObjectProtocol?
    private var inputReady=true
    private var foreground=true
    private var externalState=LSX4PadState.neutral
    private var currentPadPoint:CGPoint?
    private lazy var external=LSX4ExternalGamepad { [weak self] state in
        guard let self else {return};self.externalState=state
        self.send(connected:self.window != nil,touch:self.currentPadPoint,timestamp:UInt64(ProcessInfo.processInfo.systemUptime*1_000_000))
    }
    private let feedback = UIImpactFeedbackGenerator(style: .light)
    public override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false; backgroundColor = .clear; isMultipleTouchEnabled = true
        backgroundToken = NotificationCenter.default.addObserver(forName: UIApplication.willResignActiveNotification,
            object: nil, queue: .main) { [weak self] _ in self?.releaseAllButtons() }
        activeToken=NotificationCenter.default.addObserver(forName:UIApplication.didBecomeActiveNotification,
            object:nil,queue:.main) { [weak self] _ in
                guard let self else {return};self.foreground=true
                self.external.setEnabled(self.inputReady && self.window != nil)
            }
        accessibilityLabel = "Экранный контроллер DualShock"
    }
    public required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    deinit {
        if let backgroundToken {NotificationCenter.default.removeObserver(backgroundToken)}
        if let activeToken {NotificationCenter.default.removeObserver(activeToken)}
    }
    private func visible(_ id: String) -> Bool {
        let key = "overlay_layout_v1_\(id)_visible"
        return UserDefaults.standard.object(forKey: key) as? Bool ?? !["share", "ps", "touchpad"].contains(id)
    }
    private func centered(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
        CGRect(x: x - w / 2, y: y - h / 2, width: w, height: h)
    }
    public override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            external.start();external.setEnabled(inputReady && foreground)
        } else {external.stop()}
        releaseInputs(connected:window != nil)
    }
    public override func layoutSubviews() {
        super.layoutSubviews()
        if !contacts.isEmpty { releaseInputs(connected: window != nil) }
        let w = max(1, bounds.width), h = max(1, bounds.height), m = min(w, h)
        let margin = max(8, w * 0.018), inset = max(28, h * 0.035), bottom = h - inset
        boxes = [:]
        boxes["body"] = CGRect(x: w * 0.33, y: h * 0.94, width: w * 0.34, height: h * 0.06 + 10)
        let psSize = max(28, min(m * 0.042, 44))
        boxes["ps"] = centered(w * 0.5, bottom - max(16, m * 0.02), psSize, psSize)
        let tw = expanded ? min(w * 0.34, m * 0.58) : min(w * 0.18, m * 0.32)
        let th = expanded ? max(76, m * 0.13) : max(28, m * 0.04)
        let gap = expanded ? max(12, m * 0.02) : max(10, m * 0.016)
        let touch = CGRect(x: (w - tw) / 2, y: boxes["ps"]!.minY - gap - th, width: tw, height: th)
        boxes["touchpad"] = touch
        boxes["toggle"] = expanded ? centered(touch.midX, touch.minY + max(16, m * 0.023) * 0.8,
            max(54, tw * 0.18), max(16, m * 0.023)) : touch
        let cw = max(48, min(m * 0.076, 78)), ch = max(22, min(m * 0.038, 34)), cg = max(12, m * 0.02)
        boxes["share"] = CGRect(x: touch.minX - cw - cg, y: touch.midY - ch / 2, width: cw, height: ch)
        boxes["options"] = CGRect(x: touch.maxX + cg, y: touch.midY - ch / 2, width: cw, height: ch)
        let cluster = max(112, min(m * 0.285, 156)), unit = cluster * 0.325
        let dx = margin + cluster * 0.46, dy = bottom - cluster * 0.54, fx = w - margin - cluster * 0.46
        boxes["dpad"] = centered(dx, dy, cluster, cluster); boxes["face"] = centered(fx, dy, cluster, cluster)
        for (id, x, y) in [("up", dx, dy-unit), ("down", dx, dy+unit), ("left", dx-unit, dy), ("right", dx+unit, dy)] {
            boxes[id] = centered(x, y, cluster * 0.35, cluster * 0.35)
        }
        for (id, x, y) in [("triangle", fx, dy-unit), ("cross", fx, dy+unit), ("square", fx-unit, dy), ("circle", fx+unit, dy)] {
            boxes[id] = centered(x, y, cluster * 0.385, cluster * 0.385)
        }
        let sw = min(w * 0.09, m * 0.18), sh = max(24, m * 0.042), sg = max(12, w * 0.009), sy = max(12, h * 0.078)
        boxes["l1"] = CGRect(x: margin, y: sy, width: sw, height: sh)
        boxes["l2"] = CGRect(x: margin + sw + sg, y: sy, width: sw, height: sh)
        boxes["r2"] = CGRect(x: w - margin - sw, y: sy, width: sw, height: sh)
        boxes["r1"] = CGRect(x: w - margin - 2*sw - sg, y: sy, width: sw, height: sh)
        let r = max(24, min(m * 0.06, 42)), stickY = min(dy + cluster * 0.44, bottom - r * 0.14)
        boxes["left_stick"] = centered(max(w * 0.3, boxes["dpad"]!.maxX + r * 1.15), stickY, 2*r, 2*r)
        boxes["right_stick"] = centered(min(w * 0.7, boxes["face"]!.minX - r * 1.15), stickY, 2*r, 2*r)
        let groups = ["dpad": ["dpad", "up", "right", "down", "left"], "face": ["face", "triangle", "circle", "cross", "square"],
            "touchpad": ["touchpad", "toggle"]]
        for id in ["dpad", "face", "left_stick", "right_stick", "l1", "l2", "r1", "r2", "options", "share", "ps", "touchpad"] {
            let members = groups[id] ?? [id], b = members.compactMap { boxes[$0] }.reduce(CGRect.null) { $0.union($1) }
            let prefix = "overlay_layout_v1_\(id)_", defaults = UserDefaults.standard
            let scale = max(0.45, min(2.2, (defaults.object(forKey: prefix + "scale") as? Double) ?? 1))
            let x = max(-1, min(1, defaults.double(forKey: prefix + "x"))) * w
            let y = max(-1, min(1, defaults.double(forKey: prefix + "y"))) * h
            for member in members { if let rect = boxes[member] {
                boxes[member] = CGRect(x: b.midX + (rect.minX-b.midX)*scale+x, y: b.midY + (rect.minY-b.midY)*scale+y,
                                      width: rect.width*scale, height: rect.height*scale)
            } }
        }
        stroke = max(1, m * 0.0035); fontSize = max(8, min(m * 0.013, 12)); setNeedsDisplay()
    }
    private func group(_ id: String) -> String {
        if ["up", "right", "down", "left"].contains(id) { return "dpad" }
        if ["triangle", "circle", "cross", "square"].contains(id) { return "face" }
        return id
    }
    private func pressed(_ id: String) -> Bool { activeMask & (bits[id] ?? 0) != 0 }
    private func shape(_ rect: CGRect, radius: CGFloat, fill: UInt32, outline: UInt32) {
        let path = UIBezierPath(roundedRect: rect, cornerRadius: radius)
        padColor(fill).setFill(); path.fill(); padColor(outline).setStroke(); path.lineWidth = stroke; path.stroke()
    }
    private func text(_ string: String, _ rect: CGRect, _ color: UInt32) {
        let attrs: [NSAttributedString.Key: Any] = [.font: UIFont.boldSystemFont(ofSize: fontSize), .foregroundColor: padColor(color)]
        let size = (string as NSString).size(withAttributes: attrs)
        (string as NSString).draw(at: CGPoint(x: rect.midX-size.width/2, y: rect.midY-size.height/2), withAttributes: attrs)
    }
    private func lines(_ points: [CGPoint], color: UInt32, close: Bool = false) {
        guard let first = points.first else { return }; let path = UIBezierPath(); path.move(to: first)
        for p in points.dropFirst() { path.addLine(to: p) }; if close { path.close() }
        path.lineWidth = stroke * 1.25; path.lineCapStyle = .round; path.lineJoinStyle = .round; padColor(color).setStroke(); path.stroke()
    }
    public override func draw(_ rect: CGRect) {
        guard let body = boxes["body"], let touch = boxes["touchpad"] else { return }
        shape(body, radius: body.height*0.45, fill: 0x3f05070b, outline: 0x565a6572)
        let light = UIBezierPath(roundedRect: CGRect(x: touch.midX-touch.width*0.31, y: touch.minY-8, width: touch.width*0.62, height: 3), cornerRadius: 3)
        padColor(0x904aa3ff).setFill(); light.fill()
        for id in ["l1", "l2", "r1", "r2", "share", "options", "ps", "touchpad"] where visible(id) {
            guard let b = boxes[id] else { continue }; let down = pressed(id)
            let shoulder = ["l1", "l2", "r1", "r2"].contains(id)
            let fill: UInt32 = shoulder ? (down ? 0xc52a6ee8 : 0x76080b11) : (down ? 0xd735485f : 0x7f05070a)
            let outline: UInt32 = shoulder ? (down ? 0xeeffffff : 0x7fa7b0bf) : (down ? 0xffe7eef8 : 0x7fa2aab6)
            shape(b, radius: id == "touchpad" && expanded ? 12 : b.height*0.48, fill: fill, outline: outline)
            text(id == "touchpad" ? (expanded ? "TOUCH PAD" : "TOUCH") : id.uppercased(), b, 0xeeffffff)
        }
        for id in ["dpad", "face"] where visible(id) {
            let b = boxes[id]!, active = activeMask & (id == "dpad" ? 0xf0 : 0xf000) != 0
            shape(b, radius: b.width/2, fill: active ? 0x771b2634 : 0x4d05070a, outline: active ? 0x88d8e4f2 : 0x55414a56)
        }
        for id in ["up", "down", "left", "right", "triangle", "circle", "cross", "square"] where visible(group(id)) {
            let b = boxes[id]!, down = pressed(id), face = group(id) == "face", x = b.midX, y = b.midY, s = b.width * (face ? 0.22 : 0.17)
            shape(b, radius: b.width * (face ? 0.5 : 0.24), fill: face ? (down ? 0xee243044 : 0xdd05070a) : (down ? 0xdd344052 : 0xe3080a0d),
                  outline: face ? (down ? 0xffffffff : 0xaa89919c) : (down ? 0xfff0f5ff : 0xaa6c7480))
            let colors: [String: UInt32] = ["triangle": 0xff3bd671, "circle": 0xffff5a68, "cross": 0xff68a8ff, "square": 0xffff7ccc]
            let color = colors[id] ?? (down ? 0xffffffff : 0xccd8dde5)
            func point(_ dx: CGFloat, _ dy: CGFloat) -> CGPoint { CGPoint(x: x+dx*s, y: y+dy*s) }
            switch id {
            case "up": lines([point(-1, 0.65), point(0, -1), point(1, 0.65)], color: color)
            case "down": lines([point(-1, -0.65), point(0, 1), point(1, -0.65)], color: color)
            case "left": lines([point(0.65, -1), point(-1, 0), point(0.65, 1)], color: color)
            case "right": lines([point(-0.65, -1), point(1, 0), point(-0.65, 1)], color: color)
            case "triangle": lines([point(0, -1), point(-1.05, 0.85), point(1.05, 0.85)], color: color, close: true)
            case "cross": lines([point(-1, -1), point(1, 1)], color: color); lines([point(-1, 1), point(1, -1)], color: color)
            case "square": lines([point(-1, -1), point(1, -1), point(1, 1), point(-1, 1)], color: color, close: true)
            default: let p = UIBezierPath(ovalIn: centered(x, y, s*1.92, s*1.92)); p.lineWidth = stroke*1.25; padColor(color).setStroke(); p.stroke()
            }
        }
        for (id, offset, click) in [("left_stick", 0, "l3"), ("right_stick", 2, "r3")] where visible(id) {
            let b = boxes[id]!, r = b.width/2, down = pressed(click)
            shape(b, radius: r, fill: down ? 0xcc293748 : 0xba05070a, outline: down ? 0xffe8f1ff : 0xaaaab2bf)
            let knob = centered(b.midX + CGFloat(Int(axes[offset])-128)/127*r*0.46,
                b.midY + CGFloat(Int(axes[offset+1])-128)/127*r*0.46, r*0.92, r*0.92)
            shape(knob, radius: r*0.46, fill: 0xee111821, outline: 0xff5f6977); text(click.uppercased(), knob, 0xbbffffff)
        }
    }
    private func insideStick(_ point: CGPoint, _ id: String) -> Bool {
        guard visible(id), let b = boxes[id] else { return false }
        return hypot(point.x-b.midX, point.y-b.midY) <= b.width/2
    }
    private func hitMask(_ point: CGPoint) -> UInt32 {
        var result: UInt32 = 0
        for (id, bit) in bits where visible(group(id)) && boxes[id]?.contains(point) == true {
            if id == "touchpad" && (!expanded || boxes["toggle"]!.contains(point)) { continue }
            result |= bit
        }; return result
    }
    private func toggleHit(_ point: CGPoint) -> Bool {
        visible("touchpad") && boxes[expanded ? "toggle" : "touchpad"]?.insetBy(dx: -14, dy: -14).contains(point) == true
    }
    public override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        hitMask(point) != 0 || insideStick(point, "left_stick") || insideStick(point, "right_stick") || toggleHit(point)
    }
    public override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            let p = touch.location(in: self)
            if toggleHit(p) { expanded.toggle(); releaseInputs(connected: true); setNeedsLayout(); continue }
            var stick: String?
            for id in ["left_stick", "right_stick"] where insideStick(p, id) && !contacts.values.contains(where: { $0.stick == id }) { stick = id; break }
            contacts[touch] = Contact(point: p, origin: p, stick: stick)
            if stick != nil {
                let item = DispatchWorkItem { [weak self, weak touch] in
                    guard let self, let touch, var contact = self.contacts[touch], contact.eligible else { return }
                    contact.held = true; contact.eligible = false; self.contacts[touch] = contact; self.publish()
                }
                contacts[touch]?.hold = item; DispatchQueue.main.asyncAfter(deadline: .now()+0.5, execute: item)
            }
        }; publish()
    }
    public override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches { if var c = contacts[touch] {
            c.point = touch.location(in: self)
            if let stick = c.stick, let b = boxes[stick], hypot(c.point.x-c.origin.x, c.point.y-c.origin.y) > b.width*0.175 {
                c.eligible = false; c.hold?.cancel()
            }
            contacts[touch] = c
        } }; publish()
    }
    public override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches { contacts.removeValue(forKey: touch)?.hold?.cancel() }; publish()
    }
    public override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { touchesEnded(touches, with: event) }
    private func releaseInputs(connected: Bool) {
        for c in contacts.values { c.hold?.cancel() }; contacts.removeAll(); activeMask = 0; axes = [128, 128, 128, 128, 0, 0]
        currentPadPoint=nil;send(connected: connected, touch: nil, timestamp: 0); setNeedsDisplay()
    }
    private func publish() {
        let previous = activeMask; activeMask = 0; axes = [128, 128, 128, 128, 0, 0]; var padPoint: CGPoint?
        for c in contacts.values {
            if let stick = c.stick, let b = boxes[stick] {
                let index = stick == "left_stick" ? 0 : 2, radius = max(1, b.width*0.23)
                axes[index] = UInt8(max(0, min(255, (128+(c.point.x-b.midX)/radius*127).rounded())))
                axes[index+1] = UInt8(max(0, min(255, (128+(c.point.y-b.midY)/radius*127).rounded())))
                if c.held { activeMask |= index == 0 ? 2 : 4 }
            } else {
                activeMask |= hitMask(c.point)
                if activeMask & 0x100000 != 0, let b = boxes["touchpad"], b.contains(c.point) {
                    padPoint = CGPoint(x: max(0,min(1,(c.point.x-b.minX)/b.width)), y: max(0,min(1,(c.point.y-b.minY)/b.height)))
                }
            }
        }
        axes[4] = activeMask & 0x100 != 0 ? 255 : 0; axes[5] = activeMask & 0x200 != 0 ? 255 : 0
        if activeMask & ~previous != 0 { feedback.impactOccurred() }
        currentPadPoint=padPoint
        send(connected: window != nil, touch: padPoint, timestamp: UInt64(ProcessInfo.processInfo.systemUptime*1_000_000)); setNeedsDisplay()
    }
    private func send(connected: Bool, touch: CGPoint?, timestamp: UInt64) {
        let allowed=inputReady && foreground && window != nil
        let onscreen=LSX4PadState(mask:activeMask,axes:axes,touch:touch,connected:connected)
        let output=allowed ? LSX4PadState.merge(touch:onscreen,external:externalState) : .neutral
        if let inputHandler {
            inputHandler(output.mask,Data(output.axes),output.connected,timestamp,output.touch != nil,
                         UInt16((output.touch?.x ?? 0)*1919),UInt16((output.touch?.y ?? 0)*949))
            return
        }
#if !LSX4_FULL_CONSOLE_UI
        LSX4Bridge.updatePad(output.mask,axes:Data(output.axes),connected:output.connected,timestamp:timestamp,
                             touchActive:output.touch != nil,touchX:UInt16((output.touch?.x ?? 0)*1919),touchY:UInt16((output.touch?.y ?? 0)*949))
#endif
    }
}
