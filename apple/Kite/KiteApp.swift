import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        MainActor.assumeIsolated {
            Engine.shared.recoverFromCrash()
            Engine.shared.start()
        }
        // after the window is up, so the permission prompt has context
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            MainActor.assumeIsolated {
                Engine.shared.ensureAppSupport()
                Updater.shared.startChecking()
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        MainActor.assumeIsolated { Engine.shared.stop() }
    }
}

@main
struct KiteApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var engine = Engine.shared

    var body: some Scene {
        Window("Kite DPI", id: "main") {
            MainWindow()
                .environmentObject(engine)
                // show in the Dock only while the window is open
                .onAppear { NSApp.setActivationPolicy(.regular) }
                .onDisappear { NSApp.setActivationPolicy(.accessory) }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)

        MenuBarExtra {
            MenuView()
                .environmentObject(engine)
        } label: {
            Image(systemName: engine.running ? "bolt.shield.fill" : "bolt.shield")
        }
        .menuBarExtraStyle(.window)
    }
}

struct MenuView: View {
    @EnvironmentObject var engine: Engine
    @Environment(\.openWindow) private var openWindow

    private let green = Color(red: 0.13, green: 0.77, blue: 0.37)

    var body: some View {
        VStack(spacing: 12) {
            header
            statusCard

            if let err = engine.lastError {
                Text(err)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            UpdateBanner()

            HStack {
                Label("Mac açılınca başlat", systemImage: "power.circle")
                Spacer()
                Toggle("", isOn: $engine.launchAtLogin)
                    .toggleStyle(.switch)
                    .controlSize(.small)
                    .tint(green)
                    .labelsHidden()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 9)
            .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))

            HStack(spacing: 8) {
                footerButton("Pencere", "macwindow") {
                    openWindow(id: "main")
                    NSApp.activate()
                }
                footerButton("Güncellemeler", "arrow.triangle.2.circlepath") {
                    Updater.shared.checkNow()
                }
                footerButton("Çıkış", "xmark.circle") {
                    NSApp.terminate(nil)
                }
            }

            Text(Updater.credit)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(14)
        .frame(width: 300)
    }

    private var header: some View {
        HStack(spacing: 10) {
            ZStack {
                RoundedRectangle(cornerRadius: 9)
                    .fill(engine.running
                          ? LinearGradient(colors: [green.opacity(0.9), green], startPoint: .top, endPoint: .bottom)
                          : LinearGradient(colors: [.gray.opacity(0.5), .gray.opacity(0.35)], startPoint: .top, endPoint: .bottom))
                    .frame(width: 36, height: 36)
                Image(systemName: "bolt.shield.fill")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(.white)
            }
            VStack(alignment: .leading, spacing: 3) {
                Text("Kite DPI").font(.headline)
                HStack(spacing: 5) {
                    Circle()
                        .fill(engine.running ? green : Color.secondary)
                        .frame(width: 7, height: 7)
                    Text(engine.running ? "Açık" : "Kapalı")
                        .font(.caption)
                        .foregroundStyle(engine.running ? green : .secondary)
                }
            }
            Spacer()
            // set the state rather than flip it: SwiftUI may write the
            // current value back when the panel opens
            Toggle("", isOn: Binding(get: { engine.running }, set: { $0 ? engine.start() : engine.stop() }))
                .toggleStyle(.switch)
                .tint(green)
                .labelsHidden()
        }
    }

    private var statusCard: some View {
        HStack(spacing: 0) {
            if engine.running {
                stat("Korunan bağlantı", "\(engine.stats.desynced)")
                Divider().frame(height: 28).overlay(Color.white.opacity(0.1))
                stat("İndirilen", ByteCountFormatter.string(fromByteCount: Int64(engine.stats.bytes_down), countStyle: .binary))
            } else {
                VStack(spacing: 3) {
                    Text("Engel aşma kapalı").font(.callout.weight(.semibold))
                    Text("Açmak için sağ üstteki anahtarı kullan.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(.vertical, 11)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(engine.running ? green.opacity(0.10) : Color.white.opacity(0.06))
        )
        .animation(.easeInOut(duration: 0.25), value: engine.running)
    }

    private func stat(_ label: String, _ value: String) -> some View {
        VStack(spacing: 2) {
            Text(value)
                .font(.title3.weight(.semibold).monospacedDigit())
                .contentTransition(.numericText())
            Text(label).font(.caption2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
    }

    private func footerButton(_ title: String, _ icon: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 4) {
                Image(systemName: icon).font(.system(size: 14))
                Text(title).font(.caption2)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 8)
            .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 9))
            .contentShape(RoundedRectangle(cornerRadius: 9))
        }
        .buttonStyle(.plain)
    }
}
