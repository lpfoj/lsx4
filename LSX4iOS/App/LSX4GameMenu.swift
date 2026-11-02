import SwiftUI
import UIKit

@objc(LSX4GameMenuDelegate) public protocol LSX4GameMenuDelegate: AnyObject {
    func gameMenuSetPaused(_ paused: Bool)
    func gameMenuExit(_ completion: @escaping (String?) -> Void)
}

@MainActor @objc(LSX4GameMenuCoordinator)
public final class LSX4GameMenuCoordinator: NSObject, ObservableObject {
    @Published var overlayEnabled: Bool
    @Published var opacity: Double
    @Published var exiting = false
    @Published var error: String?
    private weak var parent: UIViewController?
    private weak var controls: DualShockOverlayView?
    private weak var delegate: LSX4GameMenuDelegate?
    private var handle: UIButton!
    private var cover: UIView?
    private var hosting: UIViewController?
    private var edge: UIScreenEdgePanGestureRecognizer!
    private var drawerBleedEdges: Edge.Set {
        if #available(iOS 27.1, *) { return [] }
        return UIDevice.current.userInterfaceIdiom == .phone ? .trailing : []
    }
    @objc(initWithParent:controls:delegate:)
    public init(parent: UIViewController, controls: DualShockOverlayView, delegate: LSX4GameMenuDelegate) {
        self.parent = parent
        self.controls = controls
        self.delegate = delegate
        overlayEnabled = UserDefaults.standard.object(forKey: "lsx4.overlay.enabled") as? Bool ?? true
        opacity = max(20, min(100, UserDefaults.standard.object(forKey: "lsx4.overlay.opacity") as? Double ?? 74))
        super.init()
        handle = UIButton(type: .system)
        handle.setImage(UIImage(systemName: "chevron.left"), for: .normal)
        handle.tintColor = .white
        handle.backgroundColor = UIColor(red: 0.04, green: 0.09, blue: 0.17, alpha: 0.62)
        handle.layer.cornerRadius = 15
        handle.layer.cornerCurve = .continuous
        handle.layer.borderWidth = 0.5
        handle.layer.borderColor = UIColor.white.withAlphaComponent(0.22).cgColor
        handle.accessibilityLabel = P("game_menu_open", "Open quick controls")
        handle.accessibilityIdentifier = "game-menu-open"
        handle.addTarget(self, action: #selector(openMenu), for: .touchUpInside)
        handle.translatesAutoresizingMaskIntoConstraints = false
        parent.view.addSubview(handle)
        NSLayoutConstraint.activate([
            handle.trailingAnchor.constraint(equalTo: parent.view.trailingAnchor),
            handle.centerYAnchor.constraint(equalTo: parent.view.centerYAnchor),
            handle.widthAnchor.constraint(equalToConstant: 30), handle.heightAnchor.constraint(equalToConstant: 66)
        ])
        edge = UIScreenEdgePanGestureRecognizer(target: self, action: #selector(edgePan(_:)))
        edge.edges = .right
        parent.view.addGestureRecognizer(edge)
        applyOverlay()
    }
    @objc private func edgePan(_ gesture: UIScreenEdgePanGestureRecognizer) {
        if gesture.state == .began { openMenu() }
    }
    @objc public func openMenu() {
        guard cover == nil, let parent else { return }
        delegate?.gameMenuSetPaused(true)
        controls?.setInputReady(false)
        let shade = UIView(frame: parent.view.bounds)
        shade.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        let scrim = UIButton(frame: shade.bounds)
        scrim.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        scrim.backgroundColor = UIColor.black.withAlphaComponent(0.42)
        scrim.accessibilityLabel = P("game_menu_close", "Close quick controls")
        scrim.accessibilityIdentifier = "game-menu-close"
        scrim.addTarget(self, action: #selector(closeMenu), for: .touchUpInside)
        shade.addSubview(scrim)
        let host = UIHostingController(rootView: LSX4GameDrawer(state: self)
            .ignoresSafeArea(.container, edges: drawerBleedEdges)
            .preferredColorScheme(.dark)
            .environment(\.locale, Locale(identifier: LSX4AppText.language))
            .environment(\.colorScheme, .dark)
            .environment(\.goldPadPresentation, parent.traitCollection.userInterfaceIdiom == .pad))
        host.overrideUserInterfaceStyle = .dark
        parent.addChild(host)
        host.view.translatesAutoresizingMaskIntoConstraints = false
        host.view.backgroundColor = .clear
        shade.addSubview(host.view)
        parent.view.addSubview(shade)
        NSLayoutConstraint.activate([
            host.view.trailingAnchor.constraint(equalTo: shade.trailingAnchor),
            host.view.topAnchor.constraint(equalTo: shade.topAnchor),
            host.view.bottomAnchor.constraint(equalTo: shade.bottomAnchor),
            host.view.widthAnchor.constraint(equalTo: shade.widthAnchor, multiplier: 0.88).withPriority(.defaultLow),
            host.view.widthAnchor.constraint(lessThanOrEqualToConstant: 400)
        ])
        host.didMove(toParent: parent)
        let swipe = UISwipeGestureRecognizer(target: self, action: #selector(closeMenu))
        swipe.direction = .right
        host.view.addGestureRecognizer(swipe)
        cover = shade
        hosting = host
        if !UIAccessibility.isReduceMotionEnabled {
            shade.alpha = 0
            host.view.transform = CGAffineTransform(translationX: 18, y: 0)
            UIView.animate(withDuration: 0.18, delay: 0, options: [.beginFromCurrentState, .curveEaseOut]) {
                shade.alpha = 1
                host.view.transform = .identity
            }
        }
    }
    @objc public func closeMenu() {
        guard !exiting else { return }
        removeCover()
        controls?.setInputReady(true)
        applyOverlay()
        delegate?.gameMenuSetPaused(false)
    }
    private func removeCover() {
        hosting?.willMove(toParent: nil)
        hosting?.view.removeFromSuperview()
        hosting?.removeFromParent()
        hosting = nil
        cover?.removeFromSuperview()
        cover = nil
    }
    func applyOverlay() {
        controls?.isHidden = !overlayEnabled
        controls?.alpha = opacity / 100
        UserDefaults.standard.set(overlayEnabled, forKey: "lsx4.overlay.enabled")
        UserDefaults.standard.set(opacity, forKey: "lsx4.overlay.opacity")
        controls?.setNeedsLayout()
        controls?.setNeedsDisplay()
    }
    func exit() {
        guard !exiting else { return }
        exiting = true
        error = nil
        delegate?.gameMenuExit { [weak self] issue in
            Task { @MainActor in
                guard let self else { return }
                if let issue { self.error = issue; self.exiting = false }
                else { self.removeCover() }
            }
        }
    }
    @objc public func invalidate() {
        exiting = true
        removeCover()
        handle.removeFromSuperview()
        if let edge { parent?.view.removeGestureRecognizer(edge) }
        delegate = nil
    }
}

private extension NSLayoutConstraint {
    func withPriority(_ value: UILayoutPriority) -> NSLayoutConstraint { priority = value; return self }
}

private struct LSX4GameDrawer: View {
    @ObservedObject var state: LSX4GameMenuCoordinator
    @AppStorage("gold.theme") private var storedTheme = 0
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.verticalSizeClass) private var verticalSize
    @State private var customize = false
    private var theme: GoldTheme { GoldTheme(rawValue: storedTheme) ?? .azure }
    private var compact: Bool { verticalSize == .compact }

    private func symbol(_ name: String) -> some View {
        Image(systemName: name).font(.system(size: 18, weight: .medium))
            .foregroundStyle(theme.accent).frame(width: 28).accessibilityHidden(true)
    }
    private var closeButton: some View {
        Button { state.closeMenu() } label: {
            Image(systemName: "xmark").font(.system(size: 14, weight: .semibold))
                .frame(width: 44, height: 44).goldGlass(radius: 16)
        }.buttonStyle(.plain)
            .accessibilityLabel(P("game_menu_close", "Close quick controls"))
            .accessibilityIdentifier("game-menu-close-button")
    }
    private var heading: some View {
        VStack(alignment: .leading, spacing: compact ? 6 : 12) {
            if compact {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("LSX4").font(.system(size: 10, weight: .semibold, design: .rounded)).tracking(2).foregroundStyle(theme.accent)
                        Text(P("game_menu_title", "Quick controls")).font(.system(.title3, design: .rounded, weight: .bold))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer(minLength: 0)
                    closeButton
                }
            } else {
                HStack {
                    HStack(spacing: 7) {
                        Image(systemName: "pause.fill").font(.system(size: 9, weight: .bold))
                        Text("LSX4").font(.system(size: 12, weight: .semibold, design: .rounded)).tracking(2)
                    }.foregroundStyle(theme.accent).padding(.horizontal, 11).padding(.vertical, 7)
                        .background(theme.accent.opacity(0.10), in: Capsule())
                    Spacer(minLength: 12)
                    closeButton
                }
                Text(P("game_menu_title", "Quick controls")).font(.system(.title2, design: .rounded, weight: .bold))
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text(P("game_menu_hint", "Swipe right or tap outside to close"))
                .font(compact ? .caption : .footnote).foregroundStyle(.white.opacity(0.65))
                .fixedSize(horizontal: false, vertical: true)
        }
    }
    private var controls: some View {
        VStack(spacing: 0) {
            Toggle(isOn: $state.overlayEnabled) {
                HStack(spacing: 10) {
                    symbol("gamecontroller")
                    Text(P("game_menu_overlay", "On-screen controls")).fixedSize(horizontal: false, vertical: true)
                }
            }.tint(theme.accent).padding(compact ? 12 : 16).accessibilityIdentifier("game-menu-overlay")
            Divider().overlay(.white.opacity(0.08)).padding(.horizontal, 16)
            Button { customize = true } label: {
                HStack(spacing: 10) {
                    symbol("slider.horizontal.3")
                    Text(P("controls_customize_overlay", "Customize on-screen controls"))
                        .fixedSize(horizontal: false, vertical: true).frame(maxWidth: .infinity, alignment: .leading)
                    Image(systemName: "chevron.right").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                }.padding(compact ? 12 : 16).frame(minHeight: 48).contentShape(Rectangle())
            }.buttonStyle(.plain).accessibilityIdentifier("game-menu-customize")
        }.goldGlass(radius: 20)
    }
    private var opacity: some View {
        VStack(alignment: .leading, spacing: compact ? 8 : 14) {
            HStack(spacing: 10) {
                symbol("circle.lefthalf.filled")
                Text(P("game_menu_opacity", "Overlay opacity"))
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 4)
                Text("\(Int(state.opacity))%")
                    .font(.callout.monospacedDigit().weight(.semibold)).foregroundStyle(theme.accent)
                    .padding(.horizontal, 9).padding(.vertical, 5)
                    .background(theme.accent.opacity(0.10), in: Capsule())
            }
            Slider(value: $state.opacity, in: 20...100, step: 1).tint(theme.accent)
                .accessibilityLabel(P("game_menu_opacity", "Overlay opacity"))
                .accessibilityValue("\(Int(state.opacity))%")
                .accessibilityIdentifier("game-menu-opacity")
        }.padding(compact ? 12 : 16).goldGlass(radius: 20)
    }
    var body: some View {
        VStack(alignment: .leading, spacing: compact ? 12 : 18) {
            heading
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    controls
                    opacity
                    HStack(alignment: .top, spacing: 10) {
                        symbol("dot.radiowaves.left.and.right")
                        Text(P("controls_gamepad_hint", "You can also use an external gamepad."))
                            .font(.footnote).foregroundStyle(.white.opacity(0.65))
                            .fixedSize(horizontal: false, vertical: true)
                    }.padding(.horizontal, 4).padding(.vertical, 4)
                    if let error = state.error {
                        Label(P(error, "Unable to stop the game safely. Please try again."), systemImage: "exclamationmark.triangle")
                            .font(.footnote).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
                    }
                }.padding(.vertical, 2)
            }.scrollIndicators(.visible)
            Button(role: .destructive) { state.exit() } label: {
                HStack(spacing: 10) {
                    if state.exiting { ProgressView().tint(.white) }
                    else { Image(systemName: "rectangle.portrait.and.arrow.right").accessibilityHidden(true) }
                    Text(P("game_menu_exit", "Exit to game library")).fontWeight(.semibold)
                        .fixedSize(horizontal: false, vertical: true)
                }.frame(maxWidth: .infinity, minHeight: 24).padding(15)
                    .background(Color.red.opacity(0.12), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(.red.opacity(0.24), lineWidth: 0.5))
            }.buttonStyle(.plain).foregroundStyle(.red.opacity(0.95)).accessibilityIdentifier("game-menu-exit")
        }.font(.subheadline).foregroundStyle(.white).padding(compact ? 16 : 20)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background {
                ZStack {
                    if reduceTransparency { Color(white: 0.06) }
                    else { Rectangle().fill(.ultraThinMaterial) }
                    LinearGradient(colors: theme.colors.map { $0.opacity(reduceTransparency ? 1 : 0.82) }, startPoint: .topLeading, endPoint: .bottomTrailing)
                    LinearGradient(colors: [theme.accent.opacity(0.13), .clear], startPoint: .topLeading, endPoint: .bottomTrailing)
                }.ignoresSafeArea()
            }
            .overlay(alignment: .leading) {
                Rectangle().fill(LinearGradient(colors: [.white.opacity(0.26), theme.accent.opacity(0.25), .clear], startPoint: .top, endPoint: .bottom)).frame(width: 0.5).ignoresSafeArea()
            }
            .disabled(state.exiting)
            .onChange(of: state.overlayEnabled) { _, _ in state.applyOverlay() }
            .onChange(of: state.opacity) { _, _ in state.applyOverlay() }
            .goldModal(isPresented: $customize, onDismiss: { state.applyOverlay() }) {
                NavigationStack {
                    GoldControlsView().toolbar {
                        ToolbarItem(placement: .confirmationAction) { Button(L("Готово")) { customize = false } }
                    }
                }.preferredColorScheme(.dark)
            }
    }
}
