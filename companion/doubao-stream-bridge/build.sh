#!/bin/zsh
set -eu
cd "${0:A:h}"
mkdir -p build/DoubaoStreamReceiver.app/Contents/MacOS
swiftc Receiver.swift -o build/DoubaoStreamReceiver.app/Contents/MacOS/DoubaoStreamReceiver -framework Cocoa -framework Carbon -framework ApplicationServices
swiftc MicRoute.swift -o build/mic-route -framework Cocoa -framework ApplicationServices -framework CoreAudio
cat > build/DoubaoStreamReceiver.app/Contents/Info.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.doubao.streamreceiver</string>
<key>CFBundleName</key><string>DoubaoStreamReceiver</string>
<key>CFBundleExecutable</key><string>DoubaoStreamReceiver</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - build/DoubaoStreamReceiver.app
