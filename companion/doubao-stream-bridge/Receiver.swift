import Cocoa
import Carbon
import ApplicationServices

let root = URL(fileURLWithPath: CommandLine.arguments.dropFirst().first ?? FileManager.default.currentDirectoryPath)
func emit(_ event: String, _ fields: [String:Any] = [:]) {
    var object = fields; object["event"] = event
    object["time"] = Date().timeIntervalSince1970
    guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]),
          let handle = try? FileHandle(forWritingTo: root.appendingPathComponent("events.jsonl")) else { return }
    handle.seekToEndOfFile(); handle.write(data); handle.write(Data([10])); try? handle.close()
}
final class Receiver: NSObject, NSApplicationDelegate, NSTextViewDelegate {
    var window: NSWindow!
    var textView: NSTextView!
    var lastCommand = ""
    var previousInput: TISInputSource?
    var previousApp: NSRunningApplication?
    var holding = false
    var generation = 0
    var timer: Timer?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        window = NSWindow(contentRect: NSRect(x: 200, y: 200, width: 750, height: 330), styleMask: [.titled,.closable,.resizable,.miniaturizable], backing: .buffered, defer: false)
        window.title = "Doubao Stream Bridge — 文字接收窗口"
        let scroll = NSScrollView(frame: window.contentView!.bounds)
        scroll.autoresizingMask = [.width,.height]; scroll.hasVerticalScroller = true
        textView = NSTextView(frame: scroll.bounds)
        textView.isRichText = false; textView.font = .systemFont(ofSize: 22)
        textView.autoresizingMask = [.width]; textView.delegate = self
        scroll.documentView = textView; window.contentView!.addSubview(scroll)
        window.makeKeyAndOrderFront(nil); window.makeFirstResponder(textView)
        emit("launched", ["event_access": CGPreflightPostEventAccess(), "accessibility": AXIsProcessTrusted()])
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in self?.poll() }
    }
    func option(_ down: Bool) {
        guard let event = CGEvent(keyboardEventSource: CGEventSource(stateID: .hidSystemState), virtualKey: 61, keyDown: down) else { return }
        event.type = .flagsChanged
        event.flags = down ? CGEventFlags(rawValue: CGEventFlags.maskAlternate.rawValue | 0x40) : []
        event.post(tap: .cghidEventTap); holding = down
        emit(down ? "hotkey_down" : "hotkey_up")
    }
    func poll() {
        guard let data = try? Data(contentsOf: root.appendingPathComponent("control.json")),
              let cmd = try? JSONSerialization.jsonObject(with: data) as? [String:Any],
              let id = cmd["id"] as? String, id != lastCommand,
              let action = cmd["action"] as? String else { return }
        lastCommand = id
        switch action {
        case "start":
            guard CGPreflightPostEventAccess() else { emit("error", ["message":"Keyboard event access unavailable", "id":id]); return }
            guard previousInput == nil else { emit("error", ["message":"Session already active; restore it first", "id":id]); return }
            generation += 1
            let sessionGeneration = generation
            previousApp = NSWorkspace.shared.frontmostApplication
            previousInput = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
            textView.string = ""
            window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
            window.makeFirstResponder(textView)
            let filter = [kTISPropertyInputSourceID!: "com.bytedance.inputmethod.doubaoime.pinyin"] as CFDictionary
            let sources = TISCreateInputSourceList(filter, false).takeRetainedValue() as! [TISInputSource]
            if let source = sources.first { emit("input_selected", ["status": TISSelectInputSource(source)]) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                guard self.generation == sessionGeneration else { return }
                self.option(true)
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    guard self.generation == sessionGeneration else { return }
                    emit("ready", ["id": id])
                }
            }
        case "stop":
            generation += 1
            if holding { option(false) }
            emit("stopping", ["id": id])
        case "cancel":
            generation += 1
            if holding { option(false) }
            // Cancel pending recognition so a late commit cannot reach the restored app.
            window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
            for down in [true, false] {
                CGEvent(keyboardEventSource: nil, virtualKey: 53, keyDown: down)?.post(tap: .cghidEventTap)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { emit("cancelled", ["id":id]) }
        case "snapshot":
            emit("snapshot", ["text": textView.string, "marked": textView.hasMarkedText(), "id": id])
        case "restore":
            generation += 1
            if holding { option(false) }
            if let source = previousInput { TISSelectInputSource(source); previousInput = nil }
            previousApp?.activate(options: [])
            emit("restored", ["id": id])
        case "quit":
            generation += 1
            if holding { option(false) }
            if let source = previousInput { TISSelectInputSource(source) }
            NSApp.terminate(nil)
        default: emit("error", ["message": "Unknown command"])
        }
    }
    func textDidChange(_ notification: Notification) {
        emit("text", ["text": textView.string, "marked": textView.hasMarkedText()])
    }
}
try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
if !FileManager.default.fileExists(atPath: root.appendingPathComponent("events.jsonl").path) {
    FileManager.default.createFile(atPath: root.appendingPathComponent("events.jsonl").path, contents: nil)
}
let app = NSApplication.shared
let receiver = Receiver(); app.delegate = receiver; app.run()
