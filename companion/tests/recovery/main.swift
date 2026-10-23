import Foundation
import Darwin

// Pure recovery-helper tests: these scripts never inspect or operate macOS UI.
var time = 0.0
var frontmost: pid_t? = 12
var activations = 0
func activate() -> Bool { activations += 1; return true }
func pause() { time += 0.05; if time >= 0.15 { frontmost = 42 } }
assert(VoiceRecovery.foreground(process: 42, activate: activate,
    frontmostPID: { frontmost }, now: { time }, pause: pause))
assert(activations == 1 && time >= 0.15)
assert(VoiceRecovery.foreground(process: 42, activate: activate,
    frontmostPID: { frontmost }, now: { time }, pause: pause))
assert(activations == 1) // Already in front: do not steal focus again.
frontmost = 12
assert(!VoiceRecovery.foreground(process: 42, activate: { false },
    frontmostPID: { frontmost }, now: { time }, pause: pause))
let beforeTimeout = time
assert(!VoiceRecovery.foreground(process: 42, timeout: 0.2, activate: activate,
    frontmostPID: { frontmost }, now: { time }, pause: { time += 0.05 }))
assert(time >= beforeTimeout + 0.2 && time < beforeTimeout + 0.3)

let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("term50-recovery-test-\(UUID().uuidString)")
try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
defer { try? FileManager.default.removeItem(at: root) }
func script(_ name: String, _ content: String) throws -> String {
    let path = root.appendingPathComponent(name)
    try content.write(to: path, atomically: true, encoding: .utf8)
    return path.path
}
let success = try script("success.sh", "exit 0\n")
assert(VoiceRecovery.microphone(helper: "/bin/sh", target: success) == nil)
let failure = try script("failure.sh", "exit 3\n")
assert(VoiceRecovery.microphone(helper: "/bin/sh", target: failure) == "MICROPHONE_RESTORE_FAILED")
assert(VoiceRecovery.microphone(helper: root.appendingPathComponent("missing").path, target: "test") == "MICROPHONE_RESTORE_FAILED")
let blocked = try script("blocked.sh", "trap '' TERM\nexec sleep 30\n")
let started = ProcessInfo.processInfo.systemUptime
assert(VoiceRecovery.microphone(helper: "/bin/sh", target: blocked, timeout: 0.05) == "MICROPHONE_RESTORE_TIMEOUT")
assert(ProcessInfo.processInfo.systemUptime - started < 2)
let report = root.appendingPathComponent("report.json")
VoiceRecovery.report(path: report.path, session: "public-test-session", errors: ["MICROPHONE_RESTORE_TIMEOUT"])
let value = try JSONSerialization.jsonObject(with: Data(contentsOf: report)) as! [String: Any]
assert(value["event"] as? String == "watchdog_restore_failed")
assert(value["codes"] as? [String] == ["MICROPHONE_RESTORE_TIMEOUT"])
assert(value.count == 4)
let attributes = try FileManager.default.attributesOfItem(atPath: report.path)
assert((attributes[.posixPermissions] as! NSNumber).intValue == 0o600)
print("voice recovery: foreground confirmation, microphone success/failure/timeout, private diagnostic PASS")
