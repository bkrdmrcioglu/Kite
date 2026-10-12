import AppKit
import ServiceManagement

/// Wraps the C engine: runs the proxy and points the system at it.
@MainActor
final class Engine: ObservableObject {
    static let shared = Engine()
    nonisolated static let port: UInt16 = 8899

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
                lastError = "Açılışta başlatma ayarlanamadı: \(error.localizedDescription)"
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
        alert.messageText = "Kite DPI için bir kez izin gerekiyor"
        alert.informativeText = "Kite DPI'ın tarayıcı dışındaki uygulamalarda da çalışabilmesi için bir kez yönetici izni gerekiyor. İzin verdikten sonra tekrar sorulmaz."
        alert.addButton(withTitle: "Devam")
        alert.addButton(withTitle: "Şimdi değil")
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
        cfg.listen_port = Self.port
        cfg.sni_port = 443
        if kite_start(&cfg) != 0 {
            lastError = "Kite DPI başlatılamadı (port \(Self.port) kullanımda)."
            return
        }
        do {
            try SystemProxy.apply(port: Self.port)
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
