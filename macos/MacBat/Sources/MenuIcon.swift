import AppKit

/// Template image for the menu bar: battery outline with the charge level, a glyph for the state
/// (bolt: charging, pause: holding at the limit, arrow: auto-discharging), optional percentage on the left.
enum MenuIcon {
    static func image(level: Int, glyph: Glyph, text: String?) -> NSImage {
        let font = NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.black]
        let textSize = text.map { ($0 as NSString).size(withAttributes: attrs) } ?? .zero
        let tw = text == nil ? 0 : ceil(textSize.width) + 4
        let height: CGFloat = 16
        let img = NSImage(size: NSSize(width: tw + 27, height: height), flipped: false) { _ in
            if let text { (text as NSString).draw(at: NSPoint(x: 0, y: (height - textSize.height) / 2), withAttributes: attrs) }
            let body = NSRect(x: tw + 0.5, y: 2.5, width: 23, height: 11)
            NSColor.black.set()
            let outline = NSBezierPath(roundedRect: body, xRadius: 3.2, yRadius: 3.2)
            outline.lineWidth = 1
            outline.stroke()
            NSColor.black.withAlphaComponent(0.55).setFill()
            NSBezierPath(roundedRect: NSRect(x: body.maxX + 1, y: 6, width: 1.8, height: 4), xRadius: 0.9, yRadius: 0.9).fill()
            // with a state glyph the charge is a light tint, so the glyph reads clearly at menu bar size
            NSColor.black.withAlphaComponent(glyph == .none ? 1 : 0.38).setFill()
            let inner = body.insetBy(dx: 2, dy: 2)
            let lv = CGFloat(max(0, min(100, level)))
            if lv > 0 {
                let w = max(1.6, inner.width * lv / 100)
                NSBezierPath(roundedRect: NSRect(x: inner.minX, y: inner.minY, width: w, height: inner.height), xRadius: 1.4, yRadius: 1.4).fill()
            }
            if let g = glyphPath(glyph, center: NSPoint(x: body.midX, y: body.midY)) {
                let ctx = NSGraphicsContext.current
                ctx?.compositingOperation = .clear   // cut a thin halo out of the tint ...
                g.lineWidth = 1.6
                g.lineJoinStyle = .round
                g.stroke()
                g.fill()
                ctx?.compositingOperation = .sourceOver
                NSColor.black.setFill()               // ... and draw the glyph inside it
                g.fill()
            }
            return true
        }
        img.isTemplate = true
        return img
    }

    static func glyphPath(_ g: Glyph, center c: NSPoint) -> NSBezierPath? {
        let p = NSBezierPath()
        switch g {
        case .none:
            return nil
        case .charging:
            let pts: [(CGFloat, CGFloat)] = [(1.6, 5.2), (-3.4, -0.6), (-0.3, -0.6), (-1.6, -5.2), (3.4, 0.6), (0.3, 0.6)]
            p.move(to: NSPoint(x: c.x + pts[0].0, y: c.y + pts[0].1))
            for q in pts.dropFirst() { p.line(to: NSPoint(x: c.x + q.0, y: c.y + q.1)) }
            p.close()
        case .holding:
            p.append(NSBezierPath(roundedRect: NSRect(x: c.x - 3.0, y: c.y - 3.4, width: 2.2, height: 6.8), xRadius: 0.6, yRadius: 0.6))
            p.append(NSBezierPath(roundedRect: NSRect(x: c.x + 0.8, y: c.y - 3.4, width: 2.2, height: 6.8), xRadius: 0.6, yRadius: 0.6))
        case .discharging:
            p.append(NSBezierPath(rect: NSRect(x: c.x - 0.95, y: c.y - 0.2, width: 1.9, height: 4.4)))
            p.move(to: NSPoint(x: c.x - 3.4, y: c.y + 0.2))
            p.line(to: NSPoint(x: c.x + 3.4, y: c.y + 0.2))
            p.line(to: NSPoint(x: c.x, y: c.y - 4.2))
            p.close()
        }
        return p
    }
}
