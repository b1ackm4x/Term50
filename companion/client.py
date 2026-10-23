#!/usr/bin/env python3
"""TLS protocol probe / real PCM test client. Never displays the pairing secret."""
import argparse
import asyncio
import contextlib
import json
import pathlib
import ssl
import struct
import sys
import time

from protocol import CIPHER, VoiceError, receive, send
from pairing import read_key


def context_for(key_file):
    key = read_key(key_file)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # TLS-PSK itself authenticates both ends.
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers(CIPHER)
    context.set_psk_client_callback(lambda hint: ("term50", key))
    return context


async def run(args):
    reader, writer = await asyncio.wait_for(asyncio.open_connection(
        args.host, args.port, ssl=context_for(args.key_file), ssl_handshake_timeout=5), 7)
    events = []
    try:
        if args.pcm is None:
            await send(writer, b"P")
            if await receive(reader, 5) != (b"K", b"TERM50/1"):
                raise VoiceError("BAD_HEALTH_REPLY")
            print("Term50 voice companion: authenticated TLS-PSK health check passed")
            return
        await send(writer, b"B")
        kind, payload = await receive(reader, 35)
        if kind == b"E":
            raise VoiceError(payload.decode("ascii"))
        if (kind, payload) != (b"R", b""):
            raise VoiceError("BAD_READY_REPLY")
        start = time.monotonic()

        async def audio():
            count = 0
            with args.pcm.open("rb") as source:
                while chunk := source.read(3840):
                    if len(chunk) % 4:
                        raise VoiceError("BAD_AUDIO")
                    await send(writer, b"A", chunk)
                    count += len(chunk)
                    # Match capture cadence so this tests streaming, not a bulk upload.
                    await asyncio.sleep(max(0, start + count / 192000 - time.monotonic()))
            await send(writer, b"S")

        task = asyncio.create_task(audio())
        try:
            last_revision = 0
            while True:
                kind, payload = await receive(reader, 45 if task.done() else 165)
                if kind == b"E":
                    raise VoiceError(payload.decode("ascii"))
                if kind not in (b"T", b"F") or len(payload) < 4:
                    raise VoiceError("BAD_TEXT_REPLY")
                revision = struct.unpack("!I", payload[:4])[0]
                if revision <= last_revision:
                    raise VoiceError("BAD_REVISION")
                last_revision = revision
                text = payload[4:].decode("utf-8")
                event = dict(event="final" if kind == b"F" else "partial", revision=revision,
                             elapsed_seconds=round(time.monotonic() - start, 3),
                             audio_finished=task.done(), characters=len(text), text=text)
                events.append(event)
                # Transcripts are explicit diagnostic output only with --show-text.
                print(json.dumps(event if args.show_text else {k: v for k, v in event.items() if k != "text"}, ensure_ascii=False), flush=True)
                if kind == b"F":
                    await task
                    break
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
    finally:
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()
        if args.events:
            args.events.parent.mkdir(parents=True, exist_ok=True)
            # Saving recognition text is opt-in; the default server keeps none.
            with args.events.open("w") as output:
                import os
                os.fchmod(output.fileno(), 0o600)
                for item in events:
                    output.write(json.dumps(item, ensure_ascii=False) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45850)
    parser.add_argument("--key-file", type=pathlib.Path, required=True)
    parser.add_argument("--pcm", type=pathlib.Path)
    parser.add_argument("--show-text", action="store_true", help="Print recognized text (public test audio only recommended)")
    parser.add_argument("--events", type=pathlib.Path, help="Explicitly preserve transcript events as private evidence")
    try:
        asyncio.run(run(parser.parse_args()))
        return 0
    except (VoiceError, OSError, RuntimeError, ValueError, TimeoutError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
