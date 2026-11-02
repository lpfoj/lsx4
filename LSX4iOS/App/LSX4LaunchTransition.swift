// SPDX-License-Identifier: GPL-2.0-or-later
import UIKit
import QuartzCore
import SwiftUI

@objc(LSX4LaunchAnimationSession)
final class LSX4LaunchAnimationSession: NSObject {
    private let timingKey: String
    private let started = CACurrentMediaTime()
    private var readyAt: CFTimeInterval?
    private var readyFrom: Double = 0
    private var tailDuration: Double = 0.2
    private var jitDuration: Double?
    private let identity = UUID().uuidString.lowercased()
    fileprivate let particleSeed = UInt64.random(in: 1...UInt64.max)
    fileprivate let particleStyle: Int = {
        let previous = UserDefaults.standard.integer(forKey: "lsx4.launchParticleStyle")
        let next = (previous + Int.random(in: 1...2)) % 3
        UserDefaults.standard.set(next, forKey: "lsx4.launchParticleStyle")
        return next
    }()
    fileprivate var elapsed: Double { CACurrentMediaTime() - started }
    private var events: [[String: Any]] = []
    var expectedDuration: Double

    @objc(initWithTimingKey:)
    init(timingKey: String) {
        self.timingKey = "lsx4.launchTiming.v1." + timingKey
        let stored = UserDefaults.standard.double(forKey: self.timingKey)
        expectedDuration = stored > 0 ? min(180, max(0.3, stored)) : 8
        super.init()
        record("preparing", active: false)
    }
    @objc func markJITPrepared() {
        if jitDuration == nil { jitDuration = CACurrentMediaTime() - started; record("jit-ready", active: false) }
    }
    func markGameReady() {
        guard readyAt == nil else { return }
        let now = CACurrentMediaTime()
        readyFrom = progress(at: now)
        tailDuration = max(0.12, min(0.42, (1 - readyFrom) * 0.8))
        readyAt = now
        let measured = min(180, max(0.3, now - started))
        let prior = UserDefaults.standard.double(forKey: timingKey)
        UserDefaults.standard.set(prior > 0 ? prior * 0.35 + measured * 0.65 : measured, forKey: timingKey)
        if let jit = jitDuration { UserDefaults.standard.set(jit, forKey: timingKey + ".jit") }
        record("first-game-frame", active: true)
    }
    func progress(at now: CFTimeInterval) -> Double {
        if let ready = readyAt {
            let blend = min(1, max(0, (now - ready) / tailDuration))
            return readyFrom + (1 - readyFrom) * blend
        }
        let fraction = max(0, now - started) / max(0.3, expectedDuration)
        return fraction <= 1 ? fraction * 0.8 : 0.8 + 0.16 * (1 - exp(-(fraction - 1)))
    }
    func readinessBlend(at now: CFTimeInterval) -> Double {
        guard let ready = readyAt else { return 0 }
        return min(1, max(0, (now - ready) / tailDuration))
    }
    func record(_ stage: String, active: Bool) {
        guard ProcessInfo.processInfo.environment["LSX4_PS5_LOCAL_SESSION_TEST"] == "1" else { return }
        let elapsed = CACurrentMediaTime() - started
        events.append(["stage": stage, "elapsed": elapsed, "animationActive": active])
        if events.count > 20 { events.removeFirst() }
        var value: [String: Any] = ["session": identity, "timingKey": timingKey, "expectedSeconds": expectedDuration,
                                    "elapsed": elapsed, "stage": stage, "animationActive": active,
                                    "gameReady": readyAt != nil, "automaticUploads": false, "events": events,
                                    "particleSeed": String(particleSeed), "particleStyle": particleStyle]
        if let jit = jitDuration { value["jitSeconds"] = jit }
        if let ready = readyAt { value["firstFrameSeconds"] = ready - started }
        if let data = try? JSONSerialization.data(withJSONObject: value),
           let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
            try? data.write(to: documents.appendingPathComponent("launch-animation-" + identity + ".json"), options: .atomic)
        }
    }
}

private struct LaunchParticleField {
    struct Particle {
        let x: Double, y: Double, speed: Double, sway: Double, phase: Double, size: Double
    }
    let particles: [Particle]
    let maze: [CGPoint]
    let style: Int

    init(seed: UInt64, style: Int) {
        self.style = style
        var state = seed
        func random() -> Double {
            state &+= 0x9e3779b97f4a7c15
            var z = state
            z = (z ^ (z >> 30)) &* 0xbf58476d1ce4e5b9
            z = (z ^ (z >> 27)) &* 0x94d049bb133111eb
            return Double((z ^ (z >> 31)) >> 11) / 9007199254740992
        }
        // The maze is built once; drawing only interpolates its icon positions.
        let columns = 15, rows = 9
        var visited = Set<Int>([0]), stack = [0]
        var route = [CGPoint(x: 0, y: 0)]
        while let cell = stack.last {
            let x = cell % columns, y = cell / columns
            let neighbors = [(x-1,y),(x+1,y),(x,y-1),(x,y+1)].filter {
                $0.0 >= 0 && $0.0 < columns && $0.1 >= 0 && $0.1 < rows && !visited.contains($0.1 * columns + $0.0)
            }
            if neighbors.isEmpty {
                stack.removeLast()
                if let parent = stack.last { route.append(CGPoint(x: parent % columns, y: parent / columns)) }
            } else {
                let next = neighbors[Int(random() * Double(neighbors.count))]
                let index = next.1 * columns + next.0
                visited.insert(index); stack.append(index)
                route.append(CGPoint(x: next.0, y: next.1))
            }
        }
        maze = route.map { CGPoint(x: 0.1 + $0.x / Double(columns-1) * 0.8,
                                  y: 0.12 + $0.y / Double(rows-1) * 0.76) }
        particles = (0..<112).map { _ in
            Particle(x: random(), y: random(), speed: 0.045 + random() * 0.10,
                     sway: 0.025 + random() * 0.08, phase: random() * .pi * 2,
                     size: 3.0 + random() * 5.5)
        }
    }
    private func wrapped(_ value: Double) -> Double { value - floor(value) }
    func pose(_ index: Int, at time: Double, reduced: Bool) -> (CGPoint, Double, Double) {
        let p = particles[index], t = reduced ? 0 : time
        let rain = CGPoint(x: wrapped(p.x + t * 0.016 + sin(t * 0.5 + p.phase) * p.sway * 0.3),
                           y: wrapped(p.y + t * p.speed))
        switch style {
        case 0:
            return (rain, 0.14 * sin(t + p.phase), 0.28 + 0.18 * p.y)
        case 1:
            let leaf = CGPoint(x: wrapped(p.x + sin(t * 0.65 + p.phase) * p.sway + t * 0.012),
                               y: wrapped(p.y + t * p.speed * 0.55 + sin(t * 0.4 + p.phase) * 0.018))
            return (leaf, t * (0.3 + p.speed * 4) + p.phase, 0.25 + 0.22 * p.y)
        default:
            // A stream traces the generated corridors, gathering from and returning to falling icons.
            let along = wrapped(Double(index) / Double(particles.count) + t * 0.018) * Double(maze.count-1)
            let a = Int(along), b = min(a+1, maze.count-1), f = along - Double(a)
            let target = CGPoint(x: maze[a].x + (maze[b].x-maze[a].x) * f,
                                 y: maze[a].y + (maze[b].y-maze[a].y) * f)
            let phase = (sin(t * 0.45 - 1.2) + 1) / 2
            let blend = reduced ? 1 : phase * phase * (3 - 2 * phase)
            return (CGPoint(x: rain.x + (target.x-rain.x) * blend,
                            y: rain.y + (target.y-rain.y) * blend),
                    (1-blend) * (t * 0.3 + p.phase), 0.28 + blend * 0.24)
        }
    }
}

private final class LaunchWaveView: UIView {
    var elapsed: Double = 0
    var progress: Double = 0
    var readinessBlend: Double = 0
    var reduceMotion = false
    private let theme: GoldTheme
    private let apple: UIImage?
    private let palette: [UIColor]
    private let accent: UIColor
    private let field: LaunchParticleField

    init(session: LSX4LaunchAnimationSession) {
        field = LaunchParticleField(seed: session.particleSeed, style: session.particleStyle)
        let index = min(3, max(0, UserDefaults.standard.integer(forKey: "gold.theme")))
        let selectedTheme = GoldTheme(rawValue: index) ?? .azure
        theme = selectedTheme
        apple = UIImage(systemName:"apple.logo")?.withTintColor(UIColor(selectedTheme.accent),renderingMode:.alwaysOriginal)
        palette = selectedTheme.colors.map { UIColor($0) }
        accent = UIColor(selectedTheme.accent)
        super.init(frame: .zero)
        backgroundColor = palette[0]
        isOpaque = true
        isUserInteractionEnabled = false
        contentMode = .redraw
        isAccessibilityElement = false
        accessibilityContainerType = .semanticGroup
        accessibilityIdentifier = "game-launch-animation"
        let labels = ["en":"Launching game", "es":"Iniciando juego", "fr":"Lancement du jeu",
                      "de":"Spiel wird gestartet", "pt-BR":"Iniciando jogo",
                      "ru":"Запуск игры", "zh-Hans":"正在启动游戏"]
        accessibilityLabel = labels[ConsoleLocalization.selected] ?? labels["en"]
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func draw(_ rect: CGRect) {
        guard let context = UIGraphicsGetCurrentContext(), bounds.width > 0, bounds.height > 0 else { return }
        let width = bounds.width, height = bounds.height
        let p = min(1, max(0, progress))
        let approach = 1 + min(0.96, p) * 1.6
        let reveal = 1 - pow(1 - readinessBlend, 5)
        let scale = reduceMotion ? 1 : approach + (5 - approach) * reveal
        context.saveGState()
        context.translateBy(x: width / 2, y: height / 2)
        context.scaleBy(x: scale, y: scale)
        context.translateBy(x: -width / 2, y: -height / 2)
        if let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                                     colors: palette.map(\.cgColor) as CFArray,
                                     locations: [0, 0.5, 1]) {
            context.drawLinearGradient(gradient, start: .zero, end: CGPoint(x: width, y: height), options: [])
        }
        let unit = max(0.25, min(width, height) / 1080)
        let particleUnit = min(1.2, max(0.7, min(width, height) / 900))
        let waves: [(base: Double, amplitude: Double, frequency: Double, speed: Double, width: Double, alpha: Double)] = [
            (0.48, 0.08, 1, 0.35, 80, 0.25),
            (0.52, 0.10, 0.8, 0.30, 100, 0.20),
            (0.56, 0.07, 1.2, 0.40, 60, 0.22)
        ]
        let time = reduceMotion ? 0 : elapsed
        for wave in waves {
            let path = CGMutablePath()
            for segment in 0...50 {
                let x = -100 + Double(segment) * (width + 200) / 50
                let phase = time * wave.speed
                let y = wave.base * height
                    + sin(x / width * wave.frequency * .pi * 2 + phase) * wave.amplitude * height
                    + sin(x / width * wave.frequency * .pi + phase * 1.3) * wave.amplitude * height * 0.3
                let point = CGPoint(x: x, y: y)
                if segment == 0 { path.move(to: point) } else { path.addLine(to: point) }
            }
            context.setLineCap(.round)
            for (strokeWidth, opacity) in [(2.5, 0.15), (1.5, 0.25), (0.8, 0.4), (0.3, 0.3)] {
                context.addPath(path)
                context.setLineWidth(wave.width * strokeWidth * unit)
                context.setStrokeColor(accent.withAlphaComponent(wave.alpha * opacity).cgColor)
                context.strokePath()
            }
        }
        for index in field.particles.indices {
            let (position, rotation, opacity) = field.pose(index, at: time, reduced: reduceMotion)
            let radius = field.particles[index].size * particleUnit
            context.saveGState()
            context.translateBy(x: width * position.x, y: height * position.y)
            context.rotate(by: rotation)
            context.setAlpha(opacity)
            if theme.usesAppleParticle(index) {
                apple?.draw(in:CGRect(x:-radius*1.2,y:-radius*1.5,width:radius*2.4,height:radius*3))
            } else {
                context.setFillColor(accent.cgColor)
                context.addPath(theme.particlePath(radius:radius));context.fillPath()
            }
            context.restoreGState()
        }
        context.restoreGState()
        let fade = reduceMotion ? readinessBlend : 1 - pow(1 - readinessBlend, 5)
        context.setFillColor(UIColor.black.withAlphaComponent(fade).cgColor)
        context.fill(bounds)
    }
}

@objc(LSX4LaunchTransitionController)
final class LSX4LaunchTransitionController: UIViewController {
    @objc let animationSession: LSX4LaunchAnimationSession
    private var displayLink: CADisplayLink?
    private var gameReady = false
    private var finished = false
    private var completion: (() -> Void)?
    private var observers: [NSObjectProtocol] = []
    private var visible = false
    private let statusLabel = UILabel()
    private let progressIndicator = UIActivityIndicatorView(style: .medium)
    private var wave: LaunchWaveView { view as! LaunchWaveView }
    var reduceMotionForPreview: Bool?
    @objc var animationActive: Bool { displayLink != nil }
    private var reduced: Bool { reduceMotionForPreview ?? UIAccessibility.isReduceMotionEnabled }

    @objc(initWithSession:)
    init(session: LSX4LaunchAnimationSession) {
        animationSession = session
        super.init(nibName: nil, bundle: nil)
    }
    @objc override convenience init(nibName nibNameOrNil: String?, bundle nibBundleOrNil: Bundle?) {
        self.init(session: LSX4LaunchAnimationSession(timingKey: "default"))
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func loadView() { view = LaunchWaveView(session: animationSession) }
    override func viewDidLoad() {
        super.viewDidLoad()
        wave.reduceMotion = reduced
        let captions = ["en":"Preparing on-device JIT", "es":"Preparando JIT en el dispositivo",
                        "fr":"Préparation du JIT sur l’appareil", "de":"JIT wird auf dem Gerät vorbereitet",
                        "pt-BR":"Preparando JIT no dispositivo", "ru":"Подготовка JIT на устройстве",
                        "zh-Hans":"正在设备上准备 JIT"]
        statusLabel.text = captions[ConsoleLocalization.selected] ?? captions["en"]
        statusLabel.accessibilityIdentifier = "launch-jit-status"
        statusLabel.font = .preferredFont(forTextStyle: .caption1)
        statusLabel.adjustsFontForContentSizeCategory = true
        statusLabel.textColor = UIColor.white.withAlphaComponent(0.84)
        statusLabel.textAlignment = .right; statusLabel.numberOfLines = 2
        statusLabel.setContentCompressionResistancePriority(.required, for: .vertical)
        progressIndicator.color = .white; progressIndicator.hidesWhenStopped = false
        progressIndicator.isAccessibilityElement = false
        progressIndicator.accessibilityIdentifier = "launch-jit-progress"
        progressIndicator.setContentHuggingPriority(.required, for: .horizontal)
        let status = UIStackView(arrangedSubviews: [statusLabel, progressIndicator])
        status.axis = .horizontal; status.alignment = .center; status.spacing = 8
        status.translatesAutoresizingMaskIntoConstraints = false
        status.accessibilityIdentifier = "launch-jit-status-container"
        view.addSubview(status)
        let safe = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            status.trailingAnchor.constraint(equalTo: safe.trailingAnchor, constant: -16),
            status.bottomAnchor.constraint(equalTo: safe.bottomAnchor, constant: -12),
            status.leadingAnchor.constraint(greaterThanOrEqualTo: safe.leadingAnchor, constant: 16),
            status.widthAnchor.constraint(lessThanOrEqualToConstant: 300),
            status.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, multiplier: 0.74)
        ])
        let center = NotificationCenter.default
        for name in [UIApplication.didBecomeActiveNotification, UIApplication.willResignActiveNotification,
                     UIAccessibility.reduceMotionStatusDidChangeNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.refreshAnimation()
            })
        }
    }
    override var prefersStatusBarHidden: Bool { true }
    override func viewDidAppear(_ animated: Bool) { super.viewDidAppear(animated); visible = true; progressIndicator.startAnimating(); refreshAnimation() }
    override func viewWillDisappear(_ animated: Bool) { visible = false; progressIndicator.stopAnimating(); stop(); super.viewWillDisappear(animated) }
    deinit { displayLink?.invalidate(); observers.forEach(NotificationCenter.default.removeObserver) }

    private func stop() { displayLink?.invalidate(); displayLink = nil }
    private func refreshAnimation() {
        guard isViewLoaded else { return }
        wave.reduceMotion = reduced
        guard visible, !finished, UIApplication.shared.applicationState == .active else { stop(); return }
        if reduced && !gameReady { stop(); wave.setNeedsDisplay(); return }
        guard displayLink == nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 24, maximum: gameReady ? 60 : 30,
                                                       preferred: gameReady ? 60 : 30)
        displayLink = link
        link.add(to: .main, forMode: .common)
    }
    @objc private func tick(_ link: CADisplayLink) {
        wave.elapsed = animationSession.elapsed
        wave.progress = animationSession.progress(at: link.timestamp)
        wave.readinessBlend = animationSession.readinessBlend(at: link.timestamp)
        if gameReady && wave.progress >= 1 { complete(); return }
        wave.accessibilityValue = gameReady ? "revealing" : "waiting"
        wave.setNeedsDisplay()
    }
    @objc(finishWithCompletion:)
    func finish(completion: @escaping () -> Void) {
        if !Thread.isMainThread { DispatchQueue.main.async { [weak self] in self?.finish(completion: completion) }; return }
        if finished { completion(); return }
        guard !gameReady else { return }
        self.completion = completion
        animationSession.markGameReady(); gameReady = true
        stop(); refreshAnimation()
    }
    private func complete() {
        guard !finished else { return }
        finished = true; stop(); progressIndicator.stopAnimating(); wave.progress = 1; wave.readinessBlend = 1; wave.setNeedsDisplay()
        UIView.animate(withDuration: reduced ? 0 : 0.16, animations: { self.view.alpha = 0 }) { [weak self] _ in
            guard let self = self else { return }
            self.animationSession.record("finished", active: false)
            let callback = self.completion; self.completion = nil; callback?()
        }
    }
    @objc func cancel() {
        if !finished { animationSession.record("cancelled", active: false) }
        finished = true; completion = nil; stop(); progressIndicator.stopAnimating()
        if isViewLoaded { view.layer.removeAllAnimations() }
        willMove(toParent: nil); view.removeFromSuperview(); removeFromParent()
    }
}
