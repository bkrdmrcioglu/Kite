import NetworkExtension
import SwiftUI

@main
struct KiteiOSApp: App {
    @StateObject private var tunnel = TunnelController()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(tunnel)
                .task { await tunnel.load() }
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var tunnel: TunnelController
    @Environment(\.scenePhase) private var scenePhase

    private let green = Color(red: 0.13, green: 0.77, blue: 0.37)

    var body: some View {
        ZStack {
            background
            VStack(spacing: 0) {
                header
                Spacer()
                powerButton
                statusText
                    .padding(.top, 28)
                Spacer()
                statsCard
                settingsCard
                    .padding(.top, 12)
                if let err = tunnel.errorMessage {
                    Text(err)
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .multilineTextAlignment(.center)
                        .padding(.top, 10)
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 12)
        }
        .preferredColorScheme(.dark)
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { Task { await tunnel.load() } }
        }
    }

    private var background: some View {
        ZStack {
            Color(red: 0.05, green: 0.06, blue: 0.08).ignoresSafeArea()
            RadialGradient(
                colors: [(tunnel.isOn ? green : Color.gray).opacity(0.28), .clear],
                center: .center, startRadius: 10, endRadius: 360
            )
            .ignoresSafeArea()
            .animation(.easeInOut(duration: 0.6), value: tunnel.isOn)
        }
    }

    private var header: some View {
        HStack {
            Image(systemName: "bolt.shield.fill")
                .font(.title2)
                .foregroundStyle(tunnel.isOn ? green : .secondary)
            Text("Kite DPI").font(.title2.weight(.bold))
            Spacer()
        }
        .padding(.top, 8)
    }

    private var powerButton: some View {
        Button {
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            Task { await tunnel.toggle() }
        } label: {
            ZStack {
                Circle()
                    .fill(tunnel.isOn ? green.opacity(0.18) : Color.white.opacity(0.06))
                    .frame(width: 220, height: 220)
                Circle()
                    .stroke(tunnel.isOn ? green : Color.white.opacity(0.15), lineWidth: 3)
                    .frame(width: 180, height: 180)
                    .shadow(color: tunnel.isOn ? green.opacity(0.8) : .clear, radius: 24)
                if tunnel.isBusy {
                    ProgressView()
                        .controlSize(.large)
                        .tint(.white)
                } else {
                    Image(systemName: "power")
                        .font(.system(size: 64, weight: .semibold))
                        .foregroundStyle(tunnel.isOn ? green : .white.opacity(0.85))
                }
            }
            .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .animation(.spring(duration: 0.4), value: tunnel.status)
        .accessibilityLabel(tunnel.isOn ? "Kite DPI'ı kapat" : "Kite DPI'ı aç")
    }

    private var statusText: some View {
        VStack(spacing: 6) {
            Text(title).font(.title3.weight(.semibold))
            Text(subtitle)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
    }

    private var title: String {
        switch tunnel.status {
        case .connected: "Engeller aşılıyor"
        case .connecting, .reasserting: "Bağlanıyor…"
        case .disconnecting: "Kapatılıyor…"
        default: "Kapalı"
        }
    }

    private var subtitle: String {
        tunnel.isOn
            ? "Discord ve engelli siteler açılıyor.\nTrafiğin telefonundan çıkmıyor."
            : "Açmak için dokun."
    }

    private var statsCard: some View {
        HStack(spacing: 0) {
            stat("Korunan bağlantı", "\(tunnel.stats.desynced)")
            Divider().frame(height: 36).overlay(Color.white.opacity(0.1))
            stat("İndirilen veri", ByteCountFormatter.string(fromByteCount: Int64(tunnel.stats.bytesDown), countStyle: .binary))
        }
        .padding(.vertical, 14)
        .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 16))
        .opacity(tunnel.isOn ? 1 : 0.5)
    }

    private func stat(_ label: String, _ value: String) -> some View {
        VStack(spacing: 4) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.title3.weight(.semibold).monospacedDigit())
                .contentTransition(.numericText())
        }
        .frame(maxWidth: .infinity)
    }

    private var settingsCard: some View {
        Toggle(isOn: Binding(get: { tunnel.alwaysOn }, set: { on in Task { await tunnel.setAlwaysOn(on) } })) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Her zaman açık")
                Text("Telefon yeniden başlasa da Kite DPI kendiliğinden açılır.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .tint(green)
        .padding(14)
        .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 16))
    }
}
