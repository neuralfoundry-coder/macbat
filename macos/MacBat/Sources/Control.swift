import Darwin
import Foundation

/// Changes go to the root macbat service over /var/run/macbat.sock. The socket is root-owned, mode 0660, group
/// admin, and the service checks the peer's credentials on every connection (root, or an admin user who is logged
/// in at the screen). When the service is not reachable, MacBat runs the same `macbat` command through the standard
/// "administrator privileges" prompt instead. Requests only ever contain fixed words and numbers.
enum Control {
    enum Result {
        case ok(String)
        case failed(String)
        case cancelled
    }
    static let socketPath = "/var/run/macbat.sock"

    static func send(_ words: [String], force: Bool, cli: String?) -> Result {
        guard words.allSatisfy({ $0.range(of: "^[a-z0-9%]+$", options: .regularExpression) != nil }) else {
            return .failed("invalid request")
        }
        if let reply = viaSocket(((force ? ["force"] : []) + words).joined(separator: " ")) {
            if reply.hasPrefix("ok") { return .ok(String(reply.dropFirst(2)).trimmingCharacters(in: .whitespaces)) }
            return .failed(String(reply.dropFirst(3)).trimmingCharacters(in: .whitespaces))
        }
        return viaAdminPrompt(words, force: force, cli: cli)
    }

    /// nil when the service is not listening (not installed / not running / no permission to connect)
    static func viaSocket(_ line: String) -> String? {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var tv = timeval(tv_sec: 20, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let cap = MemoryLayout.size(ofValue: addr.sun_path)
        withUnsafeMutablePointer(to: &addr.sun_path) {
            $0.withMemoryRebound(to: CChar.self, capacity: cap) { _ = strlcpy($0, socketPath, cap) }
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard ok == 0 else { return nil }
        let req = Array((line + "\n").utf8)
        guard req.withUnsafeBytes({ write(fd, $0.baseAddress, $0.count) }) == req.count else { return nil }
        var buf = [UInt8](repeating: 0, count: 512)
        var got = 0
        while got < buf.count {
            let n = buf.withUnsafeMutableBytes { read(fd, $0.baseAddress! + got, $0.count - got) }
            if n <= 0 { break }
            got += n
            if buf[..<got].contains(10) { break }
        }
        return got > 0 ? String(decoding: buf[..<got], as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines) : nil
    }

    /// The service is not running: the same change through `do shell script ... with administrator privileges`.
    static func viaAdminPrompt(_ words: [String], force: Bool, cli: String?) -> Result {
        var argv: [String]
        if words.first == "lowpower" {
            argv = ["/usr/bin/pmset", "-a", "lowpowermode", words.last == "on" ? "1" : "0"]
        } else if words.first == "powermode", let v = words.last, ["0", "1", "2"].contains(v) {
            argv = ["/usr/bin/pmset", "-a", "powermode", v]
        } else {
            guard let cli else { return .failed(L("macbat is not installed")) }
            argv = [cli] + words + (force ? ["--force"] : [])
        }
        let cmd = argv.map { "'" + $0.replacingOccurrences(of: "'", with: "") + "'" }.joined(separator: " ")
        let script = "do shell script \"\(cmd)\" with administrator privileges"
        let (rc, out, err) = Shell.run("/usr/bin/osascript", ["-e", script])
        if rc == 0 { return .ok(out.trimmingCharacters(in: .whitespacesAndNewlines)) }
        if err.contains("-128") { return .cancelled }
        let msg = err.replacingOccurrences(of: "macbat: ", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
        return .failed(msg.isEmpty ? "exit \(rc)" : msg)
    }
}
