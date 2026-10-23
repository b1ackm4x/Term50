#!/usr/bin/env python3
"""Stream 48 kHz stereo s16le PCM from stdin into Doubao IME via Loopback."""
import argparse
import json
import os
import pathlib
import select
import subprocess
import sys
import time
import uuid
import fcntl
import sounddevice as sd

ROOT = pathlib.Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"

def command(action):
    ident = str(uuid.uuid4())
    tmp = RUNTIME / "control.tmp"
    tmp.write_text(json.dumps({"id": ident, "action": action}))
    tmp.replace(RUNTIME / "control.json")
    return ident

def events():
    try:
        return [json.loads(line) for line in (RUNTIME / "events.jsonl").read_text().splitlines() if line]
    except (FileNotFoundError, json.JSONDecodeError):
        return []

def wait_event(kind, ident, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for event in reversed(events()):
            if event["event"] == "error" and event.get("id") == ident:
                raise RuntimeError(event["message"])
            if event["event"] == kind and event.get("id") == ident:
                return event
        time.sleep(0.05)
    raise TimeoutError(f"Receiver did not return {kind}")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pcm", type=pathlib.Path, help="PCM file; omit to read stdin")
    parser.add_argument("--device", default="Doubao Stream Bridge")
    parser.add_argument("--settle", type=float, default=4, help="Seconds without text changes before final output")
    parser.add_argument("--timeout", type=float, default=25, help="Final result timeout")
    parser.add_argument("--max-seconds", type=float, default=120, help="Maximum duration of one input session")
    parser.add_argument("--output", type=pathlib.Path, help="Also save complete result JSON")
    parser.add_argument("--manual-microphone", action="store_true", help="Do not automatically select and restore the Doubao microphone")
    args = parser.parse_args()
    RUNTIME.mkdir(exist_ok=True)
    lock = (RUNTIME / "session.lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    device = next((i for i,d in enumerate(sd.query_devices()) if d["name"] == args.device and d["max_output_channels"] >= 2), None)
    if device is None:
        raise RuntimeError(f"Virtual output device not found: {args.device}")
    binary = ROOT / "build/DoubaoStreamReceiver.app/Contents/MacOS/DoubaoStreamReceiver"
    if not binary.exists():
        raise RuntimeError("Run ./build.sh first")
    # Reuse an existing receiver only if it actually responds.
    try:
        wait_event("snapshot", command("snapshot"), timeout=1)
    except TimeoutError:
        log = (RUNTIME / "receiver.log").open("ab")
        subprocess.Popen([str(binary), str(RUNTIME)], stdout=log, stderr=log, start_new_session=True)
        wait_event("snapshot", command("snapshot"), timeout=10)
    started = time.time()
    began = False
    succeeded = False
    previous_mic = None
    try:
        if not args.manual_microphone:
            mic = subprocess.run([str(ROOT / "build/mic-route"), args.device], text=True, capture_output=True, check=True)
            previous_mic = json.loads(mic.stdout)["previous"]
        began = True
        wait_event("ready", command("start"))
        audio_started = time.time()
        source = args.pcm.open("rb") if args.pcm else sys.stdin.buffer
        total_bytes = 0
        pending = bytearray()
        eof = False
        underruns = 0
        with sd.RawOutputStream(device=device, samplerate=48000, channels=2, dtype="int16", blocksize=960) as out:
            while not eof:
                if time.time() - audio_started > args.max_seconds:
                    raise TimeoutError("Input exceeded --max-seconds")
                # Nonblocking reads permit a live pipe with irregular arrival intervals.
                readable, _, _ = select.select([source], [], [], 0.02)
                if readable:
                    chunk = os.read(source.fileno(), 3840 - len(pending))
                    if not chunk:
                        eof = True
                    else:
                        pending.extend(chunk)
                if len(pending) == 3840 or eof:
                    if len(pending) % 4:
                        raise ValueError("PCM stream ended on an incomplete stereo frame")
                    if pending:
                        total_bytes += len(pending)
                        underruns += bool(out.write(bytes(pending)))
                        pending.clear()
                elif not readable:
                    # Supply silence while waiting for live audio.
                    underruns += bool(out.write(bytes(3840)))
            out.write(bytes(48000 * 4 // 2))  # Drain trailing speech before ending recognition.
        if args.pcm:
            source.close()
        audio_finished = time.time()
        wait_event("stopping", command("stop"))
        deadline = time.monotonic() + args.timeout
        last_text = ""
        stable_since = time.monotonic()
        while time.monotonic() < deadline:
            snap = wait_event("snapshot", command("snapshot"))
            current = snap["text"]
            if current != last_text:
                last_text = current
                stable_since = time.monotonic()
            if current and not snap["marked"] and time.monotonic() - stable_since >= args.settle:
                result = {"text": current, "audio_seconds": total_bytes / 192000,
                          "elapsed_seconds": round(time.time()-started, 3),
                          "device": args.device, "audio_started": audio_started,
                          "audio_finished": audio_finished, "output_underruns": underruns,
                          "completion": "unmarked_text_stable", "settle_seconds": args.settle}
                if args.output:
                    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
                succeeded = True
                print(json.dumps(result, ensure_ascii=False), flush=True)
                return
            time.sleep(0.2)
        raise TimeoutError("No stable final text received from Doubao IME")
    finally:
        if began:
            try:
                if succeeded:
                    wait_event("stopping", command("stop"), 3)
                else:
                    wait_event("cancelled", command("cancel"), 3)
                wait_event("restored", command("restore"), 3)
            except TimeoutError:
                print("Warning: receiver did not acknowledge cleanup", file=sys.stderr)
        if previous_mic:
            restored = subprocess.run([str(ROOT / "build/mic-route"), previous_mic], text=True, capture_output=True)
            if restored.returncode:
                print("Warning: restore Doubao microphone manually: " + previous_mic, file=sys.stderr)

if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        print(f"Transcription failed: {exc}", file=sys.stderr)
        sys.exit(1)
