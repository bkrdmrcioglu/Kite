<div align="center">
  <img src="windows/icons/kite-256.png" alt="Kite DPI" width="128"/>

  # Kite DPI

  **DPI bypass that runs entirely on your device. No VPN server, no account, no data collection.**

  macOS · iOS · Windows

  Bekir Demircioğlu tarafından sevgiyle geliştirildi · Made with care by Bekir Demircioğlu

  [Türkçe](#türkçe) · [English](#english)
</div>

---

## Türkçe

Kite DPI, operatörlerin **DPI (Derin Paket İnceleme)** ile yaptığı site engellemelerini aşar. Trafiğin hiçbir sunucuya gitmez; her şey kendi cihazında olur. Engellenen sitenin adı (TLS SNI) bağlantının ilk paketinde görünür. Kite DPI bu paketi DPI'ın okuyamayacağı şekilde yeniden düzenler, gerçek sunucu ise onu normal şekilde okur.

### İndir

| Platform | Dosya |
|---|---|
| **macOS** 14+ (Apple Silicon ve Intel) | [Kite-DPI-macOS.dmg](https://github.com/bkrdmrcioglu/Kite/releases/latest) ya da `brew install --cask bkrdmrcioglu/tap/kite-dpi` |
| **Windows** 10/11 (x64) | [Kite-DPI-Windows-x64.exe](https://github.com/bkrdmrcioglu/Kite/releases/latest) |
| **iOS** 17+ | Yakında (TestFlight) |

Windows sürümü henüz imzalı değil. SmartScreen uyarısı çıkarsa **Ek bilgi → Yine de çalıştır**'a tıkla.

### Nasıl çalışır?

1. **Şifreli DNS:** Site adreslerini operatörün zehirli DNS'ini atlayarak TCP üzerinden 1.1.1.1, 8.8.8.8 ve 9.9.9.9'a sorar. Bilinen engel sayfası IP'lerini atar.
2. **TLS kayıt bölme:** TLS ClientHello'yu site adının ortasından iki ayrı TLS kaydına böler ve ayrı TCP parçaları olarak gönderir. DPI site adını göremez, sunucu kayıtları sorunsuz birleştirir.
3. **Yerel proxy:** Sistemin proxy ayarını kendine yönlendirir. Kapanınca, çökünce ya da bilgisayar kapanınca eski ayarı geri yükler.
4. **Uygulama desteği:** Proxy ayarına uymayan uygulamalar için ilk açılışta bir kez yönetici izni ister ve bu uygulamaların adreslerini hosts dosyasıyla Kite DPI'a yönlendirir. Kite DPI bu bağlantıları 443 portunda karşılar. `brew uninstall --zap --cask kite-dpi` ile kaldırırken bu satırlar da silinir.
5. **Port:** Varsayılan port 8899. Doluysa Kite DPI yakındaki boş bir porta geçer ve panelde hangi portu kullandığını gösterir. Elle değiştirmek için aşağıdaki **Port** bölümüne bak.
6. **Güncellemeler:** Açılışta ve 6 saatte bir bu deponun GitHub sürümlerine bakar. Yeni sürüm varsa panelde **Güncelle** düğmesi çıkar; tek tıkla indirip kurar ve kendini yeniden başlatır.

| Platform | Yöntem |
|---|---|
| macOS | Menü çubuğu ve pencere uygulaması, sistem proxy'si (`networksetup`) |
| iOS | Cihaz içi VPN eklentisi (NEPacketTunnelProvider). Eklenti motoru çalıştırır ve sistem proxy'sini ona yönlendirir; trafik telefondan bir sunucuya çıkmaz |
| Windows | Sistem tepsisi uygulaması, kullanıcı proxy'si (WinINet). Yönetici izni gerekmez |

### Sınırlar

- **UDP trafiği:** Sesli aramalar gibi UDP trafiği Kite DPI'dan geçmez. Operatör bu trafiği engellemiyorsa zaten çalışır.
- **IP engeli:** IP adresi üzerinden engellenen siteler açılmaz. Kite DPI yalnızca DNS ve SNI tabanlı engellemeyi aşar.
- **Operatör farkı:** Farklı operatörler farklı DPI kullanır. `tools/discord_test.py` hangi yöntemin senin hattında çalıştığını gösterir.

### Yasal not

Kite DPI'ı bulunduğun ülkenin yasalarına uygun şekilde kullanmak senin sorumluluğundadır.

---

## English

Kite DPI defeats SNI-based **Deep Packet Inspection** censorship without a VPN server. It runs a local proxy that resolves names over TCP DNS (bypassing poisoned ISP resolvers) and splits the TLS ClientHello into two TLS records right inside the hostname, so the DPI never sees the name in one piece while the real server reassembles it normally.

### Download

| Platform | |
|---|---|
| **macOS** 14+ (universal) | [Releases](https://github.com/bkrdmrcioglu/Kite/releases/latest) or `brew install --cask bkrdmrcioglu/tap/kite-dpi` |
| **Windows** 10/11 x64 | [Releases](https://github.com/bkrdmrcioglu/Kite/releases/latest) |
| **iOS** 17+ | Coming soon (TestFlight) |

### Language

The apps follow your system language: Turkish on Turkish systems, English everywhere else.

### Port

The local proxy listens on `127.0.0.1:8899` by default. If that port is taken, Kite DPI picks the next free one and shows it in the panel ("On · port 8900"). To choose a port yourself:

| Platform | Command |
|---|---|
| Windows | Run once: `"Kite DPI.exe" --port 9000` (remembered for later starts) |
| macOS | `defaults write com.bkrdmrcioglu.kite port -int 9000`, then restart Kite DPI |

Ports 1024–65535 are accepted. The system proxy is pointed at whichever port is actually in use.

### Strategies

| Strategy | What it does |
|---|---|
| `tlsrec` (default) | Splits the ClientHello into two TLS records at the SNI and sends them as separate TCP segments |
| `bytes` | Sends the hello up to the end of the SNI one byte per segment |
| `split` | Plain TCP split at the SNI |

On the network Kite DPI was developed against (Turk Telekom), plain TCP split and disorder fail because the DPI reassembles TCP, while `tlsrec` and `bytes` work.

### Project layout

```
core/       portable C engine (POSIX + Winsock), shared by every platform
cli/        command-line proxy for testing: kite -v [-p port] [-t sni_port] [-s strategy]
apple/      Xcode project (generated by xcodegen from project.yml)
  Kite/       macOS menu bar + window app
  KiteiOS/    iOS app
  KiteTunnel/ iOS packet tunnel extension running the engine
windows/    Win32 tray app with a Windows 11 style flyout
tools/      strategy tester, icon generator
scripts/    release builds (notarized DMG, Windows exe)
```

### Build

```bash
make && make test                 # engine + CLI + unit tests
./build/kite -v                   # proxy on 127.0.0.1:8899 (HTTP CONNECT + SOCKS5)

brew install xcodegen
cd apple && xcodegen generate     # then open Kite.xcodeproj

brew install mingw-w64
make windows                      # build/win/Kite.exe
```

To build the Apple targets under your own account, change `DEVELOPMENT_TEAM` and the bundle identifiers in `apple/project.yml`.

### Test your network

```bash
python3 tools/discord_test.py discord.com
```

The script opens one TLS connection per strategy and shows which ones get through. It does not change any settings.

### Privacy

Kite DPI has no servers, analytics or telemetry. The only connections it makes on its own are DNS-over-TCP queries to public resolvers (1.1.1.1, 1.0.0.1, 8.8.8.8, 9.9.9.9) and an update check against this repository's GitHub releases at launch and every 6 hours.

### License

[MIT](LICENSE)
