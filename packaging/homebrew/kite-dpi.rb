# Homebrew cask for the bkrdmrcioglu/homebrew-tap repository.
#   brew install --cask bkrdmrcioglu/tap/kite-dpi
cask "kite-dpi" do
  version "0.1.2"
  sha256 "3f64c91d3fe0117ffc7a421bfa3bb76d5ed20b212ecf1da89f27b504a4258de2"

  url "https://github.com/bkrdmrcioglu/Kite/releases/download/v#{version}/Kite-DPI-#{version}-macOS.dmg"
  name "Kite DPI"
  desc "DPI bypass that runs entirely on your Mac, no VPN server"
  homepage "https://github.com/bkrdmrcioglu/Kite"

  depends_on macos: :sonoma

  app "Kite DPI.app"

  # quitting restores the system proxy settings; the script removes the
  # /etc/hosts entries Kite DPI adds for apps that ignore the proxy
  uninstall quit:   "com.bkrdmrcioglu.kite",
            script: {
              executable: "/usr/bin/sed",
              args:       [
                "-i", "",
                "-e", "/^# >>> Kite DPI >>>$/,/^# <<< Kite DPI <<<$/d",
                "-e", "/^# >>> Kite (Discord) >>>$/,/^# <<< Kite (Discord) <<<$/d",
                "/etc/hosts"
              ],
              sudo:       true,
            }

  zap trash: [
    "~/Library/Application Support/Kite",
    "~/Library/Preferences/com.bkrdmrcioglu.kite.plist",
  ]
end
