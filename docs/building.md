# Building Term50

Run commands from the Term50 repository root. A fresh checkout contains the
application source, but not the generated Ghostty archive, ARM/QNX OpenSSL
runtime, bbnix userland, fonts, or Fen binary. These inputs must be prepared or
supplied before packaging a complete BAR.

## Host checks

Initialize the pinned Ghostty and Lua submodules, then run the tests without a
BB10 toolchain or device:

```sh
git submodule update --init --recursive
make test check-reference
make test-voice
```

The host tests need a C compiler, `pkg-config`, ICU development headers and
libraries, and the Lua submodule. Voice tests also need OpenSSL development
headers/libraries (`pkg-config openssl`) and Python 3.13+ built with TLS-PSK
support. If the default `python3` lacks
`ssl.SSLContext.set_psk_server_callback`, use
`VOICE_TEST_PYTHON=/absolute/path/to/python3 make test-voice`. On macOS,
`make test-voice` also runs the Swift recovery and microphone-route tests when
`swiftc` is available. These tests use fake audio and a local server; they do
not test Q20 audio capture or Doubao recognition.

## BB10 binaries with BerryCore GCC 9

This is the build route validated for the current voice code. Use a Linux/amd64
container with the BerryCore BB10/QNX toolchain mounted at `/root/qnx800`, this
checkout mounted in the container, and an x86-64 Linux Zig 0.15.2 executable.
The container also needs Make, Git, `patch`, Python 3, full Perl (not just
`perl-base`), CA certificates, `libc6-i386`, `lib32stdc++6`, and `file`;
packaging additionally needs `unzip`, the SDK's
`blackberry-nativepackager`, and its Java runtime. The toolchain, Zig, and the
runtime libraries are external prerequisites, not files supplied by this
checkout.

Provide a directory containing the exact pinned ARM/QNX `libssl.so.3` and
`libcrypto.so.3` binaries. The build helper verifies their hashes against
`vendor/voice-openssl/pins.json` and generates matching public headers. It may
download and verify the official OpenSSL source during that preparation. See
the [voice TLS dependency](../vendor/voice-openssl/README.md) for provenance
and generated-file details.

Inside the container, from the mounted Term50 root:

```sh
. /root/qnx800/env.sh
export ZIG=/absolute/path/to/zig-0.15.2/zig
export TERM50_VOICE_OPENSSL_RUNTIME_DIR=/absolute/path/to/pinned-arm-qnx-libs
bash vendor/libghostty-vt/scripts/cross-build.sh lib
tools/build-berrycore.sh
file Device-Debug/Term50 Device-Debug/termctl
```

The `file` output should identify both binaries as 32-bit ARM QNX ELFs. The
Ghostty script resets and cleans its pinned submodule before applying the
freestanding patches; save changes in that submodule first. The build helper
prepares OpenSSL, checks the Lua reference version, and builds both executables.
It does not assemble a BAR or test the physical device. See the
[historical BerryCore build record](modal-input-validation.md#bb10-build) for
the original toolchain validation.

### Package with an existing base BAR

`tools/package-berrycore.py` reuses bbnix, fonts, and Fen from a *separate*,
previously validated Term50 v1.1.0 base BAR, such as the
[upstream v1.1.0 release](https://github.com/acmiyaguchi/Term50/releases/tag/v1.1.0).
It accepts only the SHA-256 hashes listed in the script; a fresh checkout does
not contain the base BAR. With the base BAR accessible inside the same Linux
container:

```sh
python3 tools/package-berrycore.py \
  --base-bar /absolute/path/to/verified-v1.1.0-base.bar \
  --output Term50-local.bar
unzip -t dist/Term50-local.bar
```

The output filename must be a basename ending in `.bar`; the script writes it
under `dist/`, together with `.validation.json` and `.sha256` reports. It checks
the base and output manifests and verifies reused assets byte for byte. The
package is unsigned and is not installed by the script. The optional
`--container NAME --container-root /workspace/Term50` form runs the packaging
step in an existing container from a macOS host while keeping case-sensitive
terminfo extraction on Linux. The older `tools/package-voice.py` instead
requires a previously validated CJK/Oh My Zsh BAR and creates a separate voice
candidate; it is not a fresh-checkout packaging route.

## BlackBerry NDK and Nix route

The older Makefile route requires an NDK shell with `qcc`, `QNX_TARGET`,
`blackberry-nativepackager`, and Nix. Prepare the pinned Ghostty library with
[its build instructions](../vendor/libghostty-vt/README.md). In the BerryCore
container, run
`python3 tools/prepare-voice-openssl.py --runtime-dir /absolute/path/to/pinned-arm-qnx-libs`
to prepare the matching ARM OpenSSL runtime and headers *before* invoking
`make` in the NDK shell. Then:

```sh
export BBNIX_SYSROOT=/absolute/path/to/bbndk-linux
make
make package-dev
```

`make` builds `Device-Debug/Term50` and `Device-Debug/termctl`. `package-dev`
also builds and stages bbnix, fonts, and Fen through Nix, then writes
`Term50.bar`. `make package-release` uses the same Nix inputs and creates an
unsigned, non-development-mode BAR. This NDK route has not been revalidated
with the current voice code; the BerryCore route above is the verified binary
build. Switching toolchains in one checkout requires `make clean` first because
compiler flags are not included in every object dependency; separate checkouts
are safer if both toolchains are in active use.

`make deploy` packages, installs, and launches on a device in Development Mode;
`make connect` opens a device connection. Both need the current device address
and Development Mode password in a local, ignored `.env` copied from
[`.env.example`](../.env.example). Keep that file and all voice pairing material
out of source control. Building and packaging do not establish device behavior:
the [voice validation record](voice-development-validation.md) distinguishes
host tests, Q20 checks, and actual recognition. Mac Companion setup is in its
[own guide](../companion/README.md).
