import AppKit
import SwiftUI

struct PanelView: View {
    @EnvironmentObject var m: BatteryModel
    @AppStorage("showPercent") private var showPercent = true
    @State private var loginItem = LoginItem.enabled
    @State private var limitDraft: Double?
    @State private var fanDraft: Double?
    @State private var askTakeOver = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            header
            if let c = m.conflict {
                conflictBanner(c)
            } else if let s = m.status, s.service != "active" {
                Label(L("The macbat service is not running - changes ask for an administrator password."), systemImage: "info.circle")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if m.status == nil, let p = m.problem { problemView(p) }
            if m.status != nil {
                Divider()
                chargingSection
                Divider()
                detailsSection
                if let fans = m.status?.fans, !fans.isEmpty {
                    Divider()
                    fanSection(fans)
                }
            }
            if let n = m.note {
                Label(n.text, systemImage: n.error ? "exclamationmark.circle" : "checkmark.circle")
                    .font(.caption).foregroundStyle(n.error ? Color.red : Color.secondary)
            }
            Divider()
            footer
        }
        .padding(16)
        .frame(width: 340)
        .tint(m.accent)
        .onAppear { m.setPanelOpen(true); loginItem = LoginItem.enabled }
        .onDisappear { m.setPanelOpen(false) }
    }

    // MARK: header: big number, state, gauge with the limit

    private var stateSymbol: String {
        switch m.glyph {
        case .charging: return "bolt.fill"
        case .holding: return "pause.circle.fill"
        case .discharging: return "arrow.down.circle.fill"
        case .none: return m.status?.battery.ac == true ? "powerplug.fill" : "battery.75percent"
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .center, spacing: 12) {
                Text(verbatim: m.percentText ?? "–")
                    .font(.system(size: 40, weight: .semibold, design: .rounded))
                    .monospacedDigit()
                VStack(alignment: .leading, spacing: 3) {
                    Label(m.stateLine, systemImage: stateSymbol)
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(m.glyph == .none ? Color.primary : m.accent)
                    if let t = m.timeLine {
                        Text(verbatim: t).font(.caption).foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }
            ChargeBar(percent: m.percent, limit: m.status == nil ? 100 : (m.isTopUp ? 100 : m.limit), color: m.barColor)
        }
    }

    private func conflictBanner(_ app: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(LF("%@ is managing charging", app), systemImage: "exclamationmark.triangle.fill")
                .font(.system(size: 12, weight: .semibold)).foregroundStyle(.orange)
            Text(verbatim: m.takeOver ? LF("You took over: MacBat changes charging although %@ runs.", app)
                                      : L("MacBat stays read-only so the two don't fight."))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if !m.takeOver {
                Button(L("Take over…")) { askTakeOver = true }.controlSize(.small)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.orange.opacity(0.12)))
        .alert(LF("Take over charging from %@?", app), isPresented: $askTakeOver) {
            Button(L("Take Over"), role: .destructive) { m.takeOver = true }
            Button(L("Cancel"), role: .cancel) {}
        } message: {
            Text(verbatim: LF("Both apps would change the same SMC keys. Quit %@ first if you can.", app))
        }
    }

    private func problemView(_ p: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(p, systemImage: "exclamationmark.triangle").font(.callout)
            if m.cli == nil {
                Text(verbatim: L("Install the command-line tool:")).font(.caption).foregroundStyle(.secondary)
                Text(verbatim: "curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh")
                    .font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
            }
        }
    }

    // MARK: charging

    private var chargingSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(verbatim: L("Charge limit")).font(.system(size: 13, weight: .medium))
                Spacer()
                Text(verbatim: "\(Int(limitDraft ?? Double(m.limit)))%").monospacedDigit().foregroundStyle(.secondary)
            }
            Slider(value: Binding(get: { limitDraft ?? Double(m.limit) }, set: { limitDraft = $0 }), in: 20...100, step: 5) { editing in
                guard !editing, let v = limitDraft else { return }
                let n = Int(v)
                if n != m.limit { m.perform(["limit", "\(n)"], done: LF("Charge limit set to %d%%", n)) }
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { limitDraft = nil }
            }
            .disabled(!m.canWrite)
            SwitchRow(title: L("Auto-discharge above the limit"), isOn: m.status?.conf.auto_discharge ?? false,
                      enabled: m.canWrite && m.status?.caps?.discharge != nil) { on in
                m.perform(["discharge", on ? "on" : "off"], done: on ? L("Auto-discharge on") : L("Auto-discharge off"))
            }
            if let pw = m.powerMode {
                HStack {
                    Text(verbatim: L("Energy mode"))
                    Spacer()
                    Picker(selection: Binding(get: { pw }, set: { v in m.perform(["powermode", "\(v)"], done: L("Energy mode changed")) }),
                           label: EmptyView()) {
                        Text(verbatim: L("Automatic")).tag(0)
                        Text(verbatim: L("Low Power")).tag(1)
                        Text(verbatim: L("High Power")).tag(2)
                    }
                    .labelsHidden().fixedSize().disabled(!m.canWrite)
                }
            } else if let lp = m.lowPower {
                SwitchRow(title: L("Low Power Mode"), isOn: lp, enabled: m.canWrite) { on in
                    m.perform(["lowpower", on ? "on" : "off"], done: on ? L("Low Power Mode on") : L("Low Power Mode off"))
                }
            }
            Button {
                m.isTopUp ? m.perform(["topup", "off"], done: L("Top-up cancelled"))
                          : m.perform(["topup", "on"], done: L("Charging to 100% once, then back to the limit"))
            } label: {
                Label(m.isTopUp ? L("Cancel top-up") : L("Top up to 100% once"),
                      systemImage: m.isTopUp ? "xmark.circle" : "arrow.up.to.line.compact")
                    .frame(maxWidth: .infinity)
            }
            .controlSize(.regular)
            .disabled(!m.canWrite)
        }
    }

    // MARK: details

    private var detailsSection: some View {
        let b = m.status!.battery
        var power = b.ac == true ? L("Power adapter") : L("Battery")
        if m.adapterBypassed { power = L("Adapter bypassed") } else if b.ac == true, b.adapter_w > 0 { power += " · \(b.adapter_w) W" }
        let health = b.health_reliable && b.health != nil ? String(format: "%.1f %%", b.health!) : L("n/a (uncalibrated)")
        var electric = [String]()
        if let v = b.voltage { electric.append(String(format: "%.2f V", v)) }
        if let a = b.current { electric.append(String(format: "%+.2f A", a)) }
        return Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 7) {
            row("clock", L("Time remaining"), m.timeLine ?? "–")
            row("powerplug", L("Power source"), power)
            row("heart", L("Health"), health)
            row("arrow.triangle.2.circlepath", L("Cycles"), b.cycles >= 0 ? "\(b.cycles)" : "–")
            row("thermometer.medium", L("Temperature"), b.temp_c.map { String(format: "%.1f °C", $0) } ?? "–")
            row("bolt", L("Voltage / current"), electric.isEmpty ? "–" : electric.joined(separator: " · "))
        }
        .font(.system(size: 12))
    }

    private func row(_ symbol: String, _ title: String, _ value: String) -> some View {
        GridRow {
            Image(systemName: symbol).foregroundStyle(.secondary).frame(width: 16)
            Text(verbatim: title).foregroundStyle(.secondary)
            Text(verbatim: value).monospacedDigit().frame(maxWidth: .infinity, alignment: .trailing)
        }
    }

    // MARK: fans

    private func fanSection(_ fans: [Status.Fan]) -> some View {
        let fixed = (m.status?.fan_setting ?? "auto") != "auto"
        let lo = Double(fans.map(\.min).max() ?? 1000), hi = Double(max(fans.map(\.max).min() ?? 6000, Int(lo) + 100))
        let current = Double(fans.first?.target ?? Int(lo))
        return VStack(alignment: .leading, spacing: 9) {
            HStack {
                Label(L("Fans"), systemImage: "fanblades").font(.system(size: 13, weight: .medium))
                Spacer()
                Text(verbatim: fans.map { "\($0.rpm)" }.joined(separator: " · ") + " rpm").monospacedDigit().foregroundStyle(.secondary)
            }
            Picker(selection: Binding(get: { fixed }, set: { on in
                if on {
                    let rpm = Int((min(max(current, lo), hi) / 100).rounded() * 100)
                    m.perform(["fan", "set", "\(rpm)"], done: LF("Fans fixed at %d rpm", rpm))
                } else { m.perform(["fan", "auto"], done: L("Fans automatic")) }
            }), label: EmptyView()) {
                Text(verbatim: L("Auto")).tag(false)
                Text(verbatim: L("Fixed")).tag(true)
            }
            .pickerStyle(.segmented).labelsHidden().disabled(!m.canWrite)
            if fixed {
                HStack {
                    Slider(value: Binding(get: { fanDraft ?? current }, set: { fanDraft = $0 }), in: lo...hi, step: 100) { editing in
                        guard !editing, let v = fanDraft else { return }
                        m.perform(["fan", "set", "\(Int(v))"], done: LF("Fans fixed at %d rpm", Int(v)))
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { fanDraft = nil }
                    }
                    .disabled(!m.canWrite)
                    Text(verbatim: "\(Int(fanDraft ?? current)) rpm").monospacedDigit().foregroundStyle(.secondary).frame(width: 74, alignment: .trailing)
                }
            }
            Text(verbatim: L("Fan control on macOS is experimental.")).font(.caption2).foregroundStyle(.tertiary)
        }
    }

    // MARK: footer

    private var footer: some View {
        HStack(spacing: 10) {
            Button { m.openSettingsInTerminal() } label: {
                Label(L("Settings in Terminal"), systemImage: "terminal")
            }
            .buttonStyle(.borderless)
            .disabled(m.cli == nil)
            Spacer()
            Menu {
                if let s = m.status {
                    Text(verbatim: "\(s.model) · macbat \(s.version)")
                    Divider()
                }
                Toggle(L("Show percentage in the menu bar"), isOn: $showPercent)
                Toggle(L("Launch at login"), isOn: Binding(get: { loginItem }, set: { LoginItem.set($0); loginItem = LoginItem.enabled }))
                Divider()
                Button(L("About MacBat")) { m.about() }
                Button(L("Quit MacBat")) { NSApp.terminate(nil) }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
        }
    }
}

/// Label on the left, switch on the right.
struct SwitchRow: View {
    let title: String
    let isOn: Bool
    let enabled: Bool
    let change: (Bool) -> Void

    var body: some View {
        HStack {
            Text(verbatim: title)
            Spacer()
            Toggle(isOn: Binding(get: { isOn }, set: { change($0) })) { EmptyView() }
                .labelsHidden().toggleStyle(.switch).controlSize(.small).disabled(!enabled)
        }
    }
}

/// Slim charge gauge with the limit marked.
struct ChargeBar: View {
    let percent: Int
    let limit: Int
    let color: Color

    var body: some View {
        GeometryReader { g in
            let w = g.size.width
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.10))
                Capsule().fill(color.gradient).frame(width: max(8, w * CGFloat(min(max(percent, 0), 100)) / 100))
                if limit < 100 {
                    RoundedRectangle(cornerRadius: 1)
                        .fill(Color.primary.opacity(0.7))
                        .frame(width: 2, height: 16)
                        .offset(x: w * CGFloat(limit) / 100 - 1)
                }
            }
        }
        .frame(height: 8)
        .padding(.vertical, 4)
        .animation(.easeOut(duration: 0.35), value: percent)
    }
}
