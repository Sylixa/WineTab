# XWinTab Photoshop Fix

A patched build of [XWinTab](https://github.com/Graham--M/XWinTab) that fixes
tablet pressure and pen input for Adobe Photoshop (tested with Photoshop 2020)
running under Wine on Linux.

## What this fixes

Wine's own builtin `wintab32.dll` doesn't stream continuous pen motion
(curvy strokes render as straight lines) and doesn't report real pressure
data at all. This build:

- Streams full motion + pressure + tilt data properly
- Doesn't draw a stray straight line from the canvas corner at the start of
  every stroke
- Doesn't stop responding to the pen after alt-tabbing to/from a fullscreen
  app

See [STATUS.md](STATUS.md) for the full technical writeup of each bug and
how it was found/fixed.

## Credits

- [XWinTab](https://github.com/Graham--M/XWinTab) by Graham--M — the base
  Wintab-over-XInput implementation this is built on.
- Pressure-axis fix (`WTI_DEVICES` AXIS/mask handling), ordinal exports, and
  several crash fixes from
  [gurppt/flashcs6linux_deploy](https://github.com/gurppt/flashcs6linux_deploy)
  (`xwintab/PATCHES.md`), originally written for Adobe Flash/Animate CS6.
- The stroke-start position bug fix and the XInput2 event-delivery rewrite
  (fixing the alt-tab/fullscreen stall) are new in this repo.

Same license as upstream XWinTab: MIT (see `build64/LICENSE`).

## Requirements

- Linux with X11 or Xwayland (this uses XInput2 directly via `libxcb`)
- `libxcb`, `libxcb-xinput` (present on basically any modern desktop distro)
- A Wacom-compatible tablet whose XInput device name contains "stylus"
- Wine — **`wine-10.4-staging-tkg-amd64` recommended** (tested clean; some
  newer Wine builds have an unrelated graphics-backend freezing bug, some
  much older ones crash Photoshop outright — see the Wine version table in
  STATUS.md)

## Building

Needs `mingw-w64-gcc` (or `x86_64-w64-mingw32-gcc`) and `winegcc`:

```bash
cd build64
winegcc -o XWinTabHelper.dll.so -shared -O2 XWinTabHelper.c XWinTabHelper.dll.spec -lxcb -lxcb-xinput
x86_64-w64-mingw32-gcc -shared -O2 -o wintab32.dll WinTab.c wintab32.def
```

This produces two files: `wintab32.dll` (the actual Windows-side Wintab DLL)
and `XWinTabHelper.dll.so` (a native Linux ELF helper it calls into for the
X11/XInput2 side — this is a normal Wine technique, not something exotic).

## Installing

1. Copy both `wintab32.dll` and `XWinTabHelper.dll.so` into **the app's own
   install directory** (e.g. next to `Photoshop.exe`) — not `system32`.
   Windows/Wine's DLL search order checks the app directory before
   `system32`, so putting it there is what actually makes it take effect,
   and avoids clobbering anything else that also uses `wintab32.dll` in the
   same prefix.
2. Tell Wine to prefer this real file over its own builtin stub:
   ```bash
   WINEPREFIX=/path/to/your/prefix wine reg add \
     "HKCU\Software\Wine\DllOverrides" /v wintab32 /t REG_SZ /d native /f
   ```
   This is a one-time step — it's stored in the prefix and persists across
   launches and Wine version changes.
3. Launch the app normally. In Lutris: just make sure the game's Wine
   version is set to a working runner (see Requirements above) — no special
   environment variables are needed for daily use.

### Debug logging (optional)

Set `XWINTAB_LOG=1` before launching to get a verbose trace at
`%USERPROFILE%\XWinTabLog.txt` inside the prefix (e.g.
`drive_c/users/<name>/XWinTabLog.txt`). Useful if pressure/motion still
doesn't behave right on your setup and you want to see what Wintab calls
the app is actually making.

## Known limitations

- Only one Wintab context at a time (`WTOverlap` is a no-op stub — fine
  since Photoshop only ever opens one context).
- Proximity in/out (hover detection, used for brush-preview cursors) is
  *synthesized* from motion/button activity and a short idle timeout, not
  reported natively — XInput2 raw events have no distinct proximity event
  type. Should feel right in practice but isn't a byte-for-byte protocol
  match to real Wintab.
- Only tested against Photoshop 2020. Other Wintab-consuming apps
  (Illustrator, Krita, etc.) haven't been verified — the fix should apply
  generically, but device/quirk differences are untested.
