import Foundation

/// Points macOS' HTTP/HTTPS proxy at Kite and restores the previous settings.
///
/// The original settings are written to disk before anything changes, so a
/// crash or forced quit is repaired on the next launch.
enum SystemProxy {
    private struct Saved: Codable {
        struct Proxy: Codable {
            var enabled: Bool
            var server: String
            var port: String
        }
        var web: [String: Proxy] = [:]
        var secure: [String: Proxy] = [:]
        /// port Kite listened on when the backup was taken (older backups: 8899)
        var kitePort: String?
    }

    private static var stateURL: URL {
        let dir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Kite")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("proxy-backup.json")
    }

    static var isApplied: Bool { FileManager.default.fileExists(atPath: stateURL.path) }

    /// Enabled network services, e.g. "Wi-Fi", "USB 10/100/1000 LAN".
    private static func services() -> [String] {
        run(["-listallnetworkservices"]).output
            .split(separator: "\n")
            .dropFirst() // "An asterisk (*) denotes ..."
            .map(String.init)
            .filter { !$0.hasPrefix("*") && !$0.isEmpty }
    }

    private static func read(_ flag: String, _ service: String) -> Saved.Proxy {
        var p = Saved.Proxy(enabled: false, server: "", port: "0")
        for line in run([flag, service]).output.split(separator: "\n") {
            let parts = line.split(separator: ":", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            guard parts.count == 2 else { continue }
            switch parts[0] {
            case "Enabled": p.enabled = parts[1] == "Yes"
            case "Server": p.server = parts[1]
            case "Port": p.port = parts[1]
            default: break
            }
        }
        return p
    }

    static func apply(port: UInt16) throws {
        var saved = Saved()
        saved.kitePort = "\(port)"
        if !isApplied {
            for s in services() {
                saved.web[s] = read("-getwebproxy", s)
                saved.secure[s] = read("-getsecurewebproxy", s)
            }
            try JSONEncoder().encode(saved).write(to: stateURL)
        }
        var commands: [[String]] = []
        for s in services() {
            commands.append(["-setwebproxy", s, "127.0.0.1", "\(port)"])
            commands.append(["-setsecurewebproxy", s, "127.0.0.1", "\(port)"])
        }
        try runAll(commands)
    }

    static func restore() throws {
        guard isApplied, let data = try? Data(contentsOf: stateURL),
              let saved = try? JSONDecoder().decode(Saved.self, from: data) else {
            try? FileManager.default.removeItem(at: stateURL)
            return
        }
        var commands: [[String]] = []
        for (flag, map) in [("web", saved.web), ("secure", saved.secure)] {
            let set = flag == "web" ? "-setwebproxy" : "-setsecurewebproxy"
            let state = flag == "web" ? "-setwebproxystate" : "-setsecurewebproxystate"
            for (s, p) in map {
                // a proxy that pointed at Kite itself counts as "was off"
                let ours = p.server == "127.0.0.1" && p.port == (saved.kitePort ?? "8899")
                if p.enabled && !ours {
                    commands.append([set, s, p.server, p.port])
                } else {
                    commands.append([state, s, "off"])
                }
            }
        }
        try runAll(commands)
        try? FileManager.default.removeItem(at: stateURL)
    }

    // MARK: - networksetup

    enum Failure: LocalizedError {
        case denied(String)
        var errorDescription: String? {
            if case .denied(let m) = self { return tr("Proxy ayarı değiştirilemedi: ", "Could not change the proxy settings: ") + m }
            return nil
        }
    }

    @discardableResult
    private static func run(_ args: [String]) -> (status: Int32, output: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/networksetup")
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do { try p.run() } catch { return (-1, error.localizedDescription) }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(decoding: data, as: UTF8.self))
    }

    /// Runs the commands as the user; if macOS demands admin rights, asks once
    /// for the password and runs them all in a single privileged shell.
    private static func runAll(_ commands: [[String]]) throws {
        var needsAdmin = false
        for c in commands {
            let r = run(c)
            if r.status != 0 || r.output.lowercased().contains("admin") {
                needsAdmin = true
                break
            }
        }
        guard needsAdmin else { return }

        let script = commands
            .map { (["/usr/sbin/networksetup"] + $0).map(shellQuote).joined(separator: " ") }
            .joined(separator: " && ")
        let source = "do shell script \"\(script.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        var error: NSDictionary?
        NSAppleScript(source: source)?.executeAndReturnError(&error)
        if let error {
            throw Failure.denied(error[NSAppleScript.errorMessage] as? String ?? "izin verilmedi")
        }
    }

    private static func shellQuote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
