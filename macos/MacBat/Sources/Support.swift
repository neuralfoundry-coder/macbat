import AppKit
import ServiceManagement
import SwiftUI

/// Localized string (en / ko, following the system language).
func L(_ key: String) -> String { NSLocalizedString(key, comment: "") }
func LF(_ key: String, _ args: CVarArg...) -> String { String(format: NSLocalizedString(key, comment: ""), arguments: args) }

/// Locale of the language the app is actually showing (for durations / numbers).
let appLocale = Locale(identifier: Bundle.main.preferredLocalizations.first ?? "en")

func duration(_ minutes: Int) -> String {
    let f = DateComponentsFormatter()
    f.allowedUnits = minutes >= 60 ? [.hour, .minute] : [.minute]
    f.unitsStyle = .short
    var cal = Calendar.current
    cal.locale = appLocale
    f.calendar = cal
    return f.string(from: TimeInterval(minutes * 60)) ?? "\(minutes) min"
}

extension Color {
    init(hex: UInt32) {
        self.init(.sRGB, red: Double(hex >> 16 & 255) / 255, green: Double(hex >> 8 & 255) / 255, blue: Double(hex & 255) / 255)
    }
}

/// Start at login with SMAppService (macOS 13+). Registered once automatically when MacBat runs from
/// /Applications; the toggle in the panel's menu turns it off or on again.
enum LoginItem {
    static var enabled: Bool { SMAppService.mainApp.status == .enabled }
    static func set(_ on: Bool) {
        do { if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() } } catch {
            NSLog("MacBat: login item: \(error.localizedDescription)")
        }
    }
    static func autoRegisterIfInstalled() {
        let d = UserDefaults.standard
        guard Bundle.main.bundlePath.hasPrefix("/Applications/"), !d.bool(forKey: "didAutoRegisterLoginItem") else { return }
        d.set(true, forKey: "didAutoRegisterLoginItem")
        if SMAppService.mainApp.status != .enabled { set(true) }
    }
}

enum Shell {
    /// Runs a program, returns (exit status, stdout, stderr). Never goes through a shell.
    static func run(_ path: String, _ args: [String]) -> (Int32, String, String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["NO_COLOR"] = "1"
        env["LC_ALL"] = "en_US.UTF-8"
        p.environment = env
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { return (-1, "", error.localizedDescription) }
        let o = out.fileHandleForReading.readDataToEndOfFile()
        let e = err.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(decoding: o, as: UTF8.self), String(decoding: e, as: UTF8.self))
    }
}
