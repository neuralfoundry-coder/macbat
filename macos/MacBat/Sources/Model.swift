import AppKit
import SwiftUI

enum Glyph: CaseIterable { case none, charging, holding, discharging }

struct Note: Equatable {
    let text: String
    let error: Bool
}

@MainActor
final class BatteryModel: ObservableObject {
    @Published var status: Status?
    @Published var lowPower: Bool?
    /// macOS 14+: pmset "powermode" 0 automatic / 1 low power / 2 high power (replaces lowpowermode)
    @Published var powerMode: Int?
    @Published var problem: String?
    @Published var note: Note?
    @Published var busy = false
    /// the user explicitly chose to change charging although AlDente & co. run (this session only)
    @Published var takeOver = false

    let cli: String? = BatteryModel.findCLI()
    var snapshotMode = false
    private var timer: Timer?
    private var panelOpen = false
    private var loading = false

    init(autoStart: Bool = true) {
        guard autoStart else { return }
        refresh()
        reschedule()
        LoginItem.autoRegisterIfInstalled()
    }

    nonisolated static func findCLI() -> String? {
        let helper = Bundle.main.bundlePath + "/Contents/Helpers/macbat"
        return ["/usr/local/bin/macbat", "/opt/homebrew/bin/macbat", helper].first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    func setPanelOpen(_ open: Bool) {
        guard !snapshotMode else { return }
        panelOpen = open
        reschedule()
        if open { refresh() }
    }

    private func reschedule() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: panelOpen ? 3 : 20, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func refresh() {
        guard !loading else { return }
        guard let cli else { problem = L("macbat is not installed"); return }
        loading = true
        Task.detached(priority: .utility) {
            let r = BatteryModel.load(cli)
            await MainActor.run {
                self.apply(r)
                self.loading = false
            }
        }
    }

    func loadSync() { if let cli { apply(BatteryModel.load(cli)) } else { problem = L("macbat is not installed") } }

    nonisolated static func load(_ cli: String) -> (Status?, Bool?, Int?, String?) {
        let (rc, out, err) = Shell.run(cli, ["status", "--json"])
        var st: Status?
        var problem: String?
        if rc == 0 {
            st = try? JSONDecoder().decode(Status.self, from: Data(out.utf8))
            if st == nil { problem = L("Cannot read the battery status") }
        } else {
            let e = err.replacingOccurrences(of: "macbat: ", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
            problem = e.isEmpty ? L("Cannot read the battery status") : e
        }
        var lp: Bool?
        var pw: Int?
        let (_, pm, _) = Shell.run("/usr/bin/pmset", ["-g"])
        for line in pm.split(separator: "\n") {
            let t = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
            guard t.count >= 2 else { continue }
            if t[0] == "lowpowermode" { lp = t[1] == "1" }
            if t[0] == "powermode" { pw = Int(t[1]) }
        }
        return (st, lp, pw, problem)
    }

    private func apply(_ r: (Status?, Bool?, Int?, String?)) {
        if let s = r.0 { status = s }
        lowPower = r.1
        powerMode = r.2
        problem = r.3
    }

    // MARK: derived state

    var percent: Int { status?.battery.percent ?? 0 }
    var percentText: String? { status.map { "\($0.battery.percent)%" } }
    var limit: Int { status?.conf.limit ?? 80 }
    var isTopUp: Bool { status?.conf.topup ?? false }
    var adapterBypassed: Bool { status?.control?.adapter_off == 1 }
    var charging: Bool { status?.battery.state == "charging" }
    var holding: Bool { status?.control?.charging_blocked == 1 || (status?.mode.hasPrefix("holding") ?? false) }
    var conflict: String? { status?.conflict }
    var canWrite: Bool { status != nil && !busy && (conflict == nil || takeOver) }

    var glyph: Glyph {
        if adapterBypassed { return .discharging }
        if charging { return .charging }
        if holding { return .holding }
        return .none
    }

    var stateLine: String {
        guard let s = status else { return problem ?? L("Reading the battery…") }
        if adapterBypassed { return LF("Discharging to %d%%", limit) }
        if isTopUp && s.battery.percent < 100 { return L("Topping up to 100% once") }
        if charging { return limit < 100 ? LF("Charging to %d%%", limit) : L("Charging") }
        if holding { return LF("Holding at %d%%", limit) }
        if s.battery.state == "full" { return L("Fully charged") }
        if s.battery.ac == true { return L("Plugged in, not charging") }
        return L("On battery")
    }

    var timeLine: String? {
        guard let b = status?.battery else { return nil }
        if b.time_to_empty_min > 0 { return LF("%@ left", duration(b.time_to_empty_min)) }
        if b.time_to_full_min > 0 { return LF("%@ to full", duration(b.time_to_full_min)) }
        return nil
    }

    var accent: Color {
        switch status?.theme {
        case "ocean": return Color(hex: 0x2f8fe8)
        case "candy": return Color(hex: 0xf062b4)
        case "nord": return Color(hex: 0x5e81ac)
        case "retro": return Color(hex: 0x2fae4a)
        case "amber": return Color(hex: 0xe39b28)
        case "mono": return Color.gray
        default: return Color(hex: 0x3b8eea)
        }
    }
    var barColor: Color { percent < 20 ? .red : accent }

    // MARK: actions

    func perform(_ words: [String], done: String) {
        guard canWrite else { return }
        busy = true
        note = nil
        let force = takeOver && conflict != nil
        let cli = self.cli
        Task.detached(priority: .userInitiated) {
            let r = Control.send(words, force: force, cli: cli)
            await MainActor.run {
                self.busy = false
                switch r {
                case .ok: self.note = Note(text: done, error: false)
                case .failed(let m): self.note = Note(text: m, error: true)
                case .cancelled: self.note = Note(text: L("Cancelled"), error: true)
                }
                self.refresh()
            }
        }
    }

    /// The full settings screen of the CLI in Terminal (a .command file: no automation permission needed).
    func openSettingsInTerminal() {
        guard let cli else { return }
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("MacBat")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let f = dir.appendingPathComponent("macbat settings.command")
        try? "#!/bin/sh\nclear\nexec '\(cli)'\n".write(to: f, atomically: true, encoding: .utf8)
        try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: f.path)
        NSWorkspace.shared.open(f)
    }

    func about() {
        NSApp.activate(ignoringOtherApps: true)
        let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
        let credits = NSAttributedString(string: L("Battery charge limiter, fan control and battery status.") +
                                         "\nhttps://github.com/neuralfoundry-coder/macbat",
                                         attributes: [.font: NSFont.systemFont(ofSize: 11)])
        NSApp.orderFrontStandardAboutPanel(options: [.applicationName: "MacBat", .applicationVersion: v, .credits: credits])
    }
}
