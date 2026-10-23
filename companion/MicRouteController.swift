import Foundation

struct MicrophoneRouteError: Error { let code: String }

protocol MicrophoneRouteUI {
    // nil means Accessibility could not determine the window state.
    func settingsWindowAvailable() -> Bool?
    func reopenSettings() throws
    func currentSelection() -> String?
    func voiceNavigationAvailable() -> Bool
    func openVoicePage() throws
    func openPicker() throws
    func targetChoice(_ requested: String) -> String?
    func choose(_ label: String) throws
    func pickerHasClose() -> Bool
    func closePicker() throws
}

func normalizedMicrophone(_ name: String) -> String {
    name == "Loopback Audio 2" ? "Doubao Stream Bridge" : name
}

// Poll observations, never repeat a potentially mutating accessibility action.
final class MicrophoneRouteController {
    let ui: MicrophoneRouteUI
    let now: () -> TimeInterval
    let pause: () -> Void
    let deadline: TimeInterval
    let mayReopenSettings: Bool

    init(ui: MicrophoneRouteUI, deadline: TimeInterval, mayReopenSettings: Bool = true,
         now: @escaping () -> TimeInterval, pause: @escaping () -> Void) {
        self.ui = ui; self.deadline = deadline; self.now = now; self.pause = pause
        self.mayReopenSettings = mayReopenSettings
    }

    func wait<T>(_ code: String, _ observe: () throws -> T?) throws -> T {
        while now() < deadline {
            if let result = try observe() { return result }
            pause()
        }
        throw MicrophoneRouteError(code: code)
    }

    func run(target: String?) throws -> String {
        enum Ready { case current(String), navigation }
        var reopenedSettings = false
        let ready: Ready = try wait("MICROPHONE_ROUTE_PAGE_TIMEOUT") {
            if let current = self.ui.currentSelection() { return .current(current) }
            if self.ui.voiceNavigationAvailable() { return .navigation }
            if self.mayReopenSettings && !reopenedSettings && self.ui.settingsWindowAvailable() == false {
                // A running process can outlive its last window. Reopen once in
                // the background, then keep observing under the same deadline.
                reopenedSettings = true
                try self.ui.reopenSettings()
            }
            return nil
        }
        let previous: String
        switch ready {
        case .current(let value): previous = value
        case .navigation:
            try ui.openVoicePage()
            previous = try wait("MICROPHONE_ROUTE_PAGE_TIMEOUT") { self.ui.currentSelection() }
        }
        guard let target = target, normalizedMicrophone(target) != normalizedMicrophone(previous) else { return previous }
        try ui.openPicker()
        let choice = try wait("MICROPHONE_ROUTE_DEVICE_MISSING") { self.ui.targetChoice(target) }
        try ui.choose(choice)
        // Selection handlers can update their settings asynchronously. This is
        // a minimum debounce only; final completion is checked from the summary.
        let selectedAt = now()
        var stable = 0
        enum Verification { case closePicker, verified }
        let next: Verification = try wait("MICROPHONE_ROUTE_VERIFY_TIMEOUT") {
            if self.now() - selectedAt < 0.2 { return nil }
            if self.ui.pickerHasClose() {
                return .closePicker
            }
            if let summary = self.ui.currentSelection(), normalizedMicrophone(summary) == normalizedMicrophone(target) {
                stable += 1
                return stable >= 2 ? .verified : nil
            }
            stable = 0
            return nil
        }
        // A picker that remains open must be closed once, then verified from the
        // main page. Merely seeing the target in its choices is never success.
        if case .closePicker = next {
            try ui.closePicker(); stable = 0
            let _: Bool = try wait("MICROPHONE_ROUTE_VERIFY_TIMEOUT") {
                guard !self.ui.pickerHasClose(), let summary = self.ui.currentSelection(),
                      normalizedMicrophone(summary) == normalizedMicrophone(target) else { stable = 0; return nil }
                stable += 1
                return stable >= 2 ? true : nil
            }
        }
        return previous
    }
}
