import Foundation
import NetworkExtension

/// Installs, starts and monitors the Kite VPN configuration.
@MainActor
final class TunnelController: ObservableObject {
    @Published private(set) var status: NEVPNStatus = .invalid
    @Published private(set) var stats = TunnelStats()
    @Published private(set) var alwaysOn = false
    @Published var errorMessage: String?

    private var manager: NETunnelProviderManager?
    private var statusObserver: NSObjectProtocol?
    private var timer: Timer?

    var isOn: Bool { status == .connected }
    var isBusy: Bool { [.connecting, .disconnecting, .reasserting].contains(status) }

    func load() async {
        do {
            let managers = try await NETunnelProviderManager.loadAllFromPreferences()
            manager = managers.first
        } catch {
            errorMessage = error.localizedDescription
        }
        alwaysOn = manager?.isOnDemandEnabled ?? false
        observe()
        updateStatus()
    }

    func toggle() async {
        if isOn || status == .connecting {
            // on-demand would immediately reconnect, so switch it off first
            if alwaysOn { await setAlwaysOn(false) }
            manager?.connection.stopVPNTunnel()
        } else {
            await connect()
        }
    }

    func setAlwaysOn(_ on: Bool) async {
        do {
            let m = try await ensureManager()
            let rule = NEOnDemandRuleConnect()
            rule.interfaceTypeMatch = .any
            m.onDemandRules = [rule]
            m.isOnDemandEnabled = on
            try await m.saveToPreferences()
            try await m.loadFromPreferences()
            alwaysOn = on
            if on && !isOn { await connect() }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func connect() async {
        do {
            let m = try await ensureManager()
            m.isEnabled = true
            try await m.saveToPreferences()
            try await m.loadFromPreferences()
            try m.connection.startVPNTunnel()
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    /// The first call asks iOS for permission to add the VPN configuration.
    private func ensureManager() async throws -> NETunnelProviderManager {
        if let manager { return manager }
        let m = NETunnelProviderManager()
        let proto = NETunnelProviderProtocol()
        proto.providerBundleIdentifier = TunnelShared.providerBundleID
        proto.serverAddress = tr("Cihaz içi (sunucu yok)", "On device (no server)")
        m.protocolConfiguration = proto
        m.localizedDescription = "Kite DPI"
        m.isEnabled = true
        try await m.saveToPreferences()
        try await m.loadFromPreferences()
        manager = m
        observe()
        return m
    }

    private func observe() {
        guard statusObserver == nil, let connection = manager?.connection else { return }
        statusObserver = NotificationCenter.default.addObserver(
            forName: .NEVPNStatusDidChange, object: connection, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.updateStatus() }
        }
    }

    private func updateStatus() {
        status = manager?.connection.status ?? .disconnected
        if status == .connected {
            if timer == nil {
                timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                    MainActor.assumeIsolated { self?.pollStats() }
                }
                pollStats()
            }
        } else {
            timer?.invalidate()
            timer = nil
        }
    }

    private func pollStats() {
        guard let session = manager?.connection as? NETunnelProviderSession else { return }
        try? session.sendProviderMessage(TunnelShared.statsMessage) { [weak self] data in
            guard let data, let s = try? JSONDecoder().decode(TunnelStats.self, from: data) else { return }
            Task { @MainActor in self?.stats = s }
        }
    }
}
