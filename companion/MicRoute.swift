import Cocoa
import ApplicationServices
import CoreAudio

let routeDeadline = ProcessInfo.processInfo.systemUptime + 6.5
let settingsURL = URL(fileURLWithPath: "/Library/Input Methods/DoubaoIme.app/Contents/Applications/DoubaoImeSettings.app")
func routePause() { RunLoop.current.run(until: Date().addingTimeInterval(0.075)) }
func openSettings() throws -> NSRunningApplication {
    let config = NSWorkspace.OpenConfiguration()
    config.activates = false; config.createsNewApplicationInstance = false
    config.promptsUserIfNeeded = false; config.addsToRecentItems = false
    var application: NSRunningApplication?
    var completed = false
    NSWorkspace.shared.openApplication(at: settingsURL, configuration: config) { result, _ in
        // NSWorkspace invokes completion on a concurrent queue. Serialize its
        // result with the main-run-loop readiness polling below.
        DispatchQueue.main.async { application = result; completed = true }
    }
    while !completed && ProcessInfo.processInfo.systemUptime < routeDeadline { routePause() }
    guard completed, let application = application else {
        throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_SETTINGS_TIMEOUT")
    }
    return application
}
func attribute(_ element: AXUIElement, _ key: String) -> CFTypeRef? {
    guard ProcessInfo.processInfo.systemUptime < routeDeadline else { return nil }
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(element, key as CFString, &value) == .success ? value : nil
}
func label(_ element: AXUIElement) -> String {
    if let title = attribute(element, kAXTitleAttribute) as? String, !title.isEmpty { return title }
    return attribute(element, kAXDescriptionAttribute) as? String ?? ""
}
func descendants(_ element: AXUIElement, depth: Int = 0) -> [AXUIElement] {
    guard depth <= 12 && ProcessInfo.processInfo.systemUptime < routeDeadline else { return [] }
    AXUIElementSetMessagingTimeout(element, 0.1)
    let children = attribute(element, kAXChildrenAttribute) as? [AXUIElement] ?? []
    return [element] + children.prefix(256).flatMap { descendants($0, depth: depth + 1) }
}

final class SettingsRouteUI: MicrophoneRouteUI {
    var root: AXUIElement
    let deviceNames: Set<String>
    init(application: NSRunningApplication, deviceNames: Set<String>) {
        root = AXUIElementCreateApplication(application.processIdentifier)
        self.deviceNames = deviceNames
        AXUIElementSetMessagingTimeout(root, 0.1)
    }
    func settingsWindowAvailable() -> Bool? {
        guard let windows = attribute(root, kAXWindowsAttribute) as? [AXUIElement] else { return nil }
        return !windows.isEmpty
    }
    func reopenSettings() throws {
        let application = try openSettings()
        root = AXUIElementCreateApplication(application.processIdentifier)
        AXUIElementSetMessagingTimeout(root, 0.1)
    }
    func buttons() -> [AXUIElement] {
        descendants(root).filter {
            let role = attribute($0, kAXRoleAttribute) as? String
            return role == kAXButtonRole || role == kAXPopUpButtonRole || role == kAXMenuItemRole
        }
    }
    func pickerHasClose() -> Bool { buttons().contains { label($0) == "onboarding_close_icon" } }
    func currentButton() -> AXUIElement? {
        let items = buttons()
        guard !items.contains(where: { label($0) == "onboarding_close_icon" }) else { return nil }
        let microphones = items.filter { deviceNames.contains(label($0)) || label($0).hasPrefix("自动检测") }
        // Multiple different devices mean a picker or an incomplete hierarchy,
        // not an unambiguous selected microphone summary.
        guard Set(microphones.map(label)).count == 1 else { return nil }
        return microphones.first
    }
    func currentSelection() -> String? { currentButton().map(label) }
    func voiceNavigationAvailable() -> Bool { buttons().contains { label($0) == "语音输入" } }
    func press(_ element: AXUIElement?, code: String) throws {
        guard let element = element, AXUIElementPerformAction(element, kAXPressAction as CFString) == .success else {
            throw MicrophoneRouteError(code: code)
        }
    }
    func openVoicePage() throws {
        try press(buttons().first { label($0) == "语音输入" }, code: "MICROPHONE_ROUTE_PAGE_FAILED")
    }
    func openPicker() throws { try press(currentButton(), code: "MICROPHONE_ROUTE_PICKER_FAILED") }
    func targetChoice(_ requested: String) -> String? {
        let names = buttons().map(label)
        return names.first { $0 == requested } ?? names.first { normalizedMicrophone($0) == normalizedMicrophone(requested) }
    }
    func choose(_ requested: String) throws {
        try press(buttons().first { label($0) == requested }, code: "MICROPHONE_ROUTE_SELECT_FAILED")
    }
    func closePicker() throws {
        try press(buttons().first { label($0) == "onboarding_close_icon" }, code: "MICROPHONE_ROUTE_CLOSE_FAILED")
    }
}

func audioDeviceNames() throws -> Set<String> {
    var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr,
          size > 0 && size <= 65536 else { throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_AUDIO_ENUMERATION") }
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &ids) == noErr else {
        throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_AUDIO_ENUMERATION")
    }
    var names: Set<String> = ["Loopback Audio 2"]
    for id in ids {
        var name: Unmanaged<CFString>?
        var property = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var count = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        if AudioObjectGetPropertyData(id, &property, 0, nil, &count, &name) == noErr, let name = name {
            names.insert(name.takeUnretainedValue() as String)
        }
    }
    return names
}

func runRoute() throws -> [String: String] {
    guard AXIsProcessTrusted() else { throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_ACCESSIBILITY") }
    let bundle = "com.bytedance.inputmethod.doubaoime.settings"
    var application = NSRunningApplication.runningApplications(withBundleIdentifier: bundle).first
    let wasRunning = application != nil
    if application == nil {
        application = try openSettings()
    }
    guard let application = application else { throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_SETTINGS_TIMEOUT") }
    let ui = SettingsRouteUI(application: application, deviceNames: try audioDeviceNames())
    let controller = MicrophoneRouteController(ui: ui, deadline: routeDeadline, mayReopenSettings: wasRunning,
        now: { ProcessInfo.processInfo.systemUptime }, pause: routePause)
    let target = CommandLine.arguments.dropFirst().first
    let previous = try controller.run(target: target)
    return ["previous": previous, "requested": target ?? previous]
}

func output(_ value: [String: String]) {
    if let data = try? JSONSerialization.data(withJSONObject: value), let string = String(data: data, encoding: .utf8) {
        print(string)
    }
}
do { output(try runRoute()) }
catch let error as MicrophoneRouteError { output(["error": error.code]); exit(1) }
catch { output(["error": "MICROPHONE_ROUTE_INTERNAL"]); exit(1) }
