# Codebase Walkthrough

This document is for anyone reading `build64/WinTab.c`, `build64/XWinTabHelper.c`,
and `build64/XWinTabTypes.h` for the first time, who hasn't worked with the real
Win32 Wintab API or the X11/XCB/XInput2 APIs before. Both of those APIs come
from an older, more terse era of systems programming, and their naming
conventions can look like alphabet soup if you've never met them. This
walkthrough explains where the names come from, what each layer of the code
actually does, and how data flows from a physical pen touching a tablet all
the way to Photoshop drawing a stroke.

Nothing here is specific to this project's own coding style. The naming
oddities described in part 1 and part 2 come from specifications written in
the 1990s (Wintab) and the 2000s-2010s (X11 XInput), which this project has
to speak in order to bridge one to the other.

## 1. Wintab API naming conventions

Wintab is a Windows API for tablet/pen input, originally published as a spec
by LCS/Telegraphics in 1991 and still used today by graphics apps (Photoshop,
Illustrator, Krita, CorelDRAW, etc.) that want pressure-sensitive pen input.
`build64/WinTab.c` in this repo is a from-scratch implementation of that
1990s spec's *interface* (the function names, structs, and constants an app
expects to call), backed by real Linux tablet data instead of a real Windows
tablet driver.

### The `WT` prefix

Every public function in the Wintab API starts with `WT`, short for
"Wintab." You can see this in `build64/wintab32.def`, which lists the
functions this DLL exports to any program that loads it:

```
WTInfoW        @1020
WTOpenW        @1021
WTGetW         @1061
WTClose        @22
WTPacketsGet   @23
WTPacketsPeek  @80
WTEnable       @40
WTOverlap      @41
WTQueueSizeGet @84
WTQueueSizeSet @85
```

This is just a namespacing convention, the same idea as `GetProcAddress` /
`GetLastError` all starting with a "verb-noun" Win32-style pattern, except
Wintab uses a fixed two- or three-letter product prefix instead. Think of it
like a C library's `sqlite3_` or `curl_` prefix: it exists so the symbol
names don't collide with anything else linked into the same program, back in
an era before C++ namespaces were common.

A few of the actual functions, and what they do:

- **`WTOpenW`** (in `WinTab.c`, `WTOpenW(HWND hwnd, LPLOGCONTEXTW pLContext, BOOL enable)`)
  opens a new "tablet context" for a window, similar to how you'd open a file
  handle. This is the first thing Photoshop calls to start receiving pen
  data.
- **`WTInfoW`** (`WTInfoW(UINT cat, UINT idx, LPVOID ptr)`) is a
  general-purpose "ask a question" function. Photoshop calls it repeatedly at
  startup to ask things like "what's the pressure axis range on this
  device?" or "give me the default context settings."
- **`WTPacketsGet`** (`WTPacketsGet(HCTX ctx, int count, LPVOID ptr)`) is how
  Photoshop actually drains queued pen events: "give me up to `count` packets
  you've been buffering since I last asked."
- **`WTClose`**, **`WTEnable`**, **`WTOverlap`**, **`WTQueueSizeGet`**,
  **`WTQueueSizeSet`** round out context lifecycle and configuration.

Notice the ordinal numbers after each function name in `wintab32.def`
(`@1020`, `@22`, etc.). Some older Windows programs look up DLL functions by
a numeric ordinal instead of by name, for speed, and Wintab's spec fixes
these numbers so that any Wintab DLL is a drop-in replacement for any other.
This project preserves those specific numbers on purpose, inherited from the
original spec via the upstream projects credited in the README, so that
apps expecting ordinal-based lookups still work.

### The `W` suffix: wide-string / Unicode versions

You'll notice `WTOpenW`, `WTInfoW`, and `WTGetW` all end in `W`, while
`WTClose` and `WTPacketsGet` don't. This isn't a Wintab-specific quirk: it's
a general Win32 convention. Windows APIs that take or return strings
typically come in two flavors:

- An `A` suffix (or no suffix at all in old code) for "ANSI" (8-bit,
  single-byte-per-character) strings.
- A `W` suffix for "wide" strings: UTF-16, using the 16-bit `WCHAR` type,
  which is what Windows uses internally for all text.

`WTOpenW` takes a `LPLOGCONTEXTW`, a pointer to a `LOGCONTEXTW` struct, whose
name also carries that `W`. Functions that don't touch strings at all
(`WTClose`, `WTPacketsGet`, `WTEnable`) don't need an `A`/`W` split, so they
just have one plain name. This project only implements the `W` (Unicode)
versions, which is fine because Photoshop, like virtually all modern Windows
software, calls the wide versions.

The `WINAPI` you'll see on every function signature (for example
`HCTX WINAPI WTOpenW(...)`) is a calling-convention macro (it expands to
`__stdcall` on x86). It tells the compiler how arguments get pushed onto the
stack and who cleans them up afterward. You need to match it exactly, or the
caller and callee disagree about stack layout and everything crashes. It's
unrelated to the `W`-for-wide-string suffix above, despite looking similar.

### `HCTX`: a "handle to a context"

```c
typedef struct ContextTAG {
    PacketQueue queue;
    HWND hwnd;
    HCTX handle;
    BOOL enabled;
    BOOL inProximity;
    LOGCONTEXTW logContext;
} Context;
```

`HCTX` stands for "handle to a context." Windows APIs almost universally
expose "handles" instead of raw pointers or struct references: `HWND`
(handle to a window), `HDC` (handle to a device context), `HANDLE` (generic
handle), and so on. A handle is an opaque, small value (often literally just
an integer) that the API gives you back so you can refer to some internal
object later, without you ever seeing or touching that object's actual
memory layout. This lets Windows change its internal representation of
"a context" across OS versions without breaking any program that only ever
stores and passes around the handle value.

In this codebase, `WTOpenW` returns `(HCTX) 128`, a hardcoded constant cast
to look like a handle. Since this implementation only ever supports one
context at a time (see the README's "Known limitations"), it doesn't need a
real allocator handing out distinct handle values. Any real Wintab
implementation might, in principle, hand out different handles for
different contexts; this one just always hands back the same fixed value
and checks `g_context.handle != ctx` everywhere to validate that the caller
is talking about "the" context.

### `LOGCONTEXTW`: the "logical context"

A "context" in Wintab terms describes one open pen-input session:
what window it's tied to, what coordinate mapping is used, which packet
fields you want delivered, and so on. `LOGCONTEXTW` ("logical context,
wide-string version") is the struct that describes all of that
configuration. You can see it being filled in by `init_log_context()` in
`WinTab.c`:

```c
lctx->lcOptions = CXO_SYSTEM;
lctx->lcStatus = CXS_ONTOP;
lctx->lcPktRate = 100;
lctx->lcPktData = PK_CONTEXT | PK_STATUS | PK_SERIAL_NUMBER| PK_TIME |
                  PK_CURSOR | PK_BUTTONS |  PK_X | PK_Y |
                  PK_NORMAL_PRESSURE | PK_ORIENTATION;
```

The `lc` prefix on every field (`lcOptions`, `lcStatus`, `lcPktRate`, ...) is
just "logical context," repeated on every member, which was a common C
convention before IDEs had good autocomplete: it let you grep for all fields
belonging to one struct type, and made it obvious which struct a variable
came from even out of context.

### `WTI_`, `DVC_`, and `PK_`: the constant-prefix families

The Wintab spec defines dozens of numeric constants, grouped into a few
prefix families, each meaning something different:

- **`WTI_` (Wintab Info category)**: the first argument to `WTInfoW`, saying
  *what kind of thing* you're asking about. You can see the categories this
  project handles in `WinTab.c`'s `WTInfoW`:
  `WTI_DEFCONTEXT`, `WTI_DEFSYSCTX` (default context settings),
  `WTI_DEVICES` (info about the physical tablet device),
  `WTI_CURSORS` (info about the pen/cursor), `WTI_INTERFACE` (info about
  this DLL itself, like its version string).

- **`DVC_` (device query sub-index)**: the second argument to `WTInfoW`
  *when* the category is `WTI_DEVICES`, saying which specific piece of
  device info you want. For example:

  ```c
  if (cat == WTI_DEVICES) {
      if (idx == DVC_PKTDATA || idx == DVC_CSRDATA) { ... }
      if (idx == DVC_X || idx == DVC_Y || idx == DVC_NPRESSURE) { ... }
  }
  ```

  `DVC_X`/`DVC_Y` ask for the tablet's X/Y coordinate range, `DVC_NPRESSURE`
  asks for the "normal pressure" (regular tip pressure, as opposed to
  `DVC_TPRESSURE`, "tangential pressure," used by pucks/airbrush-style
  devices this project doesn't support), and `DVC_ORIENTATION` asks for the
  tilt/rotation axis ranges. Photoshop calls `WTInfoW` with these at startup
  to learn what your tablet's pressure resolution actually is, which is the
  fix that `gurppt/flashcs6linux_deploy` (credited in the README) originally
  added to base XWinTab.

- **`PK_` (packet data bitmask flags)**: these describe *which fields are
  present in each pen-event packet*, as a bitmask, not a single value.
  `lcPktData` in `LOGCONTEXTW` is a bitwise-OR of `PK_` flags saying "I want
  packets that include an X coordinate, a Y coordinate, pressure," etc. You
  can see this bitmask being consumed field-by-field in `packet_copy()`:

  ```c
  if (mask & PK_X)
      written += copy_field(&pkt[written], &packet->pkX, sizeof(LONG));
  if (mask & PK_Y)
      written += copy_field(&pkt[written], &packet->pkY, sizeof(LONG));
  if (mask & PK_NORMAL_PRESSURE)
      written += copy_field(&pkt[written], &packet->pkNormalPressure, sizeof(UINT));
  ```

  This is why the packet format Photoshop receives isn't a fixed C struct:
  it's a variable-length, tightly packed sequence of fields, whose exact
  layout depends on which `PK_` bits the app asked for when it opened the
  context. `packet_copy()` walks the mask bit by bit and appends only the
  fields that were requested, in a fixed field order defined by the spec.

## 2. The X11/XCB side naming conventions

The other half of this project, `build64/XWinTabHelper.c`, doesn't talk
Wintab at all. It talks to the Linux/X11 windowing stack directly, using
`libxcb` (the "X C Bindings" library, a lower-level alternative to the more
common `libX11`) and its `xcb/xinput.h` header, which wraps the X11
"XInput" protocol extension.

### `xcb_input_*` functions

Every function starting with `xcb_input_` is a binding generated from the
XInput X11 protocol extension's specification. XCB (unlike libX11) is
largely machine-generated from XML protocol descriptions, so the function
names map very directly onto extension requests and replies defined by the
X.Org protocol spec, not onto anything Linux- or kernel-specific. For
example:

```c
xcb_input_list_input_devices_cookie_t dev_list_cookie;
dev_list_cookie = xcb_input_list_input_devices(g_data.connection);
```

`xcb_input_list_input_devices` sends the `ListInputDevices` XInput request
to the X server, asking "what input devices do you know about" (keyboards,
mice, tablets, touchpads, anything). The `_cookie_t` return value is XCB's
async pattern: sending the request returns a lightweight "cookie" token
immediately, and you separately call `xcb_input_list_input_devices_reply()`
later to actually block and wait for the server's answer. This split exists
so a client can pipeline several requests to the X server without waiting
for each one's reply before sending the next.

### XInput1 vs. XI2 ("XInput2")

You'll see references to both "XInput1" and "XInput2" (often shortened to
"XI1" and "XI2") in this codebase's comments and git history. These are two
different, sequential versions of the same X11 extension:

- **XInput1** ("the X Input Extension," first released in the 1990s) is the
  original protocol for querying and receiving events from "extension
  devices" beyond the core keyboard/mouse, things like tablets, touchpads,
  and multi-button pens. Its event model is tied to *which window has
  focus*, similar to how core X11 keyboard/mouse events work.

- **XInput2** (released around 2009) is a full redesign that added, among
  other things, a distinction between "master" devices (the logical
  pointer/keyboard seen by normal apps) and "slave" devices (individual
  physical hardware, like one specific tablet), plus a new category of
  **raw events** that bypass window focus entirely (see below).

This project originally used XInput1-style event delivery (the file's
top comment still says "Currently this code uses XInput 1 functionality via
libxcb," left over from that era), inherited from the upstream XWinTab
project. STATUS.md documents finding a bug where XInput1 event delivery
silently stops working for a client after certain window-focus transitions
under Xwayland. The fix (see git commit `93c7ac2`, "Rewrite event delivery
to use XInput2 raw events") replaced that with XInput2 raw-event delivery,
which sidesteps the problem because raw events don't depend on window
focus at all. That's why both "XInput1" and "XI2"/"XInput2" appear in the
history: one is what the code used to do, the other is what it does now,
and old comments referencing "XInput 1" are a leftover from before the
rewrite.

### Raw events vs. normal/focused events

In XInput2, a "raw event" (`XCB_INPUT_RAW_MOTION`, `XCB_INPUT_RAW_BUTTON_PRESS`,
`XCB_INPUT_RAW_BUTTON_RELEASE`, all handled in `handle_event()` in
`XWinTabHelper.c`) reports hardware activity *before* it's been filtered
through window focus, pointer grabs, or which window is under the cursor.
Think of it as "the raw firehose of everything this device is doing,"
similar in spirit to reading `/dev/input/eventN` directly, except delivered
over the X11 protocol instead of the kernel evdev interface. A "normal"
(non-raw) XInput2 event, by contrast, is the kind that's actually delivered
to a specific window because that window has focus or a grab, and is
subject to all the usual window-manager/focus routing logic, exactly like a
regular `KeyPress`/`ButtonPress` event.

This project deliberately chose raw events specifically *because* they
don't care about focus. That's the whole point of the fix in commit
`93c7ac2`: Photoshop's window occasionally lost its ability to receive
normal/focused input events after certain alt-tab sequences under Xwayland,
but raw events kept flowing to the process regardless, which is exactly
the property this bug fix needed.

The tradeoff, mentioned in the code comments and in the README's "Known
limitations," is that raw events carry no built-in "proximity in/out"
notion (the concept of the pen entering or leaving hover range above the
tablet), because that's a higher-level notion that only "normal" device
events happen to carry in some drivers. That's why `XWinTabHelper.c` has to
*synthesize* proximity events itself, from motion/button activity plus an
idle timeout (`kProximityTimeoutMs`, currently 200ms), rather than simply
forwarding a proximity event the X server gave it.

### `sourceid` vs. `deviceid`

XInput2 events carry two different device-identifying fields, and mixing
them up is a common source of bugs (the code comments call this out
explicitly):

- **`deviceid`**: which device the event was *selected on* / delivered to.
  When you ask to receive raw events, you generally have to select them on
  a "master" pointer/keyboard device, or on the special `XIAllDevices`
  pseudo-device, not on one specific piece of hardware. You can see this in
  `select_events()`:

  ```c
  // Raw events can only be selected for XIAllDevices/XIAllMasterDevices,
  // not for an individual slave device id -- selecting on our specific
  // stylus id silently degrades under Xwayland...
  req.header.deviceid = XCB_INPUT_DEVICE_ALL;
  ```

- **`sourceid`**: which *actual physical device* generated this particular
  event, once you've received it. Since the code selects broadly (on
  `XCB_INPUT_DEVICE_ALL`), it will receive raw events from every input
  device on the system: your mouse, your keyboard, your trackpad, and your
  tablet stylus, all mixed together. `sourceid` is how the code figures out
  which of those events actually came from the tablet it cares about, and
  discards the rest:

  ```c
  static void handle_raw_motion(xcb_input_raw_motion_event_t *event) {
      if (event->sourceid != g_data.device.id)
          return;
      ...
  ```

  `g_data.device.id` was recorded earlier, during device enumeration in
  `check_device()`/`check_devices()`, as the specific XInput device ID that
  looked like a tablet stylus (matched by name containing "stylus" or
  "pen," or by an explicit `XWINTAB_DEVICE` environment variable override).

In short: you *select* broadly on `deviceid = XIAllDevices` because that's
the only thing XInput2 raw events allow, and then you *filter* narrowly by
each event's `sourceid` in your own code, because the X server won't do
that filtering for you at the raw-event level.

## 3. Call-by-call data flow: pen touch to Photoshop pixels

Here's the full path a single pen movement takes, naming the actual
functions and files involved at each hop.

**1. Physical hardware.** The tablet's stylus moves or the tip touches
down. This produces raw electrical/USB (or Bluetooth) signal changes inside
the tablet's own firmware.

**2. Kernel input subsystem / libinput.** The Linux kernel's USB HID driver
(or a Wacom-specific kernel driver) turns that signal into kernel input
events on a `/dev/input/eventN` device node. The `libinput` userspace
library (used by both X11 and Wayland compositors) reads those kernel
events and turns them into a higher-level, driver-independent event stream:
motion deltas/absolute positions, pressure values, tilt values, button
presses.

**3. Xwayland (or a real X server).** If you're on a Wayland compositor
(which is the common case these days, and is explicitly called out as
supported in this project's README), `Xwayland` is a compatibility X11
server that runs alongside the Wayland compositor specifically so that
X11-only applications (like Wine/Windows apps, which don't speak Wayland
natively) can keep working. Xwayland receives input from the compositor and
re-exposes it as ordinary X11/XInput2 events, including the raw events this
project relies on. If you're on plain X11 (not Wayland at all), a real X
server does the equivalent job directly, without the Xwayland translation
step.

**4. X11 XInput2 extension.** Xwayland (or the X server) tags each event
with a device ID (`sourceid`) and event type (`XCB_INPUT_RAW_MOTION`,
`XCB_INPUT_RAW_BUTTON_PRESS`, etc.), and queues it for delivery to any
client that asked to receive it, over the client's existing X11 connection
socket.

**5. `XWinTabHelper.c`'s event loop.** This is where this project's own
code starts. `CheckEvents()` (a `WINAPI`-exported function, called
repeatedly from a background thread started in `WinTab.c`) calls
`check_events()`, which does:

```c
poll(&poll_fd, 1, timeout);
while (xcb_event = xcb_poll_for_event(g_data.connection))
    handle_event(xcb_event);
```

`poll()` is the standard POSIX call to wait for data on a file descriptor
(here, the X11 connection socket) without busy-looping. Once data is ready,
`xcb_poll_for_event()` pulls one parsed X11 event out of XCB's internal
buffer at a time. `handle_event()` checks that the event is a "generic
event" belonging to the XInput extension (`ge->extension ==
g_data.xiMajorOpcode`), then dispatches by `ge->event_type` to
`handle_raw_motion()` or `handle_raw_button()`.

**6. Parsing the raw event's value list.** XInput2 raw motion events don't
carry a fixed `x`/`y`/`pressure` struct. Instead they carry a bitmask
("valuator mask") saying which axes changed, followed by a packed list of
only the values that did. `handle_raw_motion()` walks that bitmask bit by
bit:

```c
for (int bit = 0; bit < mask_words * 32; bit++) {
    if (!(mask[bit / 32] & (1u << (bit % 32))))
        continue;
    double v = fp3232_to_double(values[value_idx++]);
    switch (bit) {
    case 0: x = (int32_t) v; got_position = 1; break;
    case 1: y = (int32_t) v; got_position = 1; break;
    case 2: pressure = (int) v; break;
    case 3: xTilt = (int) v; break;
    case 4: yTilt = (int) v; break;
    }
}
```

Axis 0/1 are always X/Y; axis 2/3/4 (pressure, X-tilt, Y-tilt) are
device-specific conventions this project assumes, matched against the
tablet during device detection in `check_device()`. `fp3232_to_double()`
converts X11's fixed-point 32.32 number format (`xcb_input_fp3232_t`, a
32-bit integer part plus a 32-bit fractional part, an old fixed-point
convention from before every device reliably had a fast FPU) into a normal
C `double`.

**7. The callback into `WinTab.c`.** Once `handle_raw_motion()` (or
`handle_raw_button()`) has a fully parsed event, it fills the shared
`EventInfo` struct (defined in `XWinTabTypes.h`, shared by both files) and
calls `g_data.callback(&g_eventInfo)`. That callback was registered earlier
by `WinTab.c`, in `WTOpenW()`, via `pBeginEvents(on_event)`. This is the
literal bridge from the Linux/X11 side of the codebase back into the
Windows/Wintab side: `on_event()`, a plain `WINAPI` function living in
`WinTab.c`, is being called directly, as a normal C function pointer call,
from code compiled as native Linux code.

**8. `on_event()`: turning a generic event into a Wintab packet.** In
`WinTab.c`, `on_event()` takes the plain `{x, y, pressure, xTilt, yTilt,
buttonsState}` struct and turns it into a `PacketData` (this project's own
internal packet representation), doing Wintab-specific work:

- Scaling the raw device coordinate range into whatever output coordinate
  range Photoshop asked for, via `scale_axis()` (this respects
  `LOGCONTEXTW`'s `lcInOrgX`/`lcInExtX`/`lcOutOrgX`/`lcOutExtX` fields, the
  "logical context" input/output mapping described in part 1).
- Converting X/Y tilt into Wintab's azimuth/altitude polar representation
  via `calculate_azimuth()`.
- Assigning a monotonically increasing `pkSerialNumber` so Photoshop can
  detect dropped or out-of-order packets.
- Synthesizing `WT_PROXIMITY` transitions when appropriate.

**9. The packet queue.** The finished `PacketData` is pushed into
`g_context.queue`, a small ring buffer (`queue_write()`/`queue_read()` in
`WinTab.c`) sized 128 entries by default. This decouples "when the X11
event arrived" from "when Photoshop gets around to asking for it": the
background thread (`thread_func()`) can keep draining X11 events and
filling the queue even if Photoshop is momentarily busy doing something
else.

**10. Notifying Photoshop.** After queuing a packet, `on_event()` calls
`context_message()`, which does `PostMessageW(ctx->hwnd, msg +
ctx->logContext.lcMsgBase, ...)`. This posts a private Windows message to
Photoshop's own message queue (`msg + lcMsgBase` is Wintab's convention for
letting each app pick its own private message-ID range, offset from a base
value called `lcMsgBase`, to avoid colliding with the app's own custom
window messages). This is the standard Windows way of saying "something
happened, come look," without blocking the thread that detected it.

**11. Photoshop calls `WTPacketsGet`.** Somewhere in Photoshop's own event
loop, upon receiving that posted message, it calls back into this DLL via
`WTPacketsGet(HCTX ctx, int count, LPVOID ptr)`, which drains the ring
buffer (`queue_read()`) and serializes each `PacketData` into whatever
byte layout the app's requested `PK_` bitmask describes (via
`packet_copy()`, discussed in part 1). Photoshop then reads pen position,
pressure, and tilt back out of that buffer and uses it to render the
stroke on the canvas.

That's the complete round trip: **stylus hardware -> kernel/libinput ->
Xwayland -> X11 XInput2 raw event -> `XWinTabHelper.c`'s `check_events()` /
`handle_raw_motion()` -> `on_event()` callback in `WinTab.c` -> packet queue
-> posted window message -> Photoshop calls `WTPacketsGet` -> pixels on
screen.**

## 4. The genuinely confusing bits, explained plainly

A few things in this codebase are confusing not because the code is badly
written, but because they're inherently unusual techniques or historical
leftovers. Worth calling out explicitly:

**Why does a Linux ELF `.so` file have a name ending in `.dll.so`?**
`XWinTabHelper.dll.so` is a real native Linux shared library (an ELF `.so`,
built by `winegcc`, not a Windows PE file at all), but it's *named* as if
it were a Windows DLL with an extra `.so` tacked on. This is a Wine-specific
convention: Wine's `LoadLibrary()` implementation (called from `WinTab.c`
as `LoadLibraryW(L"XWinTabHelper.dll.so")`) knows how to recognize this
naming pattern and load a native Linux `.so` in-process, rather than
expecting an actual Windows PE binary. It lets a Windows-side codebase
(`WinTab.c`, compiled as a real `wintab32.dll` PE file by
`x86_64-w64-mingw32-gcc`) call directly into Linux-native code (compiled by
`winegcc`) that talks to X11, without any inter-process communication, pipes,
or sockets: it's one operating-system process with both a Windows PE module
and a native Linux module loaded into the same address space, calling each
other's functions as ordinary C function pointers. This is a known,
supported Wine technique (not something this project invented), but it's
easy to miss the naming trick if you haven't seen it before.

**Why is `xcb_input_raw_button_press_valuator_mask` used to parse motion
events too?** In `handle_raw_motion()`:

```c
static void handle_raw_motion(xcb_input_raw_motion_event_t *event) {
    ...
    uint32_t *mask = xcb_input_raw_button_press_valuator_mask(
        (xcb_input_raw_button_press_event_t *) event);
```

This looks like a copy-paste mistake (a "button press" function being
called on a motion event), but it isn't. The `xcb_input_raw_motion_event_t`
and `xcb_input_raw_button_press_event_t` structs happen to have byte-for-byte
identical layouts in the X11 protocol: both are "a raw XInput2 event with a
device ID, timestamp, detail, valuator mask, and axis values," and the wire
format doesn't distinguish them beyond the event-type field used earlier in
`handle_event()`'s dispatch. Rather than duplicate the mask/value accessor
logic for every raw event type, the code reuses the `raw_button_press`
accessor functions (which XCB happens to have generated first/most visibly)
via a pointer cast, on the understanding that the struct shapes are
identical. It works, but it reads strangely if you don't know the two
structs are actually the same shape under the hood.

**Why does `WinTab.c` (Windows code) call into a Linux `.so` via
`LoadLibrary()` at all, instead of just doing everything in one file?**
Because the two halves of this problem need to run in genuinely different
execution environments. `WinTab.c` has to be a real Windows PE DLL, because
Photoshop (a Windows binary running under Wine) looks it up and calls it
using the Windows DLL-loading and calling-convention machinery; there's no
way around that if you want to intercept the Wintab API. But talking to
X11/XInput2 means calling `libxcb`, a native Linux library, which doesn't
exist in a form Windows PE code can normally call (no Windows import
library for it, no compatible calling convention or ABI expectations by
default). Compiling a second module as a native Linux `.so` via `winegcc`,
and using Wine's special-cased `LoadLibrary()` support for the `.dll.so`
naming convention, is the established way to bridge the two: one process,
two binary formats, connected in-process for speed, with `WinTab.c` calling
five exported functions (`Load`, `GetSelectedDevice`, `BeginEvents`,
`CheckEvents`, `Shutdown`, all listed in `XWinTabHelper.dll.spec`) and one
callback (`EventCallback`, called back from the helper into `WinTab.c`) as
the entire interface between the two worlds.
