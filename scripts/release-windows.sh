#!/usr/bin/env bash
# Cross-compiles Kite for Windows (x64) and zips it. Needs: brew install mingw-w64
#
#   scripts/release-windows.sh 0.1.0
set -euo pipefail
VERSION=${1:?usage: release-windows.sh <version>}
cd "$(dirname "$0")/.."
rm -f build/win/kite_res.o
make windows
mkdir -p dist
cp build/win/Kite.exe "dist/Kite-$VERSION-Windows-x64.exe"
(cd build/win && zip -q "../../dist/Kite-$VERSION-Windows-x64.zip" Kite.exe)
shasum -a 256 dist/Kite-"$VERSION"-Windows-x64.*
