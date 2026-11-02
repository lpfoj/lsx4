// SPDX-License-Identifier: GPL-2.0-or-later
import UIKit
import SwiftUI

private struct GoldPadPresentationKey: EnvironmentKey { static let defaultValue = false }
extension EnvironmentValues {
    var goldPadPresentation: Bool {
        get { self[GoldPadPresentationKey.self] }
        set { self[GoldPadPresentationKey.self] = newValue }
    }
}

private struct GoldBooleanPresentation<Destination: View>: ViewModifier {
    @Binding var isPresented: Bool
    var onDismiss: (() -> Void)?
    let destination: () -> Destination
    @Environment(\.goldPadPresentation) private var isPad
    @ViewBuilder func body(content: Content) -> some View {
        if isPad {
            content.fullScreenCover(isPresented: $isPresented, onDismiss: onDismiss) {
                destination().environment(\.goldPadPresentation, true)
            }
        } else {
            content.sheet(isPresented: $isPresented, onDismiss: onDismiss, content: destination)
        }
    }
}
private struct GoldItemPresentation<Item: Identifiable, Destination: View>: ViewModifier {
    @Binding var item: Item?
    var onDismiss: (() -> Void)?
    let destination: (Item) -> Destination
    @Environment(\.goldPadPresentation) private var isPad
    @ViewBuilder func body(content: Content) -> some View {
        if isPad {
            content.fullScreenCover(item: $item, onDismiss: onDismiss) { value in
                destination(value).environment(\.goldPadPresentation, true)
            }
        } else {
            content.sheet(item: $item, onDismiss: onDismiss, content: destination)
        }
    }
}
extension View {
    func goldModal<Destination: View>(isPresented: Binding<Bool>, onDismiss: (() -> Void)? = nil,
                                     @ViewBuilder content: @escaping () -> Destination) -> some View {
        modifier(GoldBooleanPresentation(isPresented: isPresented, onDismiss: onDismiss, destination: content))
    }
    func goldModal<Item: Identifiable, Destination: View>(item: Binding<Item?>, onDismiss: (() -> Void)? = nil,
                                                         @ViewBuilder content: @escaping (Item) -> Destination) -> some View {
        modifier(GoldItemPresentation(item: item, onDismiss: onDismiss, destination: content))
    }
}

enum LSX4WindowChrome {
    static func topPadding(safeTop:CGFloat,baseline:CGFloat)->CGFloat {
        guard UIDevice.current.userInterfaceIdiom == .pad else {return baseline}
        if #available(iOS 26.0,*) {return max(baseline,40-safeTop)}
        return baseline
    }
}

final class LSX4LibraryHostingController:UIHostingController<GoldLibraryView> {
    private func updatePresentationPlatform() {
        let isPad = traitCollection.userInterfaceIdiom == .pad
        if rootView.padPresentation != isPad { rootView.padPresentation = isPad }
    }
    override func viewDidLoad() {
        super.viewDidLoad()
        updatePresentationPlatform()
        registerForTraitChanges([UITraitUserInterfaceIdiom.self]) { (host: LSX4LibraryHostingController, _: UITraitCollection) in
            host.updatePresentationPlatform()
        }
    }
    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        updatePresentationPlatform()
    }
    override var prefersStatusBarHidden:Bool {traitCollection.userInterfaceIdiom == .phone}
    override func viewDidAppear(_ animated:Bool) {
        super.viewDidAppear(animated)
        if traitCollection.userInterfaceIdiom == .phone {setNeedsStatusBarAppearanceUpdate()}
    }
    override var supportedInterfaceOrientations:UIInterfaceOrientationMask {
        traitCollection.userInterfaceIdiom == .pad ? .all : .allButUpsideDown
    }
}

@objc(LSX4WindowSceneDelegate) public final class LSX4WindowSceneDelegate:NSObject,UIWindowSceneDelegate {
    public var window:UIWindow?
    private func sceneBounds(_ scene:UIWindowScene)->CGRect {
        if #available(iOS 26.0,*) {return scene.effectiveGeometry.coordinateSpace.bounds}
        return scene.coordinateSpace.bounds
    }
    public func scene(_ scene:UIScene,willConnectTo session:UISceneSession,options connectionOptions:UIScene.ConnectionOptions) {
        guard let scene=scene as? UIWindowScene else {return}
        let delegate=UIApplication.shared.delegate as? NSObject
        let existing=delegate?.responds(to:NSSelectorFromString("window")) == true
            ? delegate?.value(forKey:"window") as? UIWindow : nil
        let window:UIWindow
        if scene.traitCollection.userInterfaceIdiom == .phone {
            window=UIWindow(windowScene:scene)
            let root=existing?.rootViewController
            existing?.rootViewController=nil
            existing?.isHidden=true
            window.rootViewController=root
        }else {window=existing ?? UIWindow(windowScene:scene)}
        window.windowScene=scene
        window.frame=sceneBounds(scene)
        if window.rootViewController == nil {window.rootViewController=LSX4LibraryControllerFactory.makeController()}
        self.window=window
        if delegate?.responds(to:NSSelectorFromString("setWindow:")) == true {delegate?.setValue(window,forKey:"window")}
        if scene.traitCollection.userInterfaceIdiom == .pad {
            scene.sizeRestrictions?.minimumSize=CGSize(width:320,height:320)
        }
        window.makeKeyAndVisible()
        recordGeometry()
    }
    public func windowScene(_ windowScene:UIWindowScene,didUpdate previousCoordinateSpace:UICoordinateSpace,
                           interfaceOrientation previousInterfaceOrientation:UIInterfaceOrientation,
                           traitCollection previousTraitCollection:UITraitCollection) {
        window?.frame=sceneBounds(windowScene)
        window?.rootViewController?.view.setNeedsLayout()
        recordGeometry()
    }
    @available(iOS 26.0,*)
    public func windowScene(_ windowScene:UIWindowScene,didUpdateEffectiveGeometry previousEffectiveGeometry:UIWindowScene.Geometry) {
        window?.frame=sceneBounds(windowScene)
        window?.rootViewController?.view.setNeedsLayout()
        recordGeometry()
    }
    public func sceneDidBecomeActive(_ scene:UIScene) {
        UIApplication.shared.delegate?.applicationDidBecomeActive?(UIApplication.shared)
    }
    public func sceneWillResignActive(_ scene:UIScene) {
        UIApplication.shared.delegate?.applicationWillResignActive?(UIApplication.shared)
    }
    public func sceneWillEnterForeground(_ scene:UIScene) {
        UIApplication.shared.delegate?.applicationWillEnterForeground?(UIApplication.shared)
    }
    public func sceneDidEnterBackground(_ scene:UIScene) {
        UIApplication.shared.delegate?.applicationDidEnterBackground?(UIApplication.shared)
    }
    private func recordGeometry() {
        guard let window else {return}
        let size=window.bounds.size
        let state:[String:Any]=["width":size.width,"height":size.height,
            "safeTop":window.safeAreaInsets.top,"safeBottom":window.safeAreaInsets.bottom,
            "ipad":window.traitCollection.userInterfaceIdiom == .pad,
            "timestamp":Date().timeIntervalSince1970]
        if let data=try? JSONSerialization.data(withJSONObject:state,options:.sortedKeys) {
            try? data.write(to:URL.documentsDirectory.appendingPathComponent("window-layout-status.json"),options:.atomic)
        }
    }
}
