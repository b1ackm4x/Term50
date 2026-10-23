# Term50

Term50 is a terminal emulator for BlackBerry 10 (OS 10.3+). It uses
`libghostty-vt` for terminal state, renders through the BB10 Screen API and
FreeType, and runs a separate shell and scrollback for each tab. Lua configures
keys and terminal actions in `~/.term.lua`.

[Features](#features) · [Voice input](#voice-input) · [Build and test](#build-and-test) · [Documentation](#documentation)

The [upstream releases](https://github.com/acmiyaguchi/Term50/releases) are
separate from this working tree's newer voice and input features.

## Features

- Multiple PTY tabs with a touch tab strip, floating controls, and per-tab
  Terminal, Claude, or tty shortcut profiles.
- Chinese font fallback and two-column character rendering while the selected
  terminal font retains control of Latin text and the cell grid.
- Optional NORMAL/INSERT keyboard modes, configurable Lua bindings, and a
  built-in help overlay. Existing users start in INSERT.
- An in-sandbox `termctl` control socket and a `term://` invoke target for
  automation.
- Zsh history picker and Oh My Zsh integration scripts for the Q20 shell.

See the [Lua configuration reference](share/term.lua.reference) for settings
and the [modal input guide](docs/modal-input.md) for Q20 key behavior.

## Voice input

On a Q20, Term50 captures microphone audio and sends it over TLS-PSK to the
macOS Companion. The Companion feeds the existing Doubao virtual microphone
and returns full text snapshots, including revisions. Term50 shows an editable
draft; confirming inserts it into the original tab without executing it.

The Mac setup needs a logged-in desktop, the external `doubao-stream-bridge`
environment, its virtual audio device, and Doubao. Start with the
[phone usage guide](docs/voice-input.md) and [Mac Companion setup](companion/README.md).
The connection test checks encrypted reachability; real recognition needs a
separate audio test. The [validation record](docs/voice-development-validation.md)
distinguishes host tests, Q20 tests, and real Doubao runs.

## Build and test

From the repository root, initialize the pinned Ghostty and Lua submodules and
run the host checks:

```sh
git submodule update --init --recursive
make test check-reference
make test-voice
```

Host tests need a C compiler, ICU and OpenSSL development libraries,
`pkg-config`, and Python 3.13+ with TLS-PSK for voice tests. They use synthetic
audio and do not access a Q20 microphone or Doubao. See the
[build guide](docs/building.md) for the validated Linux/amd64 BerryCore route,
pinned ARM OpenSSL inputs, packaging, and the older NDK/Nix route. Complete
BAR packaging needs external bbnix/Fen/font assets or a verified base BAR;
building binaries alone does not install to a device.

## Documentation

| Topic | Guide |
| --- | --- |
| Voice on the phone | [Usage, behavior, and validation scope](docs/voice-input.md) |
| Mac voice service | [Companion setup, checks, and protocol](companion/README.md) |
| Native development | [Build, package, and deploy](docs/building.md) |
| Keyboard input | [Modal input](docs/modal-input.md) · [Lua reference](share/term.lua.reference) |
| Terminal rendering | [Chinese support](docs/chinese-support.md) |
| Shell integration | [History picker](docs/history-picker.md) · [Oh My Zsh on Q20](docs/oh-my-zsh-q20.md) |
| Automation | [`termctl` and `term://` reference](share/AGENTS.md) |
| Voice internals | [Development validation](docs/voice-development-validation.md) · [TLS dependency](vendor/voice-openssl/README.md) |

The app is licensed under [Apache 2.0](share/LICENSE). Third-party notices and
licenses remain with their respective components.
