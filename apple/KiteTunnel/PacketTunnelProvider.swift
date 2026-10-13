import NetworkExtension

/// Runs kite-core inside the VPN extension and points the device's HTTP/HTTPS
/// proxy at it. No traffic leaves the phone through a remote server: the
/// tunnel only exists so iOS lets the engine run in the background and
/// applies the proxy settings system-wide.
final class PacketTunnelProvider: NEPacketTunnelProvider {
    override func startTunnel(options: [String: NSObject]?, completionHandler: @escaping (Error?) -> Void) {
        var cfg = kite_config()
        kite_config_default(&cfg)
        cfg.listen_port = TunnelShared.proxyPort
        cfg.port_fallback = 1
        if kite_start(&cfg) != 0 {
            completionHandler(NSError(domain: "Kite", code: 1, userInfo: [
                NSLocalizedDescriptionKey: tr("Kite DPI motoru başlatılamadı.", "Could not start the Kite DPI engine."),
            ]))
            return
        }

        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: "127.0.0.1")
        // A tiny unused route: the tunnel must exist, but no app traffic is
        // routed into it. Everything reaches Kite through the proxy instead.
        let ipv4 = NEIPv4Settings(addresses: ["198.18.0.1"], subnetMasks: ["255.255.255.255"])
        ipv4.includedRoutes = [NEIPv4Route(destinationAddress: "198.18.0.0", subnetMask: "255.255.255.252")]
        settings.ipv4Settings = ipv4
        settings.mtu = 1500

        let proxy = NEProxySettings()
        let server = NEProxyServer(address: "127.0.0.1", port: Int(kite_listen_port()))
        proxy.httpEnabled = true
        proxy.httpServer = server
        proxy.httpsEnabled = true
        proxy.httpsServer = server
        proxy.excludeSimpleHostnames = true
        proxy.exceptionList = ["localhost", "127.0.0.1", "*.local", "captive.apple.com"]
        // "" matches every domain, so the proxy applies even though the
        // tunnel is not the default route.
        proxy.matchDomains = [""]
        settings.proxySettings = proxy

        setTunnelNetworkSettings(settings) { error in
            if error != nil { kite_stop() }
            completionHandler(error)
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        kite_stop()
        completionHandler()
    }

    override func handleAppMessage(_ messageData: Data, completionHandler: ((Data?) -> Void)?) {
        guard messageData == TunnelShared.statsMessage else {
            completionHandler?(nil)
            return
        }
        var s = kite_stats()
        kite_get_stats(&s)
        let stats = TunnelStats(connections: s.connections, desynced: s.desynced,
                                bytesUp: s.bytes_up, bytesDown: s.bytes_down, errors: s.errors)
        completionHandler?(try? JSONEncoder().encode(stats))
    }
}
