// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import GameController

struct LSX4PadState {
    var mask:UInt32=0
    var axes:[UInt8]=[128,128,128,128,0,0]
    var touch:CGPoint?
    var connected=false
    static let neutral=LSX4PadState()

    static func merge(touch:LSX4PadState,external:LSX4PadState)->LSX4PadState {
        var result=touch
        result.mask |= external.mask
        result.connected = touch.connected || external.connected
        for i in 0..<4 where external.axes[i] != 128 {result.axes[i]=external.axes[i]}
        for i in 4..<6 {result.axes[i]=max(touch.axes[i],external.axes[i])}
        result.touch=touch.touch ?? external.touch
        return result
    }
}

// Uses Apple's hardware-independent profiles: no vendor allowlist or polling loop.
// The main queue serializes hardware events with the existing touch input sink.
final class LSX4ExternalGamepad {
    private let available:()->[GCController]
    private let changed:(LSX4PadState)->Void
    private var controllers:[ObjectIdentifier:GCController]=[:]
    private var selected:GCController?
    private var observers:[NSObjectProtocol]=[]
    private var enabled=true
    private var events=0
    private var connections=0
    private var disconnections=0
    private var lastStatus=Date.distantPast
    private(set) var state=LSX4PadState.neutral

    init(available:@escaping ()->[GCController]={GCController.controllers()},changed:@escaping (LSX4PadState)->Void) {
        self.available=available;self.changed=changed
    }
    deinit {stop()}
    func start() {
        guard observers.isEmpty else {refresh();return}
        let center=NotificationCenter.default
        observers.append(center.addObserver(forName:.GCControllerDidConnect,object:nil,queue:.main) { [weak self] n in
            if let controller=n.object as? GCController {self?.connect(controller)}
        })
        observers.append(center.addObserver(forName:.GCControllerDidDisconnect,object:nil,queue:.main) { [weak self] n in
            if let controller=n.object as? GCController {self?.disconnect(controller)}
        })
        // Register before enumerating so a connection during startup cannot be missed.
        for controller in available() {connect(controller)}
        refresh()
    }
    func stop() {
        for token in observers {NotificationCenter.default.removeObserver(token)}
        observers.removeAll()
        for controller in controllers.values {unbind(controller)}
        controllers.removeAll();selected=nil;state = .neutral;changed(state)
    }
    func setEnabled(_ value:Bool) {enabled=value;refresh()}
    func refresh() {state=enabled ? selected.map(Self.read) ?? .neutral : .neutral;changed(state)}
    private func connect(_ controller:GCController) {
        guard controller.extendedGamepad != nil || controller.gamepad != nil || controller.microGamepad != nil else {return}
        let id=ObjectIdentifier(controller)
        guard controllers[id]==nil else {return}
        controllers[id]=controller;connections+=1
        controller.handlerQueue = .main
        if let pad=controller.extendedGamepad {
            pad.valueChangedHandler={ [weak self,weak controller] _,_ in if let controller {self?.input(controller)} }
        } else if let pad=controller.gamepad {
            pad.valueChangedHandler={ [weak self,weak controller] _,_ in if let controller {self?.input(controller)} }
            controller.controllerPausedHandler={ [weak self] _ in self?.legacyPause() }
        } else if let pad=controller.microGamepad {
            pad.valueChangedHandler={ [weak self,weak controller] _,_ in if let controller {self?.input(controller)} }
        }
        if selected==nil {selected=controller}
        refresh();record("connected")
    }
    private func unbind(_ controller:GCController) {
        controller.extendedGamepad?.valueChangedHandler=nil
        controller.gamepad?.valueChangedHandler=nil
        controller.microGamepad?.valueChangedHandler=nil
        controller.controllerPausedHandler=nil
    }
    private func disconnect(_ controller:GCController) {
        guard controllers.removeValue(forKey:ObjectIdentifier(controller)) != nil else {return}
        unbind(controller);disconnections+=1
        if selected === controller {selected=controllers.values.first}
        // Publish a complete new state so no released button or stick stays latched.
        refresh();record("disconnected")
    }
    private func input(_ controller:GCController) {
        guard controllers[ObjectIdentifier(controller)] != nil else {return}
        selected=controller;events+=1;refresh()
        if events==1 || Date().timeIntervalSince(lastStatus)>=2 {record("input")}
    }
    private func legacyPause() {
        guard enabled else {return}
        var pressed=state;pressed.mask |= 8;changed(pressed)
        DispatchQueue.main.asyncAfter(deadline:.now()+0.08) { [weak self] in self?.refresh() }
    }
    private func record(_ phase:String) {
        lastStatus=Date()
        let info:[String:Any]=["phase":phase,"controllers":controllers.count,"connections":connections,
            "disconnections":disconnections,"inputEvents":events,"enabled":enabled,
            "vendor":selected?.vendorName ?? "","mask":state.mask,"axes":state.axes,
            "version":Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "",
            "timestamp":Date().timeIntervalSince1970]
        guard let url=FileManager.default.urls(for:.documentDirectory,in:.userDomainMask).first,
              let data=try? JSONSerialization.data(withJSONObject:info,options:.sortedKeys) else {return}
        try? data.write(to:url.appendingPathComponent("external-controller-status.json"),options:.atomic)
    }
    private static func axis(_ value:Float,inverted:Bool=false)->UInt8 {
        let v=max(-1,min(1,inverted ? -value : value))
        return UInt8(max(0,min(255,(128+v*(v<0 ? 128 : 127)).rounded())))
    }
    static func read(_ controller:GCController)->LSX4PadState {
        var result=LSX4PadState();result.connected=true
        func button(_ value:GCControllerButtonInput?,_ bit:UInt32) {if value?.isPressed==true {result.mask |= bit}}
        func dpad(_ value:GCControllerDirectionPad) {
            button(value.up,0x10);button(value.right,0x20);button(value.down,0x40);button(value.left,0x80)
        }
        if let p=controller.extendedGamepad {
            dpad(p.dpad)
            button(p.buttonA,0x4000);button(p.buttonB,0x2000);button(p.buttonX,0x8000);button(p.buttonY,0x1000)
            button(p.leftShoulder,0x400);button(p.rightShoulder,0x800)
            button(p.leftTrigger,0x100);button(p.rightTrigger,0x200)
            button(p.leftThumbstickButton,2);button(p.rightThumbstickButton,4)
            button(p.buttonMenu,8);button(p.buttonOptions,1);button(p.buttonHome,0x10000)
            result.axes=[axis(p.leftThumbstick.xAxis.value),axis(p.leftThumbstick.yAxis.value,inverted:true),
                         axis(p.rightThumbstick.xAxis.value),axis(p.rightThumbstick.yAxis.value,inverted:true),
                         UInt8(max(0,min(255,(p.leftTrigger.value*255).rounded()))),
                         UInt8(max(0,min(255,(p.rightTrigger.value*255).rounded())))]
            if let sony=p as? GCDualShockGamepad {
                button(sony.touchpadButton,0x100000)
                if sony.touchpadPrimary.xAxis.value != 0 || sony.touchpadPrimary.yAxis.value != 0 {
                    result.touch=CGPoint(x:Double(sony.touchpadPrimary.xAxis.value+1)/2,y:Double(1-sony.touchpadPrimary.yAxis.value)/2)
                }
            }
            if let sony=p as? GCDualSenseGamepad {
                button(sony.touchpadButton,0x100000)
                if sony.touchpadPrimary.xAxis.value != 0 || sony.touchpadPrimary.yAxis.value != 0 {
                    result.touch=CGPoint(x:Double(sony.touchpadPrimary.xAxis.value+1)/2,y:Double(1-sony.touchpadPrimary.yAxis.value)/2)
                }
            }
        } else if let p=controller.gamepad {
            dpad(p.dpad);button(p.buttonA,0x4000);button(p.buttonB,0x2000);button(p.buttonX,0x8000);button(p.buttonY,0x1000)
            button(p.leftShoulder,0x400);button(p.rightShoulder,0x800)
        } else if let p=controller.microGamepad {
            dpad(p.dpad);button(p.buttonA,0x4000);button(p.buttonX,0x8000);button(p.buttonMenu,8)
        }
        return result
    }
}
