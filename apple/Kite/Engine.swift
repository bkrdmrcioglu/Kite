import AppKit
import ServiceManagement

/// Wraps the C engine: runs the proxy and points the system at it.
@MainActor
final class Engine: ObservableObject {
    static let shared = Engine()
    /// Preferred proxy port; change with
    /// `defaults write com.bkrdmrcioglu.kite port -int 9000`.
    static var preferredPort: UInt16 {
        let p = UserDefaults.standard.integer(forKey: "port")
        return (1024...65535).contains(p) ? UInt16(p) : 8899
    }

    /// Port actually in use (another free one if the preferred port is busy).
    @Published private(set) var port: UInt16 = 0

    @Published private(set) var running = false
    @Published private(set) var stats = kite_stats()
    @Published var lastError: String?

    @Published var launchAtLogin = SMAppService.mainApp.status == .enabled {
        didSet {
            guard launchAtLogin != oldValue else { return }
            do {
                if launchAtLogin { try SMAppService.mainApp.register() }
                else { try SMAppService.mainApp.unregister() }
            } catch {
                lastError = tr("Açılışta başlatma ayarlanamadı: ", "Could not set launch at login: ") + error.localizedDescription
            }
        }
    }

    /// Some desktop apps ignore the system proxy; hosts-file entries send
    /// them to Kite's TLS listener instead. Asked for once at launch rather
    /// than offered as a setting, and asked again next launch if declined.
    func ensureAppSupport() {
        guard !HostsFile.isInstalled else { return }
        NSApp.activate()
        let alert = NSAlert()
        alert.messageText = tr("Kite DPI için bir kez izin gerekiyor", "Kite DPI needs permission once")
        alert.informativeText = tr("Kite DPI'ın tarayıcı dışındaki uygulamalarda da çalışabilmesi için bir kez yönetici izni gerekiyor. İzin verdikten sonra tekrar sorulmaz.",
                                   "To work in apps other than your browser, Kite DPI needs administrator permission once. You won't be asked again.")
        alert.addButton(withTitle: tr("Devam", "Continue"))
        alert.addButton(withTitle: tr("Şimdi değil", "Not now"))
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        do { try HostsFile.install() } catch { lastError = error.localizedDescription }
    }

    private var timer: Timer?

    /// Repairs proxy settings left behind by a crash or forced quit.
    func recoverFromCrash() {
        if SystemProxy.isApplied { restoreProxy() }
    }

    private func restoreProxy() {
        do { try SystemProxy.restore() } catch { lastError = error.localizedDescription }
    }

    func start() {
        guard !running else { return }
        var cfg = kite_config()
        kite_config_default(&cfg)
        cfg.listen_port = Self.preferredPort
        cfg.port_fallback = 1
        cfg.sni_port = 443
        if kite_start(&cfg) != 0 {
            lastError = tr("Kite DPI başlatılamadı.", "Could not start Kite DPI.")
            return
        }
        port = kite_listen_port()
        do {
            try SystemProxy.apply(port: port)
        } catch {
            kite_stop()
            lastError = error.localizedDescription
            return
        }
        lastError = nil
        running = true
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
            Task { @MainActor in Engine.shared.refresh() }
        }
    }

    func stop() {
        // restore first so the system never points at a dead proxy
        if SystemProxy.isApplied { restoreProxy() }
        kite_stop()
        timer?.invalidate()
        timer = nil
        running = false
    }

    func toggle() { running ? stop() : start() }

    private func refresh() {
        var s = kite_stats()
        kite_get_stats(&s)
        stats = s
    }
}
