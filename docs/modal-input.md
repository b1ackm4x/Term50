# Modal input

Term50 starts in **INSERT** mode by default. Modal input is optional: enable it
in `~/.term.lua` if you want a **NORMAL** mode for keyboard shortcuts. Each tab
keeps its own mode. The mode indicator uses the existing tab strip and does not
change terminal rows or columns.

## Enable NORMAL mode

Add this example to `~/.term.lua`:

```lua
default_input_mode = "normal"
normal_keys = {
  { "i", "mode_insert" },
  { "h", "tab_prev" },
  { "l", "tab_next" },
  { "?", "help_overlay" },
}
chord_bindings = {
  { key = "sym", mods = { "shift" }, action = "mode_toggle" },
  { key = "sym", mods = { "alt" }, action = "\x1b", label = "Alt+Sym = Esc" },
}
```

In NORMAL mode, press `i` or hold Space to enter INSERT. The hold activates on
the first key repeat, without a timer; Term50 consumes that entire press through
release, even if you switch tabs while holding it. It sends neither Space nor
Backspace. Space in INSERT retains its usual behavior. With the example above,
Shift+Sym toggles modes and Alt+Sym sends Escape. Without that override,
Shift+Sym retains its compiled metamode behavior. Metamode `e` and a physical
Escape key also continue to work.

INSERT keeps the original input pipeline, including metamode, symbol menus,
accent keys and external keyboards. In NORMAL, configured global chords and
`normal_keys` run first; unbound printable text is ignored. Enter, Escape, Tab,
Backspace/Delete, navigation keys and ASCII Ctrl combinations still reach the
terminal without leaving NORMAL. On Q20, Shift then Alt then `c` sends Ctrl-C in
either mode. Explicit chords take priority over those default operations.
Consumed key presses remain suppressed until release.

## Customize bindings

`normal_keys` merges by a single ASCII trigger, like `metamode_keys`. The
compiled defaults are `i` and `?`; `{}` keeps them, while `{ "i", false }`
unbinds `i`. A binding can target an action or raw text and can include a
`label` and `next_mode = "insert"` or `"normal"`. Without `next_mode`, the
action determines the resulting mode.

A successful binding applies `next_mode` to the tab where the key press began,
even if its action switches tabs; a closed origin tab is ignored. Raw text is
written exactly as configured, including control bytes. Failed or short writes
are not retried and do not change modes. A custom Lua action counts as
successful when its function returns without error; raise an error if an
internal failure should prevent `next_mode`.

The [complete Claude Code example](../share/term.lua.modal-example) shows
optional command bindings. These strings are editable examples: Term50 does
not detect agents or validate commands.

## Reload and control

`reload_config` refreshes bindings and help immediately while preserving
existing tabs' modes. A new `default_input_mode` applies only to tabs opened
after the reload. A Lua syntax or runtime load error keeps the current config.
An invalid modal table falls back as a whole to compiled defaults; an unknown
default mode falls back to INSERT.

The help overlay shows the active mode, effective NORMAL bindings and global
chords. Long lists are summarized and point back to `~/.term.lua`.

You can also select a mode through Lua with `term.action("mode_normal")`,
`term.action("mode_insert")` or `term.action("mode_toggle")`, or through
`termctl`:

```sh
termctl action mode_normal
termctl action mode_insert
termctl action mode_toggle
```

## Validation

From the repository root, run `make test check-reference` for host tests. They
require a C compiler, ICU development headers and the pinned Lua submodule.
For memory and undefined-behavior checks, run:

```sh
TEST_SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer' make test
```

Device services are stubbed in host tests. See the [modal-input validation
record](modal-input-validation.md) for build and device results and their
remaining limits.
