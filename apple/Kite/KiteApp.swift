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

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                Image(systemName: engine.running ? "bolt.shield.fill" : "bolt.shield")
                    .font(.system(size: 28))
                    .foregroundStyle(engine.running ? .green : .secondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Kite DPI").font(.headline)
                    Text(engine.running ? "Açık · \(engine.stats.desynced) bağlantı korundu" : "Kapalı")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Toggle("", isOn: Binding(get: { engine.running }, set: { _ in engine.toggle() }))
                    .toggleStyle(.switch)
                    .labelsHidden()
            }

            if let err = engine.lastError {
                Text(err).font(.caption).foregroundStyle(.red)
            }

            UpdateBanner()

            Divider()

            Toggle("Mac açılınca başlat", isOn: $engine.launchAtLogin)
                .toggleStyle(.checkbox)

            Button("Güncellemeleri kontrol et") { Updater.shared.checkNow() }
                .buttonStyle(.link)
                .font(.caption)

            HStack {
                Spacer()
                Button("Pencereyi aç") {
                    openWindow(id: "main")
                    NSApp.activate()
                }
                .controlSize(.small)
                Button("Çıkış") { NSApp.terminate(nil) }
                    .controlSize(.small)
            }

            Text(Updater.credit)
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .frame(maxWidth: .infinity, alignment: .center)
        }
        .padding(16)
        .frame(width: 280)
    }
}
