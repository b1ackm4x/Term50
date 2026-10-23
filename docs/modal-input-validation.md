# Modal input implementation and validation

Date: 2026-09-19. Baseline: `acmiyaguchi/Term50` commit
`086eb59df482b3c5473a709cd1d86e4949fde1cc`.

The parent `bb10_term` directory is not a Term50 repository; `q-term` initially
contained only `prd.md`. The matching upstream was cloned into `q-term/Term50`.
The separate terminal prototype and older device bundles were not modified.

## Changes for review

- `src/modal_input.[ch]`: top-level modal gate, NORMAL chord/binding lookup,
  one-shot hold handling and repeat suppression through release. Physical-key
  suppression is an 8 KiB bitmap, so virtual keyboards omitting key-up cannot
  exhaust a fixed slot list. No heap allocation or pointers into preferences.
- `src/term_types.h`, `src/session.[ch]`, `src/app.c`: per-session mode and
  getter/setter, INSERT default, new-tab defaults, preservation across tab
  switches/reload. Closing an earlier tab now preserves active-tab identity.
- `src/action.[ch]`, `src/main.c`, `src/terminal.h`: three generic mode actions,
  shared Lua/control dispatch, NORMAL guard before the old keyboard pipeline,
  N/I badge and live help. Terminfo writes now use the requested session and
  propagate write failure. The INSERT byte-encoding pipeline is retained.
- `src/types.h`, `src/preferences.c`: merged `normal_keys`, optional labels and
  success-only `next_mode`, validated default/hold settings, reload/destruction
  and Lua serialization. Default bindings are only `i` and `?`; empty tables
  merge, `false` unbinds. Invalid tables fall back to compiled defaults.
- `src/accent_menus.c`: fix the existing `t` accent row length (2 entries, not
  3), exposed by the new sanitizer coverage of the real preferences loader.
- `src/control.c`, `README.md`, `share/AGENTS.md`, `share/term.lua.reference`,
  `share/term.lua.modal-example`, `bar-descriptor.xml`: help/docs/example and
  package asset. Existing reference keycode numbers were corrected to actual
  BB10 keysyms. Product-specific command strings exist only in the Lua example.
- `Makefile`, `tests/run_modal_tests.sh`, `tests/test_modal_input.c`,
  `tests/test_modal_control.c`, `tests/modal_stubs.c`, `tests/compat/**`:
  host regression tests using production session/app/action/preferences/Lua and
  modal routing; Unix pipes observe bytes and a real termctl client exercises
  the real control dispatcher over a Unix socket. Only device services are
  stubbed.
- `tools/build-berrycore.sh`: reproducible alternative build for the locally
  available BerryCore GCC 9.3 toolchain; upstream qcc build flags remain intact.
- `tools/package-berrycore.py`: complete BAR packaging using the existing SDK
  and hash-pinned release runtime resources, with every output asset verified.

## Input flow

1. Existing help/URL overlays consume their own input. Help dismissal consumes
   its entire physical key gesture.
2. The modal gate handles key-up/repeat suppression, then resolves global
   chords. INSERT intercepts only explicitly configured mode chords; all other
   events continue through the existing input pipeline, including real Esc.
3. NORMAL resolves configured hold-to-insert, modifier prefixes/Alt glyphs,
   then `normal_keys`. A matching raw text binding writes exactly its bytes;
   other targets use Action dispatch. Enter/Esc, editing/navigation keys and
   ASCII Ctrl combinations write directly to the terminal and keep NORMAL;
   explicit chords take priority. Unbound printable/non-ASCII input and
   repeats remain consumed before metamode/keyhold/fallback. Latched Q20 Ctrl
   state is consumed once, including the Shift -> Alt -> c sequence.
4. A successful binding's `next_mode` applies to its originating tab, resolved
   again by stable ID after dispatch. A closed origin is ignored. Failed or
   partial writes do not change mode or retry the command.
5. Mode actions and consumed modal presses dirty the full frame. Tab selection
   already dirties the frame; rendering reads the active session's mode.
   Reload rebuilds help and font surfaces without changing existing modes.

See `share/term.lua.modal-example` for the optional complete configuration.
It overrides Shift+Sym with mode_toggle and supplies Alt+Sym for a real Esc.
Compiled defaults keep INSERT and the previous Shift+Sym metamode binding.

## Host validation

```sh
make test check-reference
TEST_SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer' make test
```

Results after the operational-key correction: 12 original chord checks,
683 modal checks, and four real client/server
control round trips passed. AddressSanitizer and UndefinedBehaviorSanitizer also
passed. Reference version 11 matches the compiled version. `git diff --check`
passed.

Coverage includes every unbound printable ASCII character, non-ASCII/special
keys, exact text/Ctrl-C/CR, action failure, both next modes, independent tabs,
closing/switching during dispatch, long hold and repeats, custom/disabled hold,
missing key-up events, held/sticky mode chords, Lua term.action, hot reload,
syntax/runtime reload failures, labels/unbind/re-add, actual example parsing,
explicit Esc binding, Lua serialization round trip and the production
mode/tab change detector feeding the full-repaint gate.

Logs: `Device-Debug/test-results.log`, `Device-Debug/modal-sanitizer.log`.
Expected malformed-config diagnostics are retained in
`Device-Debug/modal-tests[-sanitized]/stderr.log`.

## BB10 build

The available toolchain is BerryCore GCC 9.3.0 with BB10/QNX headers/libraries,
not the upstream qcc 4.6.3 + Nix development environment. A Linux/amd64 Ubuntu
22.04 container was used with this workspace mounted at `/workspace`, and the
toolchain linked to `/root/qnx800`. Container prerequisites: `make`, `git`,
`patch`, `libc6-i386`, `lib32stdc++6`, CA certificates, Python 3 and Java.

Pinned Ghostty and Lua submodules were initialized. Ghostty was built using
Zig 0.15.2 for `arm-freestanding-eabi`, Cortex-A9, ReleaseSmall and the existing
seven upstream freestanding patches:

```sh
# Inside the Linux container, from /workspace/q-term/Term50:
. /root/qnx800/env.sh
export ZIG=/workspace/.build/modal-tools/zig-x86_64-linux-0.15.2/zig
bash vendor/libghostty-vt/scripts/cross-build.sh lib
tools/build-berrycore.sh
```

The Ghostty build produced `vendor/libghostty-vt/build/ghostty/lib/libghostty-vt.a`.
Its optional nm diagnostic initially lacked the requested binary; the archive
was subsequently inspected with BerryCore's ARM nm. The macOS Zig attempt
failed in host linking, so the successful archive was built in Linux.

`tools/build-berrycore.sh` successfully compiled and linked both:

- `Device-Debug/Term50`
- `Device-Debug/termctl`

Both were verified as 32-bit little-endian ARM EABI5 QNX ELF executables with
interpreter `/usr/lib/ldqnx.so.2`. The local toolchain's stdint.h hides limit
macros in C99 mode, so this alternative build uses GNU11; app code stays C99
compatible and host tests use GNU99. `-rpath-link` resolves existing transitive
ICU/png/zlib libraries. The legacy linker warns that `-z relro` is ignored.

Build log: `Device-Debug/bb10-full-build.log`. Artifact sizes and SHA-256 values
are recorded separately in `Device-Debug/modal-artifacts.txt`.

## Remaining validation

- Physical Screen/BPS keydown/repeat/keyup delivery and hardware/VKB timing
  still need manual acceptance. The actual Q20 application, PTY, renderer,
  N/I indicators and help layout were exercised on-device as documented below;
  the automated key tests entered at the application event handler.
- The badge reuses the right-edge overlay area, with space reserved in the
  existing tab strip when visible. Single-tab display uses one top-right cell
  as existing indicators do; it does not change rows/cols. Whether that small
  overlay obscures important content in a particular TUI still needs a check
  in that TUI. The 720x720 test screen and help layout fit on-device. Long help
  lists are summarized rather than made scrollable.
- Host tests stub Ghostty/rendering and platform IO; they prove modal routing,
  state, Lua and control behavior, not a complete BB10 UI execution.
- Hold uses first repeat, without a configurable millisecond timer. Multi-key
  sequences, counts, agent detection and other PRD non-goals remain excluded.

## Restored local packaging chain (2026-09-19)

The previous report that the packaging environment was missing was incomplete.
The existing `bb10-tailscale-build` Ubuntu 22.04 container still has the SDK,
Java and `blackberry-nativepackager` used for QClip on 2026-09-18. It mounts this
workspace at `/workspace`. Its toolchain resolves to
`.tmp/qnx8/gcc-9.3.0-berrycore`. The container was restarted, Python 3 added,
and the new Term50/termctl build successfully rerun in that environment.
An additional forced rebuild compiled 57 application/Lua/QR/client C units,
reusing only the already-built pinned Ghostty archive. Both resulting ELF
files were byte-identical to the executables verified inside the new BAR.

`make package-dev` selects the upstream Nix dependency build and requires
`BBNIX_SYSROOT`. That is a different entry point from the established local
BerryCore packaging chain. Neither Nix nor a full third-party rebuild is
necessary to package these application-only changes.

The local upstream release BAR corresponds exactly to this source baseline:
`v1.1.0^{}` resolves to `086eb59df482b3c5473a709cd1d86e4949fde1cc`, and its
embedded descriptor is byte-identical to the baseline descriptor. The new
packaging script accepts only explicitly pinned hashes for this release,
the previously validated SSH-compatible variant, or the existing variant
with the optional bbtailscale addon. This preserves a chosen runtime instead
of silently rebuilding or dropping its fixes. These third-party resources
are reused, not rebuilt from source by this script.

The delivered package uses `Term50-v1.1.0-ssh-compat.bar`, SHA-256
`4bf58bd3c3744570dd220b04e00679a707ff7eb031eb0745fab4e281302514ff`.
It reuses only bbnix, fonts and Fen, including the SSH compatibility wrapper.
Term50, termctl, documentation, Lua reference and modal example are taken from
this working tree. Runtime extraction happens in Linux-local `/tmp`:
extracting terminfo onto default macOS APFS loses case-distinct entries such
as `2621a` and `2621A`. The output verifier caught this during development;
the corrected package retains every original runtime filename and byte.

Reproduce from the macOS host, with this repository as the current directory:

```sh
docker start bb10-tailscale-build
docker exec bb10-tailscale-build bash -lc \
  'source /root/qnx800/env.sh && cd /workspace/q-term/Term50 && tools/build-berrycore.sh'
python3 tools/package-berrycore.py \
  --container bb10-tailscale-build \
  --base-bar /absolute/path/to/Term50-v1.1.0-ssh-compat.bar
```

The original base BAR remains under the earlier project's `outputs` folder.
For a deployment that must retain the separate bbtailscale addon, pass the
existing `bb10_term/qnx-tailscale/Term50-bbtailscale-qnx.bar` instead; it is also
hash-pinned. Do not uninstall the existing app as part of an upgrade.

Verified deliverables:

- `dist/Term50-modal-input.bar`: 18,658,304 bytes, 4,162 archive entries.
- SHA-256: `eab2cb11a2365cb033a42960a9ca9c6b7f37ff2a1e3a379c0b65b753c97d73c5`.
- All manifest SHA-512 digests and the complete manifest/payload list match.
- All 4,146 reused runtime assets match the pinned input byte-for-byte.
- Both packaged executables and every current source asset match this build.
- The new executable has the same nine shared-library dependencies as the
  known release, including `libfreetype.so.1`, `libicuuc.so.49` and `libc.so.3`.
- Application identity is unchanged (`io.github.term50`,
  `testRel_thub_term50b96c21aa`); development mode is false. This is an unsigned
  local BAR for the existing AutoRoot workflow, not a signed store release.

Build/package logs: `Device-Debug/bb10-restored-build.log` and
`Device-Debug/bb10-restored-package.log`. Machine-readable evidence and checksum
are beside the BAR as `.validation.json` and `.sha256`.

The phone's USB installer at `<then-phone-USB-address>:443` initially returned a password
challenge to a standalone HTTP probe. A subsequent live check through the
existing local installation bridge (`127.0.0.1:18749`) in the in-app browser
automatically logged in using its existing saved login. The authenticated
application list loaded successfully, including `io.github.term50` version
`1.1.0.3`; no password entry was needed. A standalone challenge was therefore
not an installation authentication blocker. The new application was installed
and launched for the on-device validation below.


## Initial on-device validation (2026-09-19, before the operational-key correction)

Target: the USB-connected BlackBerry Classic/Q20, QNX 8.0.0,
`MSM8960_V3.2.1.1_F_CLASSICNA_Rev:11`, with a 720x720 display.
The application identity/version remains `io.github.term50` / `1.1.0.3`.
Installation used the existing authenticated USB installer without uninstalling
or clearing application data.

A temporary diagnostic build added a narrowly scoped `termctl device-test`
adapter which calls the production `app_handle_event` entry point. It used the
real ARM app, Lua/actions, tabs, PTY, Ghostty and Screen renderer. A native QNX
raw-terminal recorder captured actual PTY input bytes with echo and signal
interpretation disabled. This is on-device integration coverage; it does not
simulate the physical keyboard's Screen/BPS delivery or repeat timing. The
OS denied a separate Screen input-provider context, so no broader permission
or root input-injection mechanism was introduced.

All 37 checks passed:

- Existing configuration defaults to INSERT; NORMAL drops unbound ASCII,
  special and Unicode events before writing any PTY bytes.
- `i` enters INSERT without writing `i`; INSERT sends exact ASCII/Space/Esc.
- The first held-Space repeat enters INSERT; the same gesture sends no Space,
  Backspace or later repeats, and a fresh INSERT Space produces one byte.
- Shift+Sym mode switching, sticky Shift then Sym, and the example's sticky
  Alt then Sym Esc produce the expected mode/bytes without leaked modifiers.
- NORMAL text bindings emit exact CR/Ctrl-C, successful prefill enters INSERT,
  a failed Lua action remains NORMAL, and Lua mode actions work.
- Tabs retain separate modes; reload preserves existing modes and affects new
  tab defaults. Closing an earlier tab preserves the surviving tab's mode.
- Help opens and its dismissal gesture is consumed. Actual window captures
  confirm N/I updates and the full help panel fitting in 720x720.
- The original `.term.lua` was restored and its SHA-256 compared successfully.
  Test-created tabs were closed; the remaining tab returned to INSERT.

Evidence under `q-term/device-validation/`: `suite-results.txt` (37 PASS lines),
`normal-screen.png`, `help-screen.png`, `final-screen.png`, and `suite.sh`.
The final cleanup job returned exit 0 after checking that the temporary config
backup was absent and deleting/checking absence of its own sandbox test folder.

The production package for this phone is
`dist/Term50-modal-input-bbtailscale.bar` (18,881,969 bytes; 4,163 entries),
SHA-256 `6824d1f72960a1d164808207ce70127bfb776207a34fdb96f9d67a539edab159`.
It preserves both the existing SSH wrapper and bbtailscale addon. Its Term50
executable matches the production build byte-for-byte; the test adapter,
collector address and test assets/token are absent. The diagnostic BAR is
strictly temporary and is not a deliverable.

The production BAR was then installed successfully, launched separately as PID
34345127, and independently queried with `Is Running` returning `true`.
The temporary diagnostic BAR/token were deleted locally, and both temporary
USB bridge/collector services were stopped. The production app remains running.
Detailed evidence: `q-term/device-validation/RESULTS.md` and
`production-install.json`. Physical keyboard acceptance remains pending.


## Operational keys in NORMAL (2026-09-19 follow-up)

The initial gate treated terminal operation keys as unbound input and swallowed
Enter/Escape. It also omitted the legacy Ctrl latch that Q20's Shift -> Alt
chord creates, so the physical Ctrl-C gesture could not interrupt from NORMAL.

NORMAL now handles Enter/keypad Enter, Escape, Tab/Back-tab, Backspace/Delete,
Insert, physical arrows, Home/End/Page Up/Page Down and ASCII Ctrl combinations
directly at the modal gate. These keys keep NORMAL and do not enter the INSERT
metamode/hold pipeline. Explicit configured chords still take priority. The
latched modifier snapshot participates in NORMAL resolution and is cleared
once after consumption, without clearing a newly manufactured Ctrl prefix.
Unbound printable text is still ignored and held-key repeats remain suppressed.

The target phone retains its user-configured IJKL arrow bindings and Claude
prompt shortcuts. The application update does not rewrite `.term.lua`.

New production package: `dist/Term50-modal-control-keys-bbtailscale.bar`.
SHA-256: `62f4e3031b05b419d026333462b819f72fb6ba000812155bc2b71418100e78b4`.
The previous production BAR remains available for rollback. Both runtime
variants preserve the pinned SSH compatibility wrapper and bbtailscale addon.

Current host regression logs: `Device-Debug/control-keys-host.log` and
`Device-Debug/control-keys-sanitizers.log`. Build/package logs use the same
`control-keys-` prefix. Device evidence is under `q-term/control-key-fix/`.

The follow-up on-device suite passed all 36 checks. Enter actually launched a
foreground shell script from NORMAL, and Q20 Shift -> Alt -> c interrupted it
and triggered its INT handler. Raw PTY captures verified Enter/Esc/Ctrl and
editing/navigation bytes, existing IJKL/Claude bindings, one-shot Ctrl cleanup,
and unchanged `.term.lua` SHA-256. These keys were delivered through the
application-event test adapter; no manual physical-keyboard claim is made.
The temporary sandbox data and diagnostic credentials/package were removed.
See `q-term/control-key-fix/RESULTS.md` and `device-results.txt`.

The corrected production BAR was installed successfully, launched as PID
40779943, and independently queried as running. The temporary USB bridge
was stopped; the production app remains running with the unchanged user config.
