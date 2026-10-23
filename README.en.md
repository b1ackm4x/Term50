# Term50 · BlackBerry Q20 custom edition

**Language:** [简体中文](README.md) · English

This repository is a fork of [acmiyaguchi/Term50](https://github.com/acmiyaguchi/Term50). It focuses on the BlackBerry Classic (Q20): physical-keyboard controls, Chinese text, shell workflows, and voice input. Voice recognition runs through the Doubao input method on a Mac. The phone keeps an editable draft until you choose to insert it into the terminal.

[Screenshots](#screenshots) · [Custom features](#custom-features) · [Install and use](#install-and-use) · [Voice setup](#voice-setup) · [Development and docs](#development-and-docs)

## Screenshots

| Inline voice draft | Chinese terminal | Zsh and history suggestions |
| :---: | :---: | :---: |
| <a href="docs/design/voice-inline-device-ready.png"><img src="docs/design/voice-inline-device-ready.png" alt="Term50 voice draft awaiting confirmation on a Q20" width="220"></a> | <a href="docs/assets/chinese-q20.png"><img src="docs/assets/chinese-q20.png" alt="Simplified and traditional Chinese mixed with Latin text on a Q20" width="220"></a> | <a href="docs/assets/oh-my-zsh-q20.png"><img src="docs/assets/oh-my-zsh-q20.png" alt="Oh My Zsh prompt and history suggestion on a Q20" width="220"></a> |

These are Q20 screenshots. The voice screenshot uses a public simulated transcript; it is not a recording of phone-to-Doubao recognition. The [UI animation demo](docs/design/voice-inline-implementation.md) also uses test text and audio levels.

## Custom features

- **Inline voice input:** Hold `0` on the Q20 to start recording. Full recognition snapshots appear at the terminal cursor. You can revise, edit, or cancel the draft. Confirming inserts it into the original tab **without executing a command**. The phone and Mac Companion communicate over TLS-PSK.
- **Keyboard and floating controls:** Switch between Terminal, Claude, and tty input profiles. The Terminal profile provides navigation, history, paste, and tab shortcuts. An optional NORMAL/INSERT mode can be enabled in `~/.term.lua`.
- **Chinese rendering:** Terminal text falls back to a BB10 system Chinese font when needed and handles double-width cells without changing Latin text in the selected primary font. This does not add a Chinese input method.
- **Shell extensions:** The repository includes a Zsh history picker and Oh My Zsh integration scripts. They were exercised on a configured Q20. A newly built BAR may not contain that same shell environment; follow the respective guides to install the configuration.

## Install and use

**This branch provides source and build scripts, but no ready-to-install custom BAR release.** [Upstream releases](https://github.com/acmiyaguchi/Term50/releases) do not contain these voice and input changes.

1. Clone this branch and its submodules:

   ```sh
   git clone --recursive --branch codex/term50-voice-integration https://github.com/b1ackm4x/Term50.git
   cd Term50
   ```

2. Follow the [build guide](docs/building.md) to prepare the BerryCore/QNX toolchain, the pinned ARM OpenSSL runtime, and a hash-verified base BAR, then build and package the app. A complete BAR needs bbnix, fonts, and Fen assets that are not in this repository. The packaging script produces an unsigned BAR and does not install it.
3. Import `dist/Term50-local.bar` into the application installer in [Sachesi](https://github.com/xsacha/Sachesi), connect the phone, install it, and open Term50. Whether an unsigned native BAR is accepted depends on the device state; do not assume an unmodified stock device will accept it. For a development-mode build, you can configure a local `.env` and use `make deploy` as described in the [build guide](docs/building.md#blackberry-ndk-and-nix-route). That older toolchain route has not been revalidated with the current voice code.

After launch, use the floating button on the right to choose a profile. Direct typing remains available by default. In the Terminal profile, `I/J/K/L` mean up/left/down/right, `R` sends Ctrl+R, `X` sends Ctrl+C, `V` pastes, `H` selects the previous tab, and `N` opens a new tab. The optional selectable history list requires the [Zsh history script](docs/history-picker.md). See the [modal input guide](docs/modal-input.md) and [Lua configuration reference](share/term.lua.reference) for the full key behavior.

## Voice setup

Voice recognition needs a **logged-in, unlocked Mac** with the Doubao input method installed and signed in. This repository includes the [standalone bridge source](companion/doubao-stream-bridge/README.md) and the Term50 Companion, but not a Python virtual environment, the Loopback app, or the Mac's `Doubao Stream Bridge` virtual device. Set up that device using the standalone guide, then run the following from the repository root. `python3` must be version 3.13 or newer with TLS-PSK support:

```sh
python3 -c 'import ssl, sys; assert sys.version_info >= (3, 13) and hasattr(ssl.SSLContext, "set_psk_server_callback")'
export TERM50_DOUBAO_BRIDGE_DIR="$PWD/companion/doubao-stream-bridge"
python3 -m venv "$TERM50_DOUBAO_BRIDGE_DIR/.venv"
"$TERM50_DOUBAO_BRIDGE_DIR/.venv/bin/python" -m pip install -r "$TERM50_DOUBAO_BRIDGE_DIR/requirements.txt"
cd companion
export TERM50_MAC_IP='<reachable Mac IP address>'
zsh build.sh
zsh run.sh --check
zsh run.sh --bind "$TERM50_MAC_IP"
```

On its first real start, the Companion creates a private pairing key at `companion/private/pairing.key`. On the phone, open **Voice input → Mac connection** from the floating button, enter the Mac address, port `45850`, and that key, then enable and save the connection. Do not put the key in the repository or a chat. **Test connection** checks encrypted pairing only; it does not start Doubao recognition.

Back in the terminal, **hold `0` for about 0.6 seconds** to connect and record; recording continues after you release it. Press Enter to stop, wait for finalization, then press Enter again to insert the draft. Insertion does not send a terminal Enter. Press Esc to cancel. Keep the Mac unlocked; its receiving window needs to stay in the foreground during recognition. See the [Mac Companion guide](companion/README.md) and [phone voice guide](docs/voice-input.md) for permissions, background service setup, and troubleshooting.

Validation currently covers Q20 microphone capture through simulated recognition, draft editing and terminal insertion, plus a separate Mac path from public test audio to real Doubao recognition. **The complete phone-microphone-to-real-Doubao path has not yet passed acceptance testing.** See the [voice validation record](docs/voice-development-validation.md).

## Development and docs

After initializing submodules, run the host checks without a BB10 device:

```sh
git submodule update --init --recursive
make test check-reference
make test-voice
```

Host voice tests use synthetic audio. Dependencies, BerryCore builds, and complete packaging are covered in the [build guide](docs/building.md).

| Topic | Start here |
| --- | --- |
| Phone voice input and Mac setup | [Phone voice guide](docs/voice-input.md) · [Companion guide](companion/README.md) |
| Voice UI and validation | [Inline UI implementation](docs/design/voice-inline-implementation.md) · [Validation record](docs/voice-development-validation.md) |
| Q20 keyboard and settings | [Modal input](docs/modal-input.md) · [Lua reference](share/term.lua.reference) |
| Chinese and shell | [Chinese support](docs/chinese-support.md) · [History picker](docs/history-picker.md) · [Oh My Zsh](docs/oh-my-zsh-q20.md) |
| Build and automation | [Build guide](docs/building.md) · [`termctl` / `term://`](share/AGENTS.md) |

This fork retains the [Apache 2.0 license](share/LICENSE). Third-party components retain their own licenses.
