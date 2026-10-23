import Foundation
import Darwin

// This helper has no UI of its own. The actual route helper is invoked only by
// Receiver's orphan-session recovery; tests substitute a local inert process.
enum VoiceRecovery {
    static func foreground(process: pid_t, timeout: TimeInterval = 1,
                           activate: () -> Bool, frontmostPID: () -> pid_t?,
                           now: () -> TimeInterval, pause: () -> Void) -> Bool {
        if frontmostPID() == process { return true }
        guard activate() else { return false }
        let deadline = now() + timeout
        while now() < deadline {
            if frontmostPID() == process { return true }
            pause()
        }
        return frontmostPID() == process
    }

    static func microphone(helper: String, target: String, timeout: TimeInterval = 8) -> String? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: helper)
        process.arguments = [target]
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do { try process.run() }
        catch { return "MICROPHONE_RESTORE_FAILED" }
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        while process.isRunning && ProcessInfo.processInfo.systemUptime < deadline {
            Thread.sleep(forTimeInterval: 0.025)
        }
        if !process.isRunning {
            return process.terminationStatus == 0 ? nil : "MICROPHONE_RESTORE_FAILED"
        }
        process.terminate()
        let grace = ProcessInfo.processInfo.systemUptime + 0.5
        while process.isRunning && ProcessInfo.processInfo.systemUptime < grace {
            Thread.sleep(forTimeInterval: 0.025)
        }
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
        return "MICROPHONE_RESTORE_TIMEOUT"
    }

    static func report(path: String, session: String, errors: [String]) {
        guard !path.isEmpty else { return }
        // Deliberately excludes text, audio, microphone names, and credentials.
        let value: [String: Any] = ["session": session,
            "event": errors.isEmpty ? "watchdog_restored" : "watchdog_restore_failed",
            "codes": errors, "time": Date().timeIntervalSince1970]
        guard let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]) else { return }
        let url = URL(fileURLWithPath: path)
        do {
            try data.write(to: url, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: path)
        } catch { /* Restoration itself must not depend on writing diagnostics. */ }
    }
}
