import Foundation

enum TunnelShared {
    static let providerBundleID = "com.bkrdmrcioglu.kite.ios.tunnel"
    static let proxyPort: UInt16 = 8899
    static let statsMessage = Data("stats".utf8)
}

/// Engine counters sent from the tunnel extension to the app.
struct TunnelStats: Codable, Equatable {
    var connections: UInt64 = 0
    var desynced: UInt64 = 0
    var bytesUp: UInt64 = 0
    var bytesDown: UInt64 = 0
    var errors: UInt64 = 0
}
