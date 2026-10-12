import Foundation

/// Sends apps that ignore the system proxy (e.g. Discord's updater) to Kite's
/// transparent TLS listener by pointing their hostnames at loopback.
enum HostsFile {
    static let domains = [
        "discord.com", "www.discord.com", "ptb.discord.com", "canary.discord.com",
        "updates.discord.com", "status.discord.com", "cdn.discord.com",
        "discordapp.com", "www.discordapp.com", "cdn.discordapp.com",
        "media.discordapp.net", "images-ext-1.discordapp.net", "images-ext-2.discordapp.net",
        "dl.discordapp.net", "router.discordapp.net",
        "discord.gg", "gateway.discord.gg", "discord.media", "discord.new",
        "discordstatus.com",
    ]

    private static let path = "/etc/hosts"
    private static let begin = "# >>> Kite DPI >>>"
    private static let end = "# <<< Kite DPI <<<"
    /// markers written by 0.1.x, still recognized so they get replaced
    private static let legacyBegin = "# >>> Kite (Discord) >>>"
    private static let legacyEnd = "# <<< Kite (Discord) <<<"

    static var isInstalled: Bool {
        (try? String(contentsOfFile: path, encoding: .utf8))?.contains(begin) ?? false
    }

    static func install() throws {
        var lines = [begin]
        for d in domains {
            lines.append("127.0.0.1 \(d)")
            lines.append("::1 \(d)")
        }
        lines.append(end)
        try write(stripped() + "\n" + lines.joined(separator: "\n") + "\n")
    }

    static func remove() throws {
        guard isInstalled else { return }
        try write(stripped() + "\n")
    }

    /// Current hosts file without Kite's block.
    private static func stripped() throws -> String {
        let text = try String(contentsOfFile: path, encoding: .utf8)
        var out: [Substring] = []
        var inside = false
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            if line == begin || line == legacyBegin { inside = true; continue }
            if line == end || line == legacyEnd { inside = false; continue }
            if !inside { out.append(line) }
        }
        while out.last?.isEmpty == true { out.removeLast() }
        return out.joined(separator: "\n")
    }

    /// /etc/hosts is root-owned: write a temp copy and install it with one
    /// administrator prompt, then flush the DNS cache.
    private static func write(_ content: String) throws {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("kite-hosts")
        try content.write(to: tmp, atomically: true, encoding: .utf8)
        let cmd = "/bin/cp '\(tmp.path)' \(path) && /bin/chmod 644 \(path) && (/usr/bin/dscacheutil -flushcache; /usr/bin/killall mDNSResponder; true)"
        let source = "do shell script \"\(cmd)\" with administrator privileges"
        var error: NSDictionary?
        NSAppleScript(source: source)?.executeAndReturnError(&error)
        try? FileManager.default.removeItem(at: tmp)
        if let error {
            throw SystemProxy.Failure.denied(error[NSAppleScript.errorMessage] as? String ?? "izin verilmedi")
        }
    }
}
