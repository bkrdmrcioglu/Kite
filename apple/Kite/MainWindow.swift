import SwiftUI

/// The main window: same look as the iPhone app, adapted for the Mac.
struct MainWindow: View {
    @EnvironmentObject var engine: Engine

    private let green = Color(red: 0.13, green: 0.77, blue: 0.37)

    var body: some View {
        ZStack {
            background
            VStack(spacing: 0) {
                header
                Spacer(minLength: 12)
                powerButton
                statusText
                    .padding(.top, 22)
                Spacer(minLength: 20)
                statsCard
                settingsCard
                    .padding(.top, 10)
                if let err = engine.lastError {
                    Text(err)
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .multilineTextAlignment(.center)
                        .padding(.top, 10)
                }
            }
            .padding(.horizontal, 22)
            .padding(.top, 34) // clears the hidden title bar's traffic lights
            .padding(.bottom, 20)
        }
        .frame(width: 380, height: 600)
        .preferredColorScheme(.dark)
    }

    private var background: some View {
        ZStack {
            Color(red: 0.05, green: 0.06, blue: 0.08)
            RadialGradient(
                colors: [(engine.running ? green : Color.gray).opacity(0.28), .clear],
                center: .init(x: 0.5, y: 0.38), startRadius: 10, endRadius: 320
            )
            .animation(.easeInOut(duration: 0.6), value: engine.running)
        }
        .ignoresSafeArea()
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "bolt.shield.fill")
                .font(.title2)
                .foregroundStyle(engine.running ? green : .secondary)
            Text("Kite").font(.title2.weight(.bold))
            Spacer()
            Text("v\(String(cString: kite_version()))")
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
    }

    private var powerButton: some View {
        Button {
            engine.toggle()
        } label: {
            ZStack {
                Circle()
                    .fill(engine.running ? green.opacity(0.18) : Color.white.opacity(0.06))
                    .frame(width: 200, height: 200)
                Circle()
                    .stroke(engine.running ? green : Color.white.opacity(0.15), lineWidth: 3)
                    .frame(width: 164, height: 164)
                    .shadow(color: engine.running ? green.opacity(0.8) : .clear, radius: 22)
                Image(systemName: "power")
                    .font(.system(size: 58, weight: .semibold))
                    .foregroundStyle(engine.running ? green : .white.opacity(0.85))
            }
            .contentShape(Circle())
        }
        .buttonStyle(PressableStyle())
        .keyboardShortcut(.space, modifiers: [])
        .animation(.spring(duration: 0.4), value: engine.running)
        .help(engine.running ? "Kite'ı kapat" : "Kite'ı aç")
    }

    private var statusText: some View {
        VStack(spacing: 6) {
            Text(engine.running ? "Engeller aşılıyor" : "Kapalı")
                .font(.title3.weight(.semibold))
            Text(engine.running
                 ? "Discord ve engelli siteler açılıyor.\nTrafiğin bilgisayarından çıkmıyor."
                 : "Açmak için tıkla ya da boşluk tuşuna bas.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
    }

    private var statsCard: some View {
        HStack(spacing: 0) {
            stat("Korunan bağlantı", "\(engine.stats.desynced)")
            Divider().frame(height: 34).overlay(Color.white.opacity(0.1))
            stat("İndirilen veri", ByteCountFormatter.string(fromByteCount: Int64(engine.stats.bytes_down), countStyle: .binary))
        }
        .padding(.vertical, 14)
        .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 14))
        .opacity(engine.running ? 1 : 0.5)
    }

    private func stat(_ label: String, _ value: String) -> some View {
        VStack(spacing: 4) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Text(value)
                .font(.title3.weight(.semibold).monospacedDigit())
                .contentTransition(.numericText())
        }
        .frame(maxWidth: .infinity)
    }

    private var settingsCard: some View {
        VStack(spacing: 0) {
            settingRow(
                "Mac açılınca başlat",
                "Oturum açınca Kite arka planda çalışır.",
                isOn: $engine.launchAtLogin
            )
            Divider().overlay(Color.white.opacity(0.08)).padding(.leading, 14)
            settingRow(
                "Discord uygulaması",
                "Masaüstü uygulaması için gerekli. Bir kez şifre ister.",
                isOn: Binding(get: { engine.discordApp }, set: { engine.setDiscordApp($0) })
            )
        }
        .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 14))
    }

    private func settingRow(_ title: String, _ caption: String, isOn: Binding<Bool>) -> some View {
        Toggle(isOn: isOn) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                Text(caption)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .toggleStyle(.switch)
        .tint(green)
        .padding(.horizontal, 14)
        .padding(.vertical, 11)
    }
}

/// Slight shrink on press, like a physical button.
private struct PressableStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(.easeOut(duration: 0.15), value: configuration.isPressed)
    }
}
