#!/usr/bin/env bash
# Cross-compiles Kite DPI for Windows (x64). Needs: brew install mingw-w64
#
#   scripts/release-windows.sh 0.1.1
set -euo pipefail
VERSION=${1:?usage: release-windows.sh <version>}
cd "$(dirname "$0")/.."
rm -f build/win/kite_res.o build/win/Kite.exe
make windows
mkdir -p dist build/win/zip
cp build/win/Kite.exe "dist/Kite-DPI-$VERSION-Windows-x64.exe"
cp build/win/Kite.exe "build/win/zip/Kite DPI.exe"
rm -f "dist/Kite-DPI-$VERSION-Windows-x64.zip"
(cd build/win/zip && zip -q "../../../dist/Kite-DPI-$VERSION-Windows-x64.zip" "Kite DPI.exe")
shasum -a 256 dist/Kite-DPI-"$VERSION"-Windows-x64.*
