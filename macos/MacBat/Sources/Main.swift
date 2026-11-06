// MacBat - menu bar companion of the macbat command-line tool (macOS 13+).
// Reads everything as the normal user via `macbat status --json`; changes go to the root service over its
// control socket, or - when the service is not running - through the standard administrator password prompt.
import AppKit
import SwiftUI

@main
enum Main {
    static func main() {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--render-snapshots"), i + 1 < args.count {
            // README screenshots: renders the panel and the menu bar icon with live data, writes PNGs, exits
            MainActor.assumeIsolated { Snapshots.render(to: URL(fileURLWithPath: args[i + 1])) }
            exit(0)
        }
        MacBatApp.main()
    }
}

struct MacBatApp: App {
    @StateObject private var model = BatteryModel()

    var body: some Scene {
        MenuBarExtra {
            PanelView().environmentObject(model)
        } label: {
            MenuBarLabel(model: model)
        }
        .menuBarExtraStyle(.window)
    }
}

/// The menu bar item: one template image (battery glyph + optional percentage), so it is crisp and follows the
/// menu bar's light / dark tint.
struct MenuBarLabel: View {
    @ObservedObject var model: BatteryModel
    @AppStorage("showPercent") private var showPercent = true

    var body: some View {
        Image(nsImage: MenuIcon.image(level: model.status?.battery.percent ?? 0, glyph: model.glyph,
                                      text: showPercent ? model.percentText : nil))
            .accessibilityLabel(Text(verbatim: "MacBat \(model.percentText ?? "") \(model.stateLine)"))
    }
}
