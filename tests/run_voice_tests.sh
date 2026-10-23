#!/bin/sh
# No real microphone, desktop automation, or external ASR is used.
set -eu
cd "$(dirname "$0")/.."
build=Device-Debug/voice-tests
mkdir -p "$build"
python=${VOICE_TEST_PYTHON:-python3}
# Skipping every TLS test must not look like successful voice validation.
"$python" -c 'import ssl, sys; sys.exit(0 if hasattr(ssl.SSLContext, "set_psk_server_callback") else "Voice tests require Python 3.13+ with TLS-PSK; set VOICE_TEST_PYTHON to that interpreter.")'
cc=${TEST_CC:-cc}
icu=${TEST_ICU_CFLAGS:-}
icu_libs=${TEST_ICU_LIBS:-}
if [ -z "$icu" ]; then
    if pkg-config --exists icu-uc 2>/dev/null; then icu=$(pkg-config --cflags icu-uc)
    elif [ -d /opt/homebrew/opt/icu4c/include ]; then icu=-I/opt/homebrew/opt/icu4c/include
    fi
fi
if [ -z "$icu_libs" ]; then
    if pkg-config --exists icu-uc 2>/dev/null; then icu_libs=$(pkg-config --libs icu-uc)
    elif [ -d /opt/homebrew/opt/icu4c/lib ]; then icu_libs='-L/opt/homebrew/opt/icu4c/lib -licuuc'
    else icu_libs=-licuuc
    fi
fi
flags="-std=gnu11 -g -Wall -Wextra ${TEST_SANITIZERS:--fsanitize=address,undefined -fno-omit-frame-pointer}"
$cc $flags $icu -Itests/compat -Isrc tests/test_session_output.c src/session.c -o "$build/session-output"
"$build/session-output"
$cc $flags -D__QNXNTO__ -Itests/voice_audio_fakes -Isrc tests/test_voice_audio.c src/voice_audio.c -o "$build/audio"
"$build/audio"
$cc $flags -Isrc tests/test_voice_shortcut.c src/voice_shortcut.c -o "$build/shortcut"
"$build/shortcut"
"$python" -W error::ResourceWarning -m unittest discover -s companion/tests -v
"$python" tests/test_voice_protocol.py
"$python" tests/test_voice_ui_integration.py
if [ "$(uname -s)" = Darwin ] && command -v swiftc >/dev/null 2>&1; then
    swiftc companion/Recovery.swift companion/tests/recovery/main.swift -o "$build/recovery"
    "$build/recovery"
    swiftc companion/MicRouteController.swift companion/tests/mic-route/main.swift -o "$build/mic-route"
    "$build/mic-route"
fi
if [ -n "${VOICE_TEST_CJK_FONT:-}" ]; then
    $cc $flags $icu -Itests/compat -Isrc $(pkg-config --cflags freetype2) \
        tests/test_voice_painter.c src/quick_ui.c src/ui_settings.c src/ui_painter.c src/font.c src/bitmap.c \
        $(pkg-config --libs freetype2) $icu_libs -lm -o "$build/painter"
    "$build/painter" "$VOICE_TEST_CJK_FONT" "$build/voice-preview.ppm"
fi
