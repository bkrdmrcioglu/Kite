# Homebrew cask for the bkrdmrcioglu/homebrew-tap repository.
#   brew install --cask bkrdmrcioglu/tap/kite-dpi
cask "kite-dpi" do
  version "0.1.1"
  sha256 "04839086dfbce98dffa38b046edb1b5781604e1ada1128d37ec6a4edb6eae16b"

  url "https://github.com/bkrdmrcioglu/Kite/releases/download/v#{version}/Kite-DPI-#{version}-macOS.dmg"
  name "Kite DPI"
  desc "DPI bypass that runs entirely on your Mac, no VPN server"
  homepage "https://github.com/bkrdmrcioglu/Kite"

  depends_on macos: :sonoma

  app "Kite DPI.app"

  # quitting restores the system proxy settings
  uninstall quit: "com.bkrdmrcioglu.kite"

  zap trash: [
    "~/Library/Application Support/Kite",
    "~/Library/Preferences/com.bkrdmrcioglu.kite.plist",
  ]

  caveats <<~EOS
    If you enabled "Discord app" mode, turn it off in Kite DPI before
    uninstalling so the entries it added to /etc/hosts are removed.
  EOS
end
