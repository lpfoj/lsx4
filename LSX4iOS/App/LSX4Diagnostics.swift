// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
// Local import compatibility only: no transport, spool, MetricKit or crash hooks.
@objc(LSX4Diagnostics) final class LSX4Diagnostics:NSObject {
    static let shared=LSX4Diagnostics()
    @objc static func boot() {}
    func record(_ stage:String,_ details:[String:Any]=[:]) {}
    @discardableResult func startOperation(_ kind:String)->String {UUID().uuidString}
    func endOperation() {}
    func beforePicker(folder:Bool,completion:@escaping(Bool)->Void) {DispatchQueue.main.async {completion(true)}}
    func checkpoint(_ stage:String,details:[String:Any]=[:],completion:@escaping(Bool)->Void) {DispatchQueue.main.async {completion(true)}}
}
