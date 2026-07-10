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
   no prebuilt binary exists for this fix. Build lives in this repo's
   `build64/`.
6. Found and fixed an additional bug in the patched source: `handle_event()`
   in `XWinTabHelper.c` hardcoded x/y to `(0,0)` for ButtonPress/ProximityIn
   events (only `DEVICE_VALUATOR` events carried real coordinates), causing
   every new stroke to start with a straight ray from the canvas origin.
   Fixed by caching last-known real position (`g_lastX`/`g_lastY`) and using
   that instead of `(0, 0)`.

7. Found and fixed the stall-after-focus-transition bug (see full detail in
   git commit `93c7ac2` / "Rewrite event delivery to use XInput2 raw
   events"): legacy XInput1 event delivery in `XWinTabHelper.c` silently
   stalls for this client after certain focus-grab transitions under
   Xwayland (fullscreen app + mouse click on it, then back to Photoshop).
   Diagnosed with `xinput test-xi2` proving the X11/Xwayland layer itself
   kept delivering events fine even while the app was stuck — pointing at
   the legacy XI1 delivery path specifically. Rewrote event
   subscription/parsing to use XInput2 raw motion/button events instead
   (focus-independent by design), selecting on `XIAllDevices` and filtering
   by each event's `sourceid` (raw events can't be selected for an
   individual slave device id — an earlier attempt that tried this
   regressed and broke on any alt-tab, not just the fullscreen+click case).

## Current working state — all known issues fixed

- **Pressure sensitivity: works.**
- **Curve/motion streaming: works.**
- **Stall after fullscreen+mouse-click alt-tab: fixed.**
- Files changed (persistent, already in place):
  - `<Photoshop install dir>/wintab32.dll` — our patched build (backup of
    the old dormant v0.2 at `wintab32.dll.v0.2.bak` in same dir)
  - `<Photoshop install dir>/XWinTabHelper.dll.so` — our patched build
  - Registry: `HKCU\Software\Wine\DllOverrides\wintab32 = native` (set in
    the Wine prefix, persists)
- The fix is entirely prefix-local (the two files above + the registry key),
  so it's **Wine-version-independent** — any Wine binary pointed at this
  prefix picks it up automatically, no per-version reinstall needed.

## Wine version recommendation

Tested several Lutris runner builds for base Photoshop stability (separate
from the tablet fix, which held identically across all of them since it's
prefix-local):

| Runner | Result |
|---|---|
| system `wine-staging` 11.12-1 (pacman) | Crashes on launch outright |
| `wine-11.7-staging-tkg-amd64` | Process stays alive but UI never appears |
| `wine-11.0-amd64` | UI appears but heavy visual glitches / freezing — unrelated graphics-backend regression, not investigated further |
| `wine-10.4-staging-tkg-amd64` | **Clean — no glitches, no freezing, PNG export works, tablet fix fully holds including the fullscreen/alt-tab scenario** |
| `wine-8.0-staging-tkg-amd64` | Clean, fully working (original baseline before newer versions were tried) |

**Recommendation: use `wine-10.4-staging-tkg-amd64`** — newer than the
original 8.0 baseline, fixes whatever made 11.x freeze, and confirmed clean
across the full test cycle (pressure, curve, alt-tab/fullscreen stall,
PNG export). In Lutris: set that game's Wine version dropdown accordingly;
no special env vars needed for daily use.

The 11.x freezing/glitching is a separate, unexplored bug (graphics
backend / DXVK-related, not input-related) — worth a fresh investigation
of its own if newer Wine is wanted later, but out of scope here.

## Bonus fix: "Export As" black panel

Not a tablet/input bug, but found and fixed in the same investigation
using this same prefix, so documented here too.

**Symptom**: File → Export → Export As opens a window, but its content is
entirely black.

**Cause**: Photoshop's "Export As" (and "Save for Web", non-legacy) panels
are actually Adobe CEP (Common Extensibility Platform) extensions —
real embedded Chromium/CEF instances (`Required/CEP/CEPHtmlEngine.exe`),
not native Win32 dialogs. "Save for Web (Legacy)" is the old native
dialog and works fine, which is what made this diagnosable as CEF-specific
rather than a general Photoshop/Wine rendering problem.

Confirmed via that engine's own Chromium log files (at
`drive_c/users/<name>/AppData/Local/Temp/CEPHtmlEngine9-*-com.adobe.WEBPA.crema.save*.log`,
enabled by default, no extra flags needed) that the real error is ANGLE
(Chromium's D3D11/GL translation layer) failing to create a swap chain:

```
rx::SwapChain11::reset(579): Could not create additional swap chains or
offscreen surfaces, HRESULT: 0x80004001 (E_NOTIMPL)
eglCreateWindowSurface failed with error EGL_BAD_ALLOC
```

i.e. ANGLE's D3D11 backend hits something Wine's D3D11 (wined3d/DXVK)
doesn't implement, so it never gets a render target and the panel stays
black. This is a narrower, more specific issue than Wine's
DirectComposition gap (which was the earlier working theory) — it's an
ANGLE/D3D11 problem inside the bundled CEF build, not a Wine window-
compositing problem.

**Fix**: Adobe CEP extensions support injecting extra Chromium command-
line flags via a `CEFCommandLine` block in the extension's
`CSXS/manifest.xml`. Added `--use-angle=gl` (forces ANGLE to render via
native OpenGL instead of D3D11) to both extensions in
`Required/CEP/extensions/com.adobe.photoshop.crema/CSXS/manifest.xml`
(original backed up as `manifest.xml.bak` in the same folder). Wine's
OpenGL passthrough to the host Mesa driver is far more mature than its
D3D11/DirectComposition stack, so this sidesteps the broken path
entirely — Export As now renders correctly. Slight perceived sluggishness
in the panel is a plausible real tradeoff (GL passthrough vs. a working
D3D11+dcomp compositor path), not necessarily just perception.

Confirmed via process inspection (`ps aux | grep CEPHtmlEngine`) that this
is CEF-specific: only auxiliary panels (this export panel, and a
background `com.adobe.Butler.backend` service) spawn `CEPHtmlEngine.exe`
processes. The main canvas/document rendering is Photoshop's own native
engine, not CEF, and was never affected by this.
