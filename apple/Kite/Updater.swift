import AppKit
import SwiftUI

/// Checks GitHub releases for a newer version and installs it in place.
///
/// The new app is downloaded as the release DMG, its Developer ID signature
/// is verified, and a small shell script swaps the bundle once this process
/// has quit (quitting restores the system proxy), then relaunches it.
@MainActor
final class Updater: ObservableObject {
    static let shared = Updater()

    enum State: Equatable {
        case idle
        case checking
        case upToDate
        case available(version: String)
        case installing
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    private static let latestURL = URL(string: "https://api.github.com/repos/bkrdmrcioglu/Kite/releases/latest")!
    nonisolated private static let teamID = "HLZQLSTBB8"
    private var dmgURL: URL?
    private var timer: Timer?

    var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    func startChecking() {
        Task { await check() }
        timer = Timer.scheduledTimer(withTimeInterval: 6 * 60 * 60, repeats: true) { _ in
            Task { @MainActor in await Updater.shared.check() }
        }
    }

    static let credit = "Bekir Demircioğlu tarafından sevgiyle geliştirildi"

    /// "Güncellemeleri kontrol et": same check, but reports "up to date"
    /// and network errors instead of staying silent.
    func checkNow() {
        guard state != .installing, state != .checking else { return }
        state = .checking
        Task {
            let found = await check()
            if case .available = state { return }
            state = found == nil ? .failed("Güncellemeler kontrol edilemedi. İnternet bağlantını kontrol et.") : .upToDate
            // let the result fade back to the plain button after a while
            try? await Task.sleep(for: .seconds(6))
            if state == .upToDate || state == .failed("Güncellemeler kontrol edilemedi. İnternet bağlantını kontrol et.") {
                state = .idle
            }
        }
    }

    /// Returns the latest released version, or nil if GitHub could not be reached.
    @discardableResult
    func check() async -> String? {
        guard state != .installing else { return nil }
        struct Release: Decodable {
            struct Asset: Decodable {
                let name: String
                let browser_download_url: URL
            }
            let tag_name: String
            let assets: [Asset]
        }
        var request = URLRequest(url: Self.latestURL)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        guard let (data, _) = try? await URLSession.shared.data(for: request),
              let release = try? JSONDecoder().decode(Release.self, from: data) else { return nil }
        let latest = release.tag_name.trimmingCharacters(in: CharacterSet(charactersIn: "v"))
        guard Self.isNewer(latest, than: currentVersion),
              let dmg = release.assets.first(where: { $0.name.hasSuffix("-macOS.dmg") }) else { return latest }
        dmgURL = dmg.browser_download_url
        state = .available(version: latest)
        return latest
    }

    func install() {
        guard case .available = state, let dmgURL else { return }
        state = .installing
        Task {
            do {
                let app = try await Self.download(dmgURL)
                try Self.relaunch(replacing: Bundle.main.bundleURL, with: app)
                NSApp.terminate(nil)
            } catch {
                state = .failed(error.localizedDescription)
            }
        }
    }

    // MARK: - Steps

    struct Failure: LocalizedError {
        let errorDescription: String?
        init(_ message: String) { errorDescription = "Güncelleme kurulamadı: \(message)" }
    }

    /// Downloads the DMG and copies the verified app out of it into a temp folder.
    nonisolated private static func download(_ url: URL) async throws -> URL {
        let (file, response) = try await URLSession.shared.download(from: url)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw Failure("indirme başarısız") }

        let work = FileManager.default.temporaryDirectory.appendingPathComponent("kite-update-\(UUID().uuidString)")
        let mount = work.appendingPathComponent("mnt")
        try FileManager.default.createDirectory(at: mount, withIntermediateDirectories: true)
        let dmg = work.appendingPathComponent("update.dmg")
        try FileManager.default.moveItem(at: file, to: dmg)

        try run("/usr/bin/hdiutil", ["attach", dmg.path, "-nobrowse", "-readonly", "-mountpoint", mount.path])
        defer { _ = try? run("/usr/bin/hdiutil", ["detach", mount.path, "-force"]) }

        guard let name = try FileManager.default.contentsOfDirectory(atPath: mount.path)
            .first(where: { $0.hasSuffix(".app") }) else { throw Failure("DMG içinde uygulama yok") }
        let app = work.appendingPathComponent(name)
        try run("/usr/bin/ditto", [mount.appendingPathComponent(name).path, app.path])

        // only ever install a build signed by the same Developer ID team
        try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
        let req = "anchor apple generic and certificate leaf[subject.OU] = \"\(teamID)\""
        try run("/usr/bin/codesign", ["--verify", "-R=\(req)", app.path])
        return app
    }

    /// Starts a detached script that waits for this process to exit, swaps
    /// the bundle and opens the new version.
    nonisolated private static func relaunch(replacing current: URL, with new: URL) throws {
        let pid = ProcessInfo.processInfo.processIdentifier
        let script = """
        while kill -0 \(pid) 2>/dev/null; do sleep 0.2; done
        rm -rf "$1" && /usr/bin/ditto "$2" "$1" && /usr/bin/xattr -dr com.apple.quarantine "$1"
        /usr/bin/open "$1"
        rm -rf "$3"
        """
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", script, "kite-update", current.path, new.path, new.deletingLastPathComponent().path]
        try p.run()
    }

    @discardableResult
    nonisolated private static func run(_ tool: String, _ args: [String]) throws -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        try p.run()
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        p.waitUntilExit()
        guard p.terminationStatus == 0 else {
            throw Failure("\(URL(fileURLWithPath: tool).lastPathComponent) hata verdi")
        }
        return out
    }

    nonisolated static func isNewer(_ a: String, than b: String) -> Bool {
        let x = a.split(separator: ".").map { Int($0) ?? 0 }
        let y = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let l = i < x.count ? x[i] : 0, r = i < y.count ? y[i] : 0
            if l != r { return l > r }
        }
        return false
    }
}

/// "Güncelleme var" banner shared by the menu bar panel and the window.
struct UpdateBanner: View {
    @ObservedObject var updater = Updater.shared

    var body: some View {
        switch updater.state {
        case .idle:
            EmptyView()
        case .checking:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Güncellemeler kontrol ediliyor…").font(.callout)
                Spacer()
            }
            .padding(10)
            .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))
        case .upToDate:
            HStack(spacing: 8) {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                Text("Kite DPI güncel · sürüm \(updater.currentVersion)").font(.callout)
                Spacer()
            }
            .padding(10)
            .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))
        case .available(let version):
            HStack {
                Image(systemName: "arrow.down.circle.fill").foregroundStyle(.blue)
                VStack(alignment: .leading, spacing: 1) {
                    Text("Güncelleme var").font(.callout.weight(.semibold))
                    Text("Sürüm \(version)").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Güncelle") { updater.install() }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
            }
            .padding(10)
            .background(Color.blue.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
        case .installing:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Güncelleme indiriliyor…").font(.callout)
                Spacer()
            }
            .padding(10)
            .background(Color.blue.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
        case .failed(let message):
            Text(message).font(.caption).foregroundStyle(.red)
        }
    }
}
