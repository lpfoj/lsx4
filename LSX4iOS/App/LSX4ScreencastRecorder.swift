// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit finite developer recording. Does nothing in ordinary launches.
import Foundation
import ReplayKit
import UIKit

@objc(LSX4ScreencastRecorder) @MainActor public final class LSX4ScreencastRecorder: NSObject {
    private static var instance: LSX4ScreencastRecorder?
    private var folder: URL!
    private var timer: Timer?
    private var sequence = 0
    private var phase = "ready"
    private var recording = false
    private var duration = 60.0
    private var started: Date?
    private var run = ""
    @objc public static func beginIfRequested() {
        guard let value=ProcessInfo.processInfo.environment["LSX4_SCREENCAST_RUN_ID"],
              let uuid=UUID(uuidString:value), uuid.uuidString.lowercased()==value else { return }
        let capture=LSX4ScreencastRecorder(); instance=capture;capture.run=value
        capture.folder=URL.documentsDirectory.appendingPathComponent("screencast-"+value)
        do { try FileManager.default.createDirectory(at:capture.folder,withIntermediateDirectories:true) }
        catch { return }
        capture.publish()
        capture.timer=Timer.scheduledTimer(withTimeInterval:0.25,repeats:true) { _ in
            MainActor.assumeIsolated { capture.poll() }
        }
        DispatchQueue.main.asyncAfter(deadline:.now()+600) {
            capture.timer?.invalidate(); if capture.recording { capture.stop() }
        }
    }
    private func publish(_ error:String?=nil) {
        let state:[String:Any] = ["runId":run,"sequence":sequence,"phase":phase,
            "recording":recording,"error":error ?? "","timestamp":Date().timeIntervalSince1970,
            "uiLanguage":UserDefaults.standard.string(forKey:"ui_language") ?? "",
            "executionMode":UserDefaults.standard.string(forKey:"lsx4.executionMode") ?? "",
            "microphoneEnabled":false,"output":"screen.mp4"]
        if let data=try? JSONSerialization.data(withJSONObject:state,options:.prettyPrinted) {
            try? data.write(to:folder.appendingPathComponent("status.json"),options:.atomic)
        }
    }
    private func poll() {
        guard let data=try? Data(contentsOf:folder.appendingPathComponent("command.json")),
              let packet=try? JSONSerialization.jsonObject(with:data) as? [String:Any],
              packet["runId"] as? String==run, let next=packet["sequence"] as? Int,
              next>sequence, next<=100 else { return }
        let action=packet["action"] as? String
        if action=="start", phase=="ready", let seconds=packet["seconds"] as? Double,
           seconds.isFinite, seconds>=5, seconds<=120 {
            sequence=next;duration=seconds;phase="awaiting-system-confirmation";publish()
            let recorder=RPScreenRecorder.shared();recorder.isMicrophoneEnabled=false
            recorder.startRecording { error in
                DispatchQueue.main.async {
                    if let error { self.phase="failed";self.publish(error.localizedDescription);return }
                    self.recording=true;self.phase="recording";self.started=Date();self.publish()
                    DispatchQueue.main.asyncAfter(deadline:.now()+self.duration) {
                        if self.recording { self.stop() }
                    }
                }
            }
        } else if action=="stop", recording {
            sequence=next;stop()
        }
    }
    private func stop() {
        guard recording else { return }
        recording=false;phase="saving";publish()
        RPScreenRecorder.shared().stopRecording(withOutput:folder.appendingPathComponent("screen.mp4")) { error in
            DispatchQueue.main.async {
                self.phase=error==nil ? "completed" : "failed";self.publish(error?.localizedDescription)
            }
        }
    }
}
