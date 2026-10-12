# Homebrew cask for the bkrdmrcioglu/homebrew-tap repository.
#   brew install --cask bkrdmrcioglu/tap/kite
cask "kite" do
  version "0.1.0"
  sha256 "REPLACE_WITH_DMG_SHA256"

  url "https://github.com/bkrdmrcioglu/Kite/releases/download/v#{version}/Kite-#{version}-macOS.dmg"
  name "Kite"
  desc "DPI bypass that runs entirely on your Mac, no VPN server"
  homepage "https://github.com/bkrdmrcioglu/Kite"

  depends_on macos: :sonoma

  app "Kite.app"

  # quitting restores the system proxy settings
  uninstall quit: "com.bkrdmrcioglu.kite"

  zap trash: [
    "~/Library/Application Support/Kite",
    "~/Library/Preferences/com.bkrdmrcioglu.kite.plist",
  ]

  caveats <<~EOS
    If you enabled "Discord app" mode, turn it off in Kite before uninstalling
    so the entries Kite added to /etc/hosts are removed.
  EOS
end
