"""Shared private pairing-key validation. Never include key material in errors."""
import os
import re
import stat


def read_key(path):
    def valid_file(info):
        return stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode) == 0o600

    if not valid_file(path.lstat()):
        raise RuntimeError("Pairing key must be a regular file with permissions 0600")
    # Check the opened descriptor too: a replacement symlink/FIFO must not
    # redirect or block a read between lstat and open.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        if not valid_file(os.fstat(stream.fileno())):
            raise RuntimeError("Pairing key file changed during validation")
        contents = stream.read(131)
    raw = contents.strip()
    if len(contents) > 130 or re.fullmatch(rb"[0-9a-fA-F]{64}", raw) is None:
        raise RuntimeError("Pairing key must contain 64 hexadecimal characters")
    key = bytes.fromhex(raw.decode("ascii"))
    if len(key) != 32:
        raise RuntimeError("Invalid pairing key length")
    return key
