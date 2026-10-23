import Cocoa
import ApplicationServices
import CoreAudio

func attr(_ e: AXUIElement, _ key: String) -> CFTypeRef? {
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(e, key as CFString, &value) == .success ? value : nil
}
func descendants(_ root: AXUIElement, depth: Int = 0) -> [AXUIElement] {
    if depth > 12 { return [] }
    let children = attr(root, kAXChildrenAttribute) as? [AXUIElement] ?? []
    return [root] + children.flatMap { descendants($0, depth: depth + 1) }
}
func name(_ e: AXUIElement) -> String {
    return (attr(e, kAXTitleAttribute) as? String) ?? (attr(e, kAXDescriptionAttribute) as? String) ?? ""
}
func pause(_ seconds: Double) { RunLoop.current.run(until: Date().addingTimeInterval(seconds)) }
func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8)); exit(1)
}
guard AXIsProcessTrusted() else { fail("Accessibility access unavailable") }
let bundle = "com.bytedance.inputmethod.doubaoime.settings"
var running = NSRunningApplication.runningApplications(withBundleIdentifier: bundle).first
if running == nil {
    let url = URL(fileURLWithPath: "/Library/Input Methods/DoubaoIme.app/Contents/Applications/DoubaoImeSettings.app")
    let config = NSWorkspace.OpenConfiguration(); config.activates = false
    NSWorkspace.shared.openApplication(at: url, configuration: config) { application, error in running = application }
    let deadline = Date().addingTimeInterval(8)
    while running == nil && Date() < deadline { pause(0.1) }
}
guard let application = running else { fail("Could not open Doubao settings") }
let root = AXUIElementCreateApplication(application.processIdentifier)
func buttons() -> [AXUIElement] {
    descendants(root).filter { (attr($0,kAXRoleAttribute) as? String) == kAXButtonRole }
}
if let voice = buttons().first(where: { name($0) == "语音输入" }) {
    AXUIElementPerformAction(voice,kAXPressAction as CFString); pause(0.25)
}
var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
var size: UInt32 = 0
AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size)
var ids = [AudioDeviceID](repeating: 0, count: Int(size)/MemoryLayout<AudioDeviceID>.size)
AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids)
var deviceNames = Set<String>()
for id in ids {
    var n: Unmanaged<CFString>?
    var a = AudioObjectPropertyAddress(mSelector:kAudioObjectPropertyName,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
    var s = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    if AudioObjectGetPropertyData(id, &a, 0, nil, &s, &n) == noErr, let value = n { deviceNames.insert(value.takeUnretainedValue() as String) }
}
// This version also retains a stale pre-rename label until settings is restarted.
deviceNames.insert("Loopback Audio 2")
guard let current = buttons().first(where: { deviceNames.contains(name($0)) || name($0).hasPrefix("自动检测") }) else {
    fail("Microphone selection button not found; open Doubao voice settings")
}
let previous = name(current)
let target = CommandLine.arguments.dropFirst().first
if let target = target, target != previous {
    guard AXUIElementPerformAction(current,kAXPressAction as CFString) == .success else { fail("Could not open microphone picker") }
    pause(0.3)
    let all = buttons()
    let selection = all.first { name($0) == target } ?? (target == "Doubao Stream Bridge" ? all.first { name($0) == "Loopback Audio 2" } : nil)
    guard let selection = selection else { fail("Requested microphone not in picker: \(target)") }
    guard AXUIElementPerformAction(selection,kAXPressAction as CFString) == .success else { fail("Could not select microphone") }
    pause(0.2)
    if let close = buttons().first(where: { name($0) == "onboarding_close_icon" }) {
        AXUIElementPerformAction(close,kAXPressAction as CFString)
    }
    pause(0.2)
    let selected = buttons().contains { name($0) == target || (target == "Doubao Stream Bridge" && name($0) == "Loopback Audio 2") }
    guard selected else { fail("Microphone selection did not take effect") }
}
let data = try JSONSerialization.data(withJSONObject:["previous":previous,"requested":target ?? previous])
print(String(data:data,encoding:.utf8)!)
