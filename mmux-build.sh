#!/bin/bash
# MouseMux build script - MUST relink xul.dll for widget/windows changes!
# nsWindow.cpp and MouseMuxClient.cpp are linked into xul.dll, NOT firefox.exe

set -e
export PATH="/c/mozilla-build/python3:/c/mozilla-build/msys2/usr/bin:$PATH"
cd "$(dirname "$0")"

echo "=== MouseMux Build ==="
echo "Clearing old log..."
rm -f /d/scratch/firefox/mousemux_client.log 2>/dev/null || true

echo "Touching source files (forces __DATE__/__TIME__ rebuild)..."
touch widget/windows/nsWindow.cpp widget/windows/MouseMuxClient.cpp widget/windows/InputFilter.cpp widget/windows/MouseMuxDebugDialog.cpp 2>/dev/null || true

echo "Removing xul.dll to force relink..."
rm -f obj-x86_64-pc-windows-msvc/toolkit/library/xul.dll 2>/dev/null || true
rm -f obj-x86_64-pc-windows-msvc/dist/bin/xul.dll 2>/dev/null || true
rm -f obj-x86_64-pc-windows-msvc/dist/firefox/xul.dll 2>/dev/null || true

echo "Building..."
./mach build

echo "=== Build complete ==="
