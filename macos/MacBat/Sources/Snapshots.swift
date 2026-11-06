import AppKit
import SwiftUI

/// `MacBat --render-snapshots DIR`: renders the panel and the menu bar item with live data into PNGs (light and
/// dark), the way the README shows them. Offscreen, nothing is written to the system, no screen recording needed.
@MainActor
enum Snapshots {
    static func render(to dir: URL) {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = BatteryModel(autoStart: false)
        model.snapshotMode = true
        model.loadSync()
        let lang = Bundle.main.preferredLocalizations.first ?? "en"
        for dark in [true, false] {
            let suffix = "\(lang)-\(dark ? "dark" : "light")"
            let panel = PanelView().environmentObject(model)
                .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color(nsColor: .windowBackgroundColor)))
                .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Color.primary.opacity(0.14)))
                .padding(16)
            save(panel, dark: dark, to: dir.appendingPathComponent("panel-\(suffix).png"))
            save(MenuBarStrip(model: model).padding(16), dark: dark, to: dir.appendingPathComponent("menubar-\(suffix).png"))
        }
    }

    static func save<V: View>(_ view: V, dark: Bool, to url: URL) {
        let appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
        let host = NSHostingView(rootView: view.environment(\.colorScheme, dark ? .dark : .light))
        host.appearance = appearance
        host.frame = NSRect(origin: .zero, size: host.fittingSize)
        let win = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        win.isOpaque = false
        win.backgroundColor = .clear
        win.appearance = appearance
        win.contentView = host
        host.layoutSubtreeIfNeeded()
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return }
        host.cacheDisplay(in: host.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: url)
        print(url.path)
    }
}

/// The item as it sits in the menu bar, and the four state glyphs.
struct MenuBarStrip: View {
    let model: BatteryModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 0) {
                Spacer()
                Image(nsImage: MenuIcon.image(level: model.percent, glyph: model.glyph, text: model.percentText))
                    .renderingMode(.template).foregroundStyle(.primary)
                    .padding(.horizontal, 8).padding(.vertical, 3)
                    .background(RoundedRectangle(cornerRadius: 4).fill(Color.primary.opacity(0.10)))
                    .padding(.trailing, 10)
            }
            .frame(width: 300, height: 26)
            .background(Rectangle().fill(.regularMaterial))
            .overlay(alignment: .bottom) { Rectangle().fill(Color.primary.opacity(0.12)).frame(height: 0.5) }
            HStack(alignment: .top, spacing: 0) {
                legend(.charging, L("Charging"))
                legend(.holding, L("Holding at the limit"))
                legend(.discharging, L("Discharging to the limit"))
                legend(.none, L("On battery"))
            }
            .frame(width: 300)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color(nsColor: .windowBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Color.primary.opacity(0.14)))
    }

    private func legend(_ g: Glyph, _ title: String) -> some View {
        VStack(spacing: 5) {
            Image(nsImage: MenuIcon.image(level: g == .none ? 64 : 80, glyph: g, text: nil))
                .renderingMode(.template).foregroundStyle(.primary)
            Text(verbatim: title).font(.system(size: 10)).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
    }
}
