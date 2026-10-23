"""Term50 voice protocol v1. All strings are strict UTF-8; text is a snapshot."""
import asyncio
import struct

MAX_FRAME = 65536
MAX_AUDIO = 16384
MAX_TEXT = 16384
MAX_SECONDS = 120
CIPHER = "PSK-AES256-GCM-SHA384"


class VoiceError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__(code)


async def receive(reader, timeout=15):
    try:
        async with asyncio.timeout(timeout):
            size = struct.unpack("!I", await reader.readexactly(4))[0]
            if not 1 <= size <= MAX_FRAME:
                raise VoiceError("BAD_FRAME")
            body = await reader.readexactly(size)
        return body[:1], body[1:]
    except TimeoutError:
        raise VoiceError("CLIENT_TIMEOUT") from None


async def send(writer, kind, payload=b"", timeout=5):
    if len(kind) != 1 or len(payload) + 1 > MAX_FRAME:
        raise VoiceError("BAD_FRAME")
    writer.write(struct.pack("!I", len(payload) + 1) + kind + payload)
    try:
        await asyncio.wait_for(writer.drain(), timeout)
    except TimeoutError:
        raise VoiceError("CLIENT_SLOW") from None


def text_payload(revision, text):
    encoded = text.encode("utf-8", errors="strict")
    if not 0 < revision <= 0xffffffff or len(encoded) > MAX_TEXT:
        raise VoiceError("TEXT_LIMIT")
    # Terminal control bytes may never be recognized as executable input.
    if any((ord(ch) < 32 and ch not in "\n\t") or 127 <= ord(ch) < 160 for ch in text):
        raise VoiceError("TEXT_CONTROL")
    return struct.pack("!I", revision) + encoded
