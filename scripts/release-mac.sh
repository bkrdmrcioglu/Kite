#!/usr/bin/env bash
# Builds a universal, Developer ID signed and notarized Kite DMG.
#
#   scripts/release-mac.sh 0.1.0
#
# One-time setup (stores an app-specific password in the keychain):
#   xcrun notarytool store-credentials kite-notary --apple-id <email> --team-id HLZQLSTBB8
set -euo pipefail

VERSION=${1:?usage: release-mac.sh <version>}
# SHA-1 of the Developer ID Application certificate (two share the same name)
IDENTITY=${KITE_SIGN_IDENTITY:-9FB49F616367DD1EDDEEFE1720AC4940701104A5}
TEAM=${KITE_TEAM:-HLZQLSTBB8}
PROFILE=${KITE_NOTARY_PROFILE:-kite-notary}

cd "$(dirname "$0")/.."
(cd apple && xcodegen generate -q)

xcodebuild -project apple/Kite.xcodeproj -scheme Kite -configuration Release \
  -derivedDataPath build/release -destination 'generic/platform=macOS' \
  ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO \
  MARKETING_VERSION="$VERSION" \
  CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$IDENTITY" DEVELOPMENT_TEAM="$TEAM" \
  OTHER_CODE_SIGN_FLAGS="--timestamp" \
  build -quiet

APP=build/release/Build/Products/Release/Kite.app
codesign --verify --strict --deep "$APP"
lipo -archs "$APP/Contents/MacOS/Kite"

STAGE=build/dmg
rm -rf "$STAGE" && mkdir -p "$STAGE" dist
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="dist/Kite-$VERSION-macOS.dmg"
hdiutil create -volname "Kite $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
codesign --sign "$IDENTITY" --timestamp "$DMG"

xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature -v "$DMG"
shasum -a 256 "$DMG"
