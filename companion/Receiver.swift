import Cocoa
import Carbon
import ApplicationServices

// Owned receiver: all transcript files are private and per-session.
let root = URL(fileURLWithPath: CommandLine.arguments[1])
let session = CommandLine.arguments[2]
let routeHelper = CommandLine.arguments.count > 3 ? CommandLine.arguments[3] : ""
let previousMicrophone = CommandLine.arguments.count > 4 ? CommandLine.arguments[4] : ""
let recoveryReport = CommandLine.arguments.count > 5 ? CommandLine.arguments[5] : ""
func emit(_ name: String, _ fields: [String: Any] = [:]) {
    var object = fields
    object["event"] = name; object["session"] = session
    guard let data = try? JSONSerialization.data(withJSONObject: object),
          let output = try? FileHandle(forWritingTo: root.appendingPathComponent("events.jsonl")) else { return }
    output.seekToEndOfFile(); output.write(data); output.write(Data([10])); try? output.close()
}

final class InputView: NSTextView {
    var changed: (() -> Void)?
    override func setMarkedText(_ string: Any, selectedRange: NSRange, replacementRange: NSRange) {
        super.setMarkedText(string, selectedRange: selectedRange, replacementRange: replacementRange)
        changed?()
    }
    override func insertText(_ string: Any, replacementRange: NSRange) {
        super.insertText(string, replacementRange: replacementRange); changed?()
    }
    override func unmarkText() { super.unmarkText(); changed?() }
}

final class Receiver: NSObject, NSApplicationDelegate, NSWindowDelegate, NSTextViewDelegate {
    var window: NSWindow!
    var view: InputView!
    var previousInput: TISInputSource?
    var previousApp: NSRunningApplication?
    var holding = false
    var active = false
    var finishing = false
    var generation = 0
    var lastCommand = ""
    var lastLease = ""
    var leaseTime = ProcessInfo.processInfo.systemUptime
    var lastText = ""
    var lastMarked = false
    var revision = 0
    var removeRuntimeOnExit = false
    var timer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        window = NSWindow(contentRect: NSRect(x: 180, y: 200, width: 700, height: 300),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Term50 语音输入 — 豆包接收窗口"
        window.delegate = self
        let scroll = NSScrollView(frame: window.contentView!.bounds)
        scroll.autoresizingMask = [.width, .height]; scroll.hasVerticalScroller = true
        view = InputView(frame: scroll.bounds)
        view.isRichText = false; view.font = .systemFont(ofSize: 22)
        view.delegate = self; view.changed = { [weak self] in self?.snapshot() }
        scroll.documentView = view; window.contentView!.addSubview(scroll)
        // Do not steal focus before saving the previous foreground application.
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in self?.poll() }
        emit("launched", ["event_access": CGPreflightPostEventAccess()])
    }

    func option(_ down: Bool) {
        guard let event = CGEvent(keyboardEventSource: CGEventSource(stateID: .hidSystemState),
                                  virtualKey: 61, keyDown: down) else { return }
        event.type = .flagsChanged
        event.flags = down ? CGEventFlags(rawValue: CGEventFlags.maskAlternate.rawValue | 0x40) : []
        event.post(tap: .cghidEventTap); holding = down
    }

    func snapshot() {
        guard active && !finishing else { return }
        let text = view.string, marked = view.hasMarkedText()
        if text.utf8.count > 16384 {
            finish(cancel: true, id: nil, error: "TEXT_LIMIT"); return
        }
        if text != lastText || marked != lastMarked {
            lastText = text; lastMarked = marked; revision += 1
            emit("text", ["text": text, "marked": marked, "revision": revision])
        }
    }

    func foregroundSafe() -> Bool {
        guard let front = NSWorkspace.shared.frontmostApplication else { return false }
        if front.processIdentifier == ProcessInfo.processInfo.processIdentifier { return true }
        // Doubao can activate its recording panel. Other app activation cancels.
        return front.bundleIdentifier == "com.bytedance.inputmethod.doubaoime"
    }

    func restore() -> [String] {
        var errors: [String] = []
        if let input = previousInput {
            if TISSelectInputSource(input) == noErr { previousInput = nil }
            else { errors.append("INPUT_SOURCE_RESTORE_FAILED") }
        }
        if let app = previousApp {
            let foregroundRestored = VoiceRecovery.foreground(process: app.processIdentifier,
                activate: { app.activate(options: []) },
                frontmostPID: { NSWorkspace.shared.frontmostApplication?.processIdentifier },
                now: { ProcessInfo.processInfo.systemUptime },
                pause: { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) })
            if !foregroundRestored { errors.append("FOREGROUND_RESTORE_FAILED") }
        }
        previousApp = nil; active = false
        return errors
    }

    func completeRestoration(id: String?, microphoneError: String? = nil) {
        var errors: [String] = []
        if let error = microphoneError { errors.append(error) }
        errors.append(contentsOf: restore())
        if removeRuntimeOnExit { VoiceRecovery.report(path: recoveryReport, session: session, errors: errors) }
        if errors.isEmpty {
            emit("restored", ["id": id ?? "watchdog"])
        } else {
            emit("restore_failed", ["id": id ?? "watchdog", "code": errors[0], "codes": errors])
        }
        NSApp.terminate(nil)
    }

    func finish(cancel: Bool, id: String?, error: String? = nil) {
        guard !finishing else { return }
        finishing = true; generation += 1
        if holding { option(false) }
        if let error = error { emit("error", ["code": error]) }
        if error == "BRIDGE_LEASE_EXPIRED" { removeRuntimeOnExit = true }
        if cancel && active {
            window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
            window.makeFirstResponder(view)
            for down in [true, false] {
                CGEvent(keyboardEventSource: nil, virtualKey: 53, keyDown: down)?.post(tap: .cghidEventTap)
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + (cancel ? 0.75 : 0.05)) {
            if self.removeRuntimeOnExit && !routeHelper.isEmpty && !previousMicrophone.isEmpty {
                // Option was released before reaching this point. Only a lost
                // adapter lease transfers microphone ownership to Receiver.
                DispatchQueue.global(qos: .userInitiated).async {
                    let error = VoiceRecovery.microphone(helper: routeHelper, target: previousMicrophone)
                    DispatchQueue.main.async { self.completeRestoration(id: id, microphoneError: error) }
                }
            } else {
                self.completeRestoration(id: id)
            }
        }
    }

    func poll() {
        let now = ProcessInfo.processInfo.systemUptime
        if let lease = try? String(contentsOf: root.appendingPathComponent("lease"), encoding: .utf8), lease != lastLease {
            lastLease = lease; leaseTime = now
        }
        if now - leaseTime > 4 { finish(cancel: true, id: nil, error: "BRIDGE_LEASE_EXPIRED"); return }
        if active && !finishing && !foregroundSafe() {
            // Restore the app the user just chose, instead of an older app.
            previousApp = NSWorkspace.shared.frontmostApplication
            finish(cancel: true, id: nil, error: "FOCUS_LOST"); return
        }
        snapshot()
        guard !finishing, let data = try? Data(contentsOf: root.appendingPathComponent("control.json")),
              let command = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              command["session"] as? String == session,
              let id = command["id"] as? String, id != lastCommand,
              let action = command["action"] as? String else { return }
        lastCommand = id
        switch action {
        case "start":
            guard !active && previousInput == nil else { emit("error", ["code": "ALREADY_ACTIVE"]); return }
            guard CGPreflightPostEventAccess() else { emit("error", ["code": "EVENT_PERMISSION"]); return }
            let filter = [kTISPropertyInputSourceID!: "com.bytedance.inputmethod.doubaoime.pinyin"] as CFDictionary
            let sources = TISCreateInputSourceList(filter, false).takeRetainedValue() as! [TISInputSource]
            guard let source = sources.first else { emit("error", ["code": "DOUBAO_UNAVAILABLE"]); return }
            previousApp = NSWorkspace.shared.frontmostApplication
            previousInput = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
            guard TISSelectInputSource(source) == noErr else { emit("error", ["code": "INPUT_SOURCE_FAILED"]); return }
            view.string = ""; lastText = ""; lastMarked = false; revision = 0
            window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
            window.makeFirstResponder(view)
            generation += 1; let expected = generation
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
                guard expected == self.generation && !self.finishing else { return }
                guard NSApp.isActive && self.window.firstResponder === self.view else {
                    self.finish(cancel: true, id: id, error: "FOCUS_FAILED"); return
                }
                self.active = true; self.option(true)
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    guard expected == self.generation && !self.finishing else { return }
                    emit("ready", ["id": id])
                }
            }
        case "stop":
            generation += 1
            if holding { option(false) }
            snapshot(); emit("stopping", ["id": id])
        case "cancel": finish(cancel: true, id: id)
        case "restore": finish(cancel: false, id: id)
        default: emit("error", ["code": "BAD_COMMAND"])
        }
    }
    func textDidChange(_ notification: Notification) { snapshot() }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        finish(cancel: true, id: nil, error: "WINDOW_CLOSED"); return false
    }
    func applicationWillTerminate(_ notification: Notification) {
        if holding { option(false) }
        if let input = previousInput, TISSelectInputSource(input) != noErr {
            emit("restore_failed", ["id": "termination", "code": "INPUT_SOURCE_RESTORE_FAILED"])
        }
        // Adapter hard-kill skips Python's TemporaryDirectory cleanup. Its lease
        // expiry is the receiver's authority to remove its own private files.
        if removeRuntimeOnExit { try? FileManager.default.removeItem(at: root) }
    }
}

let app = NSApplication.shared
let receiver = Receiver(); app.delegate = receiver; app.run()
