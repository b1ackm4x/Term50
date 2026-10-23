# Voice TLS dependency

Term50's voice client requires TLS 1.2 with `PSK-AES256-GCM-SHA384`.
The legacy SDK's OpenSSL 1.0.1i does not implement this cipher. Voice therefore
uses the existing bbnix ARM/QNX OpenSSL 3.5.4 runtime, with its distinct `.so.3`
SONAMEs, rather than the SDK's `.so.2` libraries.

`tools/prepare-voice-openssl.py` verifies the runtime hashes in `pins.json`,
downloads and verifies the official release source, and generates public
headers with the portable 32-bit target and feature flags recorded in
`pins.json`. It does not compile libraries
or copy host-platform headers. The Nix base32 source hash is recorded alongside
its equivalent hexadecimal SHA-256. Generated headers and staged libraries
are checked on subsequent runs and kept out of source control.

Run the preparation tool in the BerryCore container after sourcing its
toolchain environment. Python 3, Make and full Perl (the `perl` package,
not only `perl-base`) are required. `tools/build-berrycore.sh` invokes it automatically.
Set `TERM50_VOICE_OPENSSL_RUNTIME_DIR` when running that build script to point
at a directory containing the exact pinned runtime binaries. Direct calls to
the preparation tool require `--runtime-dir`. Both libraries must be packaged in
`app/native/lib`, which the app's existing startup environment includes in
`LD_LIBRARY_PATH`.

Upstream release: https://github.com/openssl/openssl/releases/tag/openssl-3.5.4

The source and runtime are licensed under Apache-2.0; the preparation step
retains upstream `LICENSE.txt` alongside this file.
