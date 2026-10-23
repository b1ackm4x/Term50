#!/bin/zsh
set -eu
cd "${0:A:h}"
app=build/Term50VoiceReceiver.app
mkdir -p "$app/Contents/MacOS"
cp Receiver.swift build/main.swift
swiftc build/main.swift Recovery.swift -o "$app/Contents/MacOS/Term50VoiceReceiver" -framework Cocoa -framework Carbon -framework ApplicationServices
mkdir -p build/mic-route-source
cp MicRoute.swift build/mic-route-source/main.swift
swiftc build/mic-route-source/main.swift MicRouteController.swift -o build/mic-route -framework Cocoa -framework ApplicationServices -framework CoreAudio
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.github.term50.voice-receiver</string>
<key>CFBundleName</key><string>Term50VoiceReceiver</string>
<key>CFBundleExecutable</key><string>Term50VoiceReceiver</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$app"
