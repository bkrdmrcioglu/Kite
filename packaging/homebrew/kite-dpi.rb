# Homebrew cask for the bkrdmrcioglu/homebrew-tap repository.
#   brew install --cask bkrdmrcioglu/tap/kite-dpi
cask "kite-dpi" do
  version "0.1.4"
  sha256 "a53f2fb90fb4957f4099974f45e018fbbb4c24bf4e43dde67185ef202c85c92f"

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
