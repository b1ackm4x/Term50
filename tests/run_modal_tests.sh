#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build=Device-Debug/modal-tests${TEST_SANITIZERS:+-sanitized}
mkdir -p "$build"
cc=${TEST_CC:-cc}
icu=${TEST_ICU_CFLAGS:-}
if [ -z "$icu" ]; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists icu-uc; then
        icu=$(pkg-config --cflags icu-uc)
    elif [ -d /opt/homebrew/opt/icu4c/include ]; then
        icu=-I/opt/homebrew/opt/icu4c/include
    fi
fi
flags="-std=gnu99 -g -Wall -Wextra -Wno-missing-field-initializers -Wno-comment $icu ${TEST_SANITIZERS:-}"
# Lua itself has no platform services; use the exact pinned interpreter sources.
for source in vendor/lua/*.c; do
    case "$source" in */lua.c|*/onelua.c|*/ltests.c) continue;; esac
    obj="$build/$(basename "$source" .c).o"
    if [ ! -f "$obj" ] || [ "$source" -nt "$obj" ]; then
        $cc $flags -DLUA_USE_POSIX -Ivendor/lua -c "$source" -o "$obj"
    fi
done
$cc $flags -Itests/compat -Isrc -Ivendor/lua \
    tests/test_modal_input.c tests/test_modal_control.c tests/modal_stubs.c src/control_proto.c \
    src/modal_input.c src/session.c src/app.c src/action.c src/chord_match.c \
    src/preferences.c src/symmenu.c src/accent_menus.c "$build"/*.o -lm -o "$build/test_modal_input"
$cc -std=gnu99 -Wall -Wextra -Isrc tools/termctl/main.c src/control_proto.c -o "$build/termctl-host"
if ! TERM50_TEST_EXAMPLE="$PWD/share/term.lua.modal-example" "$build/test_modal_input" "$PWD/$build/termctl-host" "$@" 2> "$build/stderr.log"; then
    cat "$build/stderr.log" >&2
    exit 1
fi
