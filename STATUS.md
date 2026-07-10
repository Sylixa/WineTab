# Photoshop 2020 (Wine) tablet pressure fix — status

## Problem chain found & fixed so far

1. Wine's builtin `wintab32.dll` never streams intermediate motion packets —
   only endpoint samples reach the app, so curvy strokes render as straight
   lines (long-standing upstream Wine bug, unrelated to pressure itself).
2. Photoshop actually loads `wintab32.dll` from its own install directory
   (`.../Adobe Photoshop 2020/wintab32.dll`), not `system32` — Windows/Wine
   DLL search order checks the app directory first. A stale **XWinTab v0.2**
   build was already sitting there unused (dormant, no DllOverride existed to
   activate it) since before this investigation.
3. Installed XWinTab (https://github.com/Graham--M/XWinTab) v0.5.0 instead —
   fixes the straight-line bug (proper motion streaming) but pressure was
   still broken — v0.5.0 predates an unmerged upstream fix.
4. Found the actual pressure fix in a fork:
   https://github.com/gurppt/flashcs6linux_deploy (xwintab/src, patches
   documented in xwintab/PATCHES.md) — adds real `WTI_DEVICES` AXIS/pressure
   query handling that base XWinTab lacks.
5. Cross-compiled that patched source ourselves (mingw-w64 + winegcc) since
   no prebuilt binary exists for this fix. Build lives in
   `~/Code/xwintab-photoshop-fix/build64/`.
6. Found and fixed an additional bug in the patched source: `handle_event()`
   in `XWinTabHelper.c` hardcoded x/y to `(0,0)` for ButtonPress/ProximityIn
   events (only `DEVICE_VALUATOR` events carried real coordinates), causing
   every new stroke to start with a straight ray from the canvas origin.
   Fixed by caching last-known real position (`g_lastX`/`g_lastY`) and using
   that instead of `(0, 0)`.

## Current working state

- **Pressure sensitivity: works.**
- **Curve/motion streaming: works.**
- Files changed (persistent, already in place):
  - `/mnt/storage/software/Adobe Photoshop 2020/wintab32.dll` — our patched
    build (backup of the old dormant v0.2 at `wintab32.dll.v0.2.bak` in same
    dir)
  - `/mnt/storage/software/Adobe Photoshop 2020/XWinTabHelper.dll.so` — our
    patched build
  - Registry: `HKCU\Software\Wine\DllOverrides\wintab32 = native` (set in
    prefix `/mnt/storage/software/softwareprefix`, persists)
- **Wine runner must be `wine-8.0-staging-tkg-amd64`** (Lutris runner dir) —
  system default `wine-11.12-staging` crashes Photoshop outright on launch.
  In Lutris: set that game's Wine version dropdown accordingly; no special
  env vars needed for daily use.

## Known remaining bug (being worked on next)

After: Photoshop → switch to a **fullscreen app** → **mouse-click** inside
that fullscreen app → switch back to Photoshop → drawing breaks completely
(both pen and mouse) until Photoshop is restarted.

- Does NOT reproduce if the fullscreen app is only clicked with the pen.
- Confirmed via `xinput test-xi2 --root` (raw XInput2) that raw tablet
  events (position + pressure) keep flowing fine at the X11/Xwayland layer
  even while Photoshop is in the broken state — so this is not an
  Xwayland/compositor-level event delivery failure.
- `XWinTabHelper.c` uses **legacy XInput 1.x** (`xcb_input_select_extension_event`,
  `DeviceMotionNotify`/`DeviceValuator`/`DeviceButtonPress`) for event
  delivery, not XInput2. Empirically, while a second process
  (`xinput test-xi2 --root`) was actively holding an XI2 event selection on
  the same device, Photoshop's drawing spontaneously recovered (in a
  degraded "mouse mode" — drawable, but 0 pressure) — and broke again the
  moment that diagnostic process exited.
- Working theory: Xwayland's legacy XInput1 event delivery to a given
  client gets stuck/starved after certain focus-grab transitions (mouse
  click on another window), while XInput2 raw events keep flowing reliably
  through the same transition regardless of focus.
- Planned fix: rewrite the event-subscription/parsing portion of
  `XWinTabHelper.c` to use XInput2 (raw motion/button events via
  `xcb_input_xi_select_events` + `xcb_input_raw_motion_event_t` etc.)
  instead of the legacy XInput1 path, since XI2 has proven reliable through
  the exact scenario that breaks XI1.
