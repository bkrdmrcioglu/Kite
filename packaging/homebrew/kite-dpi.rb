# Homebrew cask for the bkrdmrcioglu/homebrew-tap repository.
#   brew install --cask bkrdmrcioglu/tap/kite-dpi
cask "kite-dpi" do
  version "0.1.7"
  sha256 "6e67e5ec487523a8d653e4ffa75a9ecc7886c7012b938188c93b698d169e6c8f"

  url "https://github.com/bkrdmrcioglu/Kite/releases/download/v#{version}/Kite-DPI-#{version}-macOS.dmg"
  name "Kite DPI"
  desc "DPI bypass that runs entirely on your Mac, no VPN server"
  homepage "https://github.com/bkrdmrcioglu/Kite"

  auto_updates true
  depends_on macos: :sonoma

  app "Kite DPI.app"

  # quitting restores the system proxy settings
  uninstall quit: "com.bkrdmrcioglu.kite"

  # `brew uninstall --zap` also removes the /etc/hosts entries Kite DPI adds
  # for apps that ignore the proxy. Kept out of `uninstall`, which also runs
  # on every upgrade and would ask for a password each time.
  zap script: {
        executable: "/usr/bin/sed",
        args:       [
          "-i", "",
          "-e", "/^# >>> Kite DPI >>>$/,/^# <<< Kite DPI <<<$/d",
          "-e", "/^# >>> Kite (Discord) >>>$/,/^# <<< Kite (Discord) <<<$/d",
          "/etc/hosts"
        ],
        sudo:       true,
      },
      trash:  [
        "~/Library/Application Support/Kite",
        "~/Library/Preferences/com.bkrdmrcioglu.kite.plist",
      ]
end
