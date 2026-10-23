import Foundation

// Pure state tests: no Cocoa, Accessibility, CoreAudio, or actual helper calls.
final class Clock { var time: TimeInterval = 0 }
final class FakeUI: MicrophoneRouteUI {
    let clock: Clock
    var pageOpen = false, pickerOpen = false
    var navigationAt: TimeInterval = 0.3, summaryAt: TimeInterval = 0.6, choiceAt: TimeInterval = 1
    var previous = "外置麦克风", choice = "Loopback Audio 2"
    var targetPresent = true, commitSelection = true, selectFails = false
    var selectedAt: TimeInterval?
    var navigationClicks = 0, pickerClicks = 0, selectionClicks = 0, closeClicks = 0
    var windowAvailable: Bool? = true
    var reopenSucceeds = true, reopenThrows = false
    var reopenCount = 0
    init(_ clock: Clock) { self.clock = clock }
    func settingsWindowAvailable() -> Bool? { windowAvailable }
    func reopenSettings() throws {
        reopenCount += 1
        if reopenThrows { throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_SETTINGS_TIMEOUT") }
        if reopenSucceeds { windowAvailable = true; navigationAt = clock.time + 0.2; summaryAt = clock.time + 0.4 }
    }
    func currentSelection() -> String? {
        guard windowAvailable == true && pageOpen && !pickerOpen && clock.time >= summaryAt else { return nil }
        if let selected = selectedAt, commitSelection && clock.time >= selected + 0.3 { return choice }
        return previous
    }
    func voiceNavigationAvailable() -> Bool { windowAvailable == true && clock.time >= navigationAt }
    func openVoicePage() throws { navigationClicks += 1; pageOpen = true }
    func openPicker() throws { pickerClicks += 1; pickerOpen = true }
    func targetChoice(_ requested: String) -> String? {
        pickerOpen && targetPresent && clock.time >= choiceAt ? choice : nil
    }
    func choose(_ label: String) throws {
        selectionClicks += 1
        if selectFails { throw MicrophoneRouteError(code: "MICROPHONE_ROUTE_SELECT_FAILED") }
        assert(label == choice); selectedAt = clock.time
    }
    func pickerHasClose() -> Bool { pickerOpen }
    func closePicker() throws { closeClicks += 1; assert(pickerOpen); pickerOpen = false }
}
func controller(_ ui: FakeUI, _ clock: Clock) -> MicrophoneRouteController {
    MicrophoneRouteController(ui: ui, deadline: 3, now: { clock.time }, pause: { clock.time += 0.05 })
}
do {
    let clock = Clock(), ui: FakeUI
    ui = FakeUI(clock)
    let previous = try controller(ui, clock).run(target: nil)
    assert(previous == "外置麦克风")
    assert(clock.time >= 0.6 && ui.navigationClicks == 1 && ui.pickerClicks == 0)
    assert(ui.reopenCount == 0)
}
do {
    let clock = Clock(), ui: FakeUI
    ui = FakeUI(clock)
    let previous = try controller(ui, clock).run(target: "Doubao Stream Bridge")
    assert(previous == "外置麦克风")
    assert(ui.currentSelection() == "Loopback Audio 2")
    assert(ui.navigationClicks == 1 && ui.pickerClicks == 1 && ui.selectionClicks == 1 && ui.closeClicks == 1)
    assert(clock.time >= ui.selectedAt! + 0.3)
}
do {
    let clock = Clock(), ui: FakeUI
    ui = FakeUI(clock); ui.pageOpen = true; ui.summaryAt = 0; ui.previous = "Loopback Audio 2"
    let previous = try controller(ui, clock).run(target: "Doubao Stream Bridge")
    assert(previous == "Loopback Audio 2")
    assert(ui.navigationClicks == 0 && ui.pickerClicks == 0 && ui.selectionClicks == 0)
    assert(ui.reopenCount == 0)
}
do {
    let clock = Clock(), ui: FakeUI
    ui = FakeUI(clock); ui.windowAvailable = false
    let previous = try controller(ui, clock).run(target: nil)
    assert(previous == "外置麦克风")
    assert(ui.reopenCount == 1 && ui.navigationClicks == 1 && ui.pickerClicks == 0)
}
for scenario in ["reopen_has_no_window", "reopen_fails", "unknown_window", "cold_launch"] {
    let clock = Clock(), ui: FakeUI
    ui = FakeUI(clock); ui.windowAvailable = false; ui.reopenSucceeds = false
    if scenario == "unknown_window" { ui.windowAvailable = nil }
    if scenario == "reopen_fails" { ui.reopenThrows = true }
    let route = MicrophoneRouteController(ui: ui, deadline: 3, mayReopenSettings: scenario != "cold_launch",
        now: { clock.time }, pause: { clock.time += 0.05 })
    do {
        _ = try route.run(target: nil)
        assertionFailure("expected bounded readiness failure")
    } catch let error as MicrophoneRouteError {
        assert(error.code == (scenario == "reopen_fails" ? "MICROPHONE_ROUTE_SETTINGS_TIMEOUT" : "MICROPHONE_ROUTE_PAGE_TIMEOUT"))
        assert(ui.reopenCount == (scenario == "unknown_window" || scenario == "cold_launch" ? 0 : 1))
        assert(ui.navigationClicks == 0 && ui.pickerClicks == 0 && ui.selectionClicks == 0)
        assert(clock.time <= 3.1)
    }
}
for scenario in ["missing", "uncommitted", "action_failed", "not_ready"] {
    let clock = Clock(), ui: FakeUI
    ui = FakeUI(clock)
    if scenario == "missing" { ui.targetPresent = false }
    if scenario == "uncommitted" { ui.commitSelection = false }
    if scenario == "action_failed" { ui.selectFails = true }
    if scenario == "not_ready" { ui.navigationAt = 10 }
    do {
        _ = try controller(ui, clock).run(target: "Doubao Stream Bridge")
        assertionFailure("expected failure: \(scenario)")
    } catch let error as MicrophoneRouteError {
        let expected = ["missing": "MICROPHONE_ROUTE_DEVICE_MISSING", "uncommitted": "MICROPHONE_ROUTE_VERIFY_TIMEOUT",
                        "action_failed": "MICROPHONE_ROUTE_SELECT_FAILED", "not_ready": "MICROPHONE_ROUTE_PAGE_TIMEOUT"]
        assert(error.code == expected[scenario])
        assert(ui.pickerClicks <= 1 && ui.selectionClicks <= 1 && ui.closeClicks <= 1)
    }
}
print("voice microphone routing: closed-window reopen, cold readiness, delayed picker, alias, committed summary, bounded failure, single mutation PASS")
