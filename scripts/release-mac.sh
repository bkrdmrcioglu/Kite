#!/usr/bin/env bash
# Builds a universal, Developer ID signed and notarized Kite DPI DMG.
#
#   scripts/release-mac.sh 0.1.1
#
# Notarization uses the Apple account signed in to Xcode (Settings > Accounts),
# so no app-specific password or API key is needed.
set -euo pipefail

VERSION=${1:?usage: release-mac.sh <version>}
TEAM=${KITE_TEAM:-HLZQLSTBB8}
# SHA-1 of the Developer ID Application certificate (two share the same name)
IDENTITY=${KITE_SIGN_IDENTITY:-9FB49F616367DD1EDDEEFE1720AC4940701104A5}
NAME="Kite DPI"

cd "$(dirname "$0")/.."
(cd apple && xcodegen generate -q)

OUT=build/release-mac
rm -rf "$OUT" && mkdir -p "$OUT" dist
xcodebuild -project apple/Kite.xcodeproj -scheme Kite -configuration Release \
  -destination 'generic/platform=macOS' -archivePath "$OUT/Kite.xcarchive" \
  -derivedDataPath build/dd-release-mac \
  ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO MARKETING_VERSION="$VERSION" \
  CODE_SIGN_STYLE=Automatic DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_IDENTITY="Apple Development" \
  -allowProvisioningUpdates archive -quiet

cat > "$OUT/export.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>developer-id</string>
  <key>destination</key><string>upload</string>
  <key>teamID</key><string>$TEAM</string>
  <key>signingStyle</key><string>automatic</string>
</dict></plist>
PLIST

# upload for notarization, then wait for Apple and export the stapled app
xcodebuild -exportArchive -archivePath "$OUT/Kite.xcarchive" -exportOptionsPlist "$OUT/export.plist" \
  -exportPath "$OUT/upload" -allowProvisioningUpdates
for _ in $(seq 1 60); do
  if xcodebuild -exportNotarizedApp -archivePath "$OUT/Kite.xcarchive" -exportPath "$OUT/notarized" \
       -allowProvisioningUpdates >/dev/null 2>&1; then
    break
  fi
  sleep 30
done

APP="$OUT/notarized/$NAME.app"
xcrun stapler validate "$APP"
spctl --assess --type execute "$APP"
lipo -archs "$APP/Contents/MacOS/$NAME"

STAGE="$OUT/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="dist/Kite-DPI-$VERSION-macOS.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "$NAME $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
codesign --sign "$IDENTITY" --timestamp "$DMG"
shasum -a 256 "$DMG"
