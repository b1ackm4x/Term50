# Native quick controls validation

This is a historical 2026-09-19 validation snapshot. The voice connection
backend was added later; see the [voice guide](voice-input.md) and
[development validation](voice-development-validation.md) for its current scope.

On 2026-09-19 the native floating entry, three input profiles and Chinese settings
pages were built for QNX/ARM and exercised on the USB-connected BlackBerry Q20.

Host checks: 12 chord, 714 modal, 4 real client/control round trips, and 234 UI,
settings and alpha-compositing checks passed; ASan/UBSan passed as well.

Device checks: 36 UI checks, 12 real PTY byte checks, configuration staging and
application restart checks passed. User Lua configuration hashes were unchanged.
The test adapter injected touch/key events through the actual app event handler;
this does not establish physical key timing, hand-held drag feel or long-duration
performance. The production BAR excludes the adapter and temporary runner.

At that date, the remote-input backend was not implemented. Connection settings
could be saved, but Test explicitly reported that no connection was attempted.

The full Chinese report and native screenshots remain in the local workspace at
`quick-ui-validation/RESULTS.md`, outside this repository. The historical BAR at
`dist/Term50-quick-controls-bbtailscale.bar` is local and not included in the
source checkout.
