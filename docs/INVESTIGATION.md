# Window border utility: investigation & proposed design

*Date: 2026-10-14 · Machine used for verification: macOS 26.6.2 (25G83), Apple Silicon, Xcode 26.5 / Swift 6.3.2*
*Reference studied: [FelixKratz/JankyBorders](https://github.com/FelixKratz/JankyBorders) @ `a7297ca` (v1.9.0, 2026‑05‑14), GPL‑3.0*

> Status: investigation only. Nothing has been implemented. Probes mentioned below were throwaway programs kept outside the project.
>
> **Update (2026-10-14):** the scope was later cut down to *one colour per app, picked from the menu bar*. There are no title rules, no rule editor, and only fixed width/inactive defaults. The implemented design is described in the README. Part D's rule engine, settings window and JSON schema describe the original, larger proposal.
>
> Two measurements made during implementation changed the design:
> - **Overlays are four thin strips, not one window.** A full-size transparent overlay costs ~38 MB per large Retina window (backing memory sits in our process); four strips cost ~3 MB.
> - **`SLSSetWindowShape(cid, wid, x, y, region)` also moves the window's origin to (x, y).**

---

## 0. Summary

- **Feasible.** Every primitive needed (window discovery, change notifications, overlay windows, owner PID → bundle ID) exists and works on macOS 26.6.2 **with no permissions at all**. I confirmed this with probes run as a fresh app that had no permissions granted (§B).
- **Bundle IDs need no permission.** Window → owner PID (SkyLight or CoreGraphics) → `NSRunningApplication.bundleIdentifier`. Verified 4/4 windows in the no-permission context.
- **Window titles need permission.** In the no-permission context, titles came back empty from **both** `CGWindowListCopyWindowInfo` (`kCGWindowName`) and SkyLight (`kCGSWindowTitle`), and AX returned `kAXErrorAPIDisabled`. Title rules therefore need **Accessibility** (recommended) or Screen Recording (not recommended).
- **There's no public-API path** for permission-free, low-latency window tracking. The engine must use private SkyLight APIs, so it can't ship on the Mac App Store and needs upkeep with each macOS release.
- **An independent implementation is practical.** We need facts (API signatures, event IDs) and general techniques, not JankyBorders' code. The MIT-licensed yabai documents the same SkyLight surface.
- **JankyBorders colours by focus only** (active/inactive), with per-window overrides by window ID via CLI. It has no app/title rule engine; that's our main addition.

---

## Part A: How JankyBorders works

### A1. Window discovery

1. **Startup snapshot** (`windows_add_existing_windows`):
   - `SLSCopyManagedDisplaySpaces(cid)` → every display's list of Spaces (`"Spaces"[].id64`).
   - `SLSCopyWindowsWithOptionsAndTags(cid, owner=0, spaces, options=0x2, set_tags=1, clear_tags=0)` → window IDs on all those Spaces.
   - `SLSWindowQueryWindows(cid, ids, 0)` + `SLSWindowQueryResultCopyWindows` → an iterator returning per-window **tags**, **attributes**, **parent ID**, **level** and (macOS 26) **corner radii**.
2. **Filtering** (`window_suitable`, which depends on **undocumented tag bits**). A window is kept if all of these hold:
   - `parent == 0`
   - `(attributes & 0x2) || (tags & bit58)`
   - not attached (bit 7, sheets)
   - doesn't ignore window cycling (bit 18, added in 1.9.0)
   - document (bit 0), or floating + modal (bits 1 & 31)

   The 1.9.0 filter tweak caused regressions where Ghostty, iTerm and Mail windows stopped getting borders (issue #200), which shows how fragile this is.
3. **Live discovery**: SkyLight notifications `1325` (window created / added to Space) and `1326` (destroyed / removed). Their payload is `{uint64 space_id; uint32 window_id}`.
4. **Consistency pass on Space change** (`1401`, delayed 20 ms): re-enumerates the current Space of each display and creates any missing borders.
5. **Own windows are excluded**: owner PID == own PID.

### A2. Focused-window detection

- **Default (no permissions)**, `get_front_window`:
  1. `_SLPSGetFrontProcess(&psn)` → `SLSGetConnectionIDForPSN` gives the front app's connection.
  2. `SLSCopyWindowsWithOptionsAndTags(cid, front_cid, [active_space], 0x2, …)` returns that app's windows, front to back.
  3. The **first suitable window** is treated as focused. This is a heuristic.
- **Optional (`ax_focus=on`, needs Accessibility)**: `AXUIElementCreateApplication(pid)` → `kAXFocusedWindowAttribute` → `_AXUIElementGetWindow()` (private) → window ID.
- **Re-evaluated when**:

  | Trigger | Delay |
  |---|---|
  | front-app change (`1508`) | 50 ms |
  | reorder (`808`) | 10 ms |
  | title change (`1322`) | 50 ms |
  | update (`723`) | 50 ms |
  | window create/destroy | — |
- **Active active/inactive style** is chosen per border on redraw.

### A3. Border window creation & rendering

Each tracked window gets **one overlay window**, created on **its own SkyLight connection** (`SLSNewConnection` per border).

1. **Create**: `SLSNewWindow(cid, kCGBackingStoreBuffered, -9999, -9999, region)`, i.e. off-screen, with a rectangular region.
2. **Configure**:
   - `SLSSetWindowResolution` (1× or 2× when `hidpi=on`)
   - `SLSSetWindowTags` sets undocumented tag bits 1 and 9 (believed to be "floating" and click-through/ignore-events)
   - `SLSSetWindowOpacity(…, 0)` makes it non-opaque
   - `SLSWindowSetShadowProperties` with `com.apple.WindowShadowDensity = 0` removes the shadow
3. **Draw**:
   - `SLWindowContextCreate` returns a `CGContext` backed by the window.
   - CoreGraphics strokes a rounded rect. An even-odd clip keeps paint out of the target's content area.
   - `CGContextFlush` → `SLSFlushWindowContentRegion`.
4. **Resize without tearing** (reworked repeatedly for macOS 26, latest in 1.9.0 "fix flickering on window resize"):
   - `SLSDisableUpdate` → `SLSWindowFreezeWithOptions`
   - → `SLSSetWindowShape` with the new region
   - → redraw → `SLSWindowThaw` → `SLSReenableUpdate`
5. **Atomic placement** via a transaction:
   - `SLSTransactionCreate`
   - `MoveWindowWithGroup(origin)` and `SetWindowTransform(-origin)`
   - `SetWindowLevel(level of target)` and `SetWindowSubLevel(sub-level of target)`
   - `OrderWindow(border, below|above, target_wid)`
   - `Commit`
6. **Hide**: `SLSTransactionOrderWindow(border, 0 /*out*/, target)`. **Unhide**: re-order relative to the target.

> Inference: the backing store spans the **whole** border rectangle (window size + border), mostly transparent. With `hidpi=on` that is 4× the pixels. This is a plausible contributor to the WindowServer memory/GPU reports (#128, #188). Worth measuring in our spike (§D10).

### A4. Private / system APIs used

| Area | APIs | Kind |
|---|---|---|
| Connection & events | `SLSMainConnectionID`, `SLSNewConnection`, `SLSReleaseConnection`, `SLSRegisterNotifyProc`, `SLSRequestNotificationsForWindows`, `SLSGetEventPort`, `SLEventCreateNextEvent`, `_CFMachPortSetOptions` | private (SkyLight / CF SPI) |
| Window query | `SLSWindowQueryWindows`, `SLSWindowQueryResultCopyWindows`, `SLSWindowIteratorAdvance/GetCount/GetWindowID/GetParentID/GetTags/GetAttributes/GetLevel`, `SLSWindowIteratorGetCornerRadii` (26+, via `dlsym`), `SLSGetWindowBounds`, `SLSWindowIsOrderedIn`, `SLSGetWindowLevel`, `SLSGetWindowTransform`, `SLSCopyWindowsWithOptionsAndTags` | private |
| Ownership | `SLSGetWindowOwner`, `SLSConnectionGetPID`, `_SLPSGetFrontProcess`, `SLSGetConnectionIDForPSN`, `proc_name` (libproc) | private + public |
| Spaces/displays | `SLSCopyManagedDisplays`, `SLSCopyManagedDisplaySpaces`, `SLSManagedDisplayGetCurrentSpace`, `SLSCopyManagedDisplayForWindow`, `SLSCopyActiveMenuBarDisplayIdentifier`, `SLSCopySpacesForWindows`, `SLSMoveWindowsToManagedSpace`, `CGGetActiveDisplayList`, `CGDisplayCreateUUIDFromDisplayID` | private + public |
| Overlay windows | `SLSNewWindow`, `SLSNewWindowWithOpaqueShapeAndContext`, `SLSReleaseWindow`, `SLSSetWindowShape`, `CGSNewRegionWithRect`, `CGRegionCreateEmptyRegion`, `SLSSetWindowTags/ClearWindowTags`, `SLSSetWindowResolution`, `SLSSetWindowOpacity`, `SLSSetWindowAlpha`, `SLSWindowSetShadowProperties`, `SLWindowContextCreate`, `SLSFlushWindowContentRegion`, `SLSWindowFreezeWithOptions`, `SLSWindowThaw`, `SLSDisableUpdate`, `SLSReenableUpdate` | private |
| Transactions | `SLSTransactionCreate/Commit`, `…MoveWindowWithGroup`, `…OrderWindow`, `…SetWindowLevel`, `…SetWindowSubLevel`, `…SetWindowAlpha`, `…SetWindowTransform` | private |
| **Raw WindowServer IPC** | Hand-built MIG `mach_msg` to read a window's **sub-level**: request ID `0x73c3` (≤15) / `0x76e3` (26), reply `0x7427` / `0x7747`. The port comes from **`CGSGetConnectionPortById`, a non-exported symbol** found by walking SkyLight's in-memory symbol table | private, extremely fragile |
| Accessibility | `AXIsProcessTrusted(WithOptions)`, `AXUIElementCreateApplication`, `kAXFocusedWindowAttribute`, `_AXUIElementGetWindow` (private) | public + private |
| Drawing/animation | CoreGraphics paths/gradients, `CVDisplayLink` (yabai animation proxy only) | public |
| IPC with its own CLI | Mach bootstrap service `git.felix.borders`; yabai integration port `git.felix.jbevent` | public Mach APIs |

### A5. Positioning relative to the target

- **Border frame** = `CGRectInset(target_bounds, -(width + 8pt padding))`. Bounds come from `SLSGetWindowBounds` in global, top-left-origin coordinates, so it works across displays without per-display maths.
- **Stroke path**: drawn in a rect equal to the target bounds inside that canvas; the inner area is clipped out. The corner radius is the target's radius (macOS 26: read per window, else 9pt), with `inner_radius = radius + 1`.
- **Z-order**:
  - The overlay copies the target's level and sub-level, then is ordered **directly below** the target (default) or above it.
  - In "below" mode, the target covers the inner half of the stroke, and windows stacked above the target correctly hide the border.
- **Space**: `SLSMoveWindowsToManagedSpace(border, target_space)`, so macOS shows and hides the overlay along with its Space. Sticky targets (tag bit 11) make the overlay sticky too.

### A6. Lifecycle handling

| Situation | Mechanism in JankyBorders |
|---|---|
| **Move** | `806` → `border_move`: on a high-priority queue, re-read bounds and run a transaction **move only** (no redraw) |
| **Resize** | `807` (arrives with `806`) → full update: new frame, freeze → reshape → redraw → thaw |
| **Minimize / restore** | No special code: relies on hide `816` / unhide `815`. Measured on 26.6.2 (§B2): `816` arrives only **after** the minimize animation finishes and `815` arrives at the **start** of the restore. So the border lingers ~0.6 s and appears early (issue #182) |
| **Hide (app ⌘H / orderOut)** | `816` → order the overlay out; `815` → order it back in (skipped if the Space isn't visible or the window is too small) |
| **Close / destroy** | `804` close (kept as backup) + `1326` destroy (checks the Space ID matches, unless sticky) → order out, then release the overlay window and connection on the main queue |
| **Create** | `1325` → filter → create the overlay → re-send the **entire** watched-window list via `SLSRequestNotificationsForWindows` (fixed 1024-entry array) |
| **Native fullscreen** | The window moves to a new fullscreen Space. `1401` → consistency pass after 20 ms ("not all native-fullscreen windows have yet updated their space id"). The border is outside the window, so it falls off-screen. Open: border missing after leaving fullscreen on macOS 27 beta (#201) |
| **Spaces** | Overlays are bound to the target's Space, so macOS handles visibility. `1401` triggers a pass that creates missing borders and updates existing ones |
| **Multiple monitors** | Global coordinates and per-display current Space (`SLSCopyManagedDisplays` + `SLSManagedDisplayGetCurrentSpace`). With more than one display, the active Space comes from the menu-bar display. `hidpi` is a global switch, not per display |
| **Level changes** | `811` → update (copy the new level) |
| **Mission Control / Stage Manager / Exposé** | **Not handled.** WindowServer transforms windows without notifying, so stale borders remain (#29, #147). The author declined to poll transforms for performance reasons |
| **yabai animations** | yabai sends proxy-window IDs over Mach. JankyBorders then follows the proxy's transform with a `CVDisplayLink` |

### A7. Window → process/app mapping

- **PID**: `SLSGetWindowOwner(cid, wid, &owner_cid)` → `SLSConnectionGetPID(owner_cid, &pid)`.
- **App identity**: `proc_name(pid)`, i.e. the **process name**. That name is used for `blacklist=` / `whitelist=`.
- **No bundle IDs anywhere.** Process names are a weak key:
  - Cursor's process is `Cursor` but its bundle ID is the opaque `com.todesktop.230313mzl4w4u92`.
  - Names collide and change with renames or localisation.
- **Per-window styling** exists only as `borders apply-to=<window-id> …`, driven by external scripts (e.g. yabai signals).

### A8. Bundle ID without Accessibility: **yes (verified)**

The path is: owner PID (SkyLight as in A7, or `kCGWindowOwnerPID` from `CGWindowListCopyWindowInfo`) → `NSRunningApplication(processIdentifier:)` → `.bundleIdentifier`, plus `.localizedName` and `.icon` for the UI.

From the no-permission probe (§B1):
- 4/4 on-screen windows resolved to `com.google.Chrome`, `com.mitchellh.ghostty`, `com.brave.Browser`.
- The SkyLight owner PID matched the CoreGraphics owner PID in 4/4 cases.

Edge cases:
- **Processes without a bundle** (CLI-launched GUI tools): fall back to the executable path (`proc_pidpath`).
- **Chrome/Edge PWA windows** are most likely owned by the browser process, so they'd match the browser's bundle ID, not the PWA shim's. Verify.

### A9. What needs extra permissions

| Feature | Permission | Evidence |
|---|---|---|
| Window titles (title rules) | **Accessibility** (`kAXTitleAttribute`), or Screen Recording (`kCGWindowName` / `kCGSWindowTitle`) | Verified: 0/4 titles from CoreGraphics and SkyLight without Screen Recording. AX → `-25211` (`kAXErrorAPIDisabled`) without Accessibility |
| Title-change tracking | none for the *signal* (SkyLight `1322`, which JankyBorders registers); AX to *read* the new title | `1322` per JankyBorders source; not exercised in my probe |
| Exact focused window | Accessibility (`kAXFocusedWindowAttribute` + `_AXUIElementGetWindow`) | JankyBorders `ax_focus` |
| Discovery, events, overlays, bundle ID, front app | **none** | Verified: discovery, events, bundle ID, front app. Overlays: JankyBorders runs without AX by default |

Recommendation: request **Accessibility only when the user creates the first title rule**. Never require Screen Recording; it's a scarier grant ("record your screen") and gives nothing extra here.

### A10. macOS 26+ compatibility (what changed, what JankyBorders had to do)

1. **Per-window corner radii** (Liquid Glass). I observed **16** for Ghostty and **20** for Chrome/Brave.
   - New SPI `SLSWindowIteratorGetCornerRadii` (26+).
   - JankyBorders resolves it via `dlsym`, because hard-linking broke builds with older SDKs (#174).
2. **Resize glitches**: borders failed to resize or got corrupted (#160, #199). The resize path was rewritten twice (`f969290`, `7892941`), last in May 2026 for 26.5.
3. **Sub-level IPC IDs shifted** (`0x73c3` → `0x76e3`). The author tried exported `SLSGetWindowSubLevel` (`13c2e69`) and reverted to raw MIG 4 days later (`ef99c4d`). No reason was given.
4. **New window open/close/minimize animations**: hide events now arrive ~0.4–0.6 s late, so borders linger (#182, #172). The workaround users found is `NSAutomaticWindowAnimationsEnabled = false`.
5. **High WindowServer GPU use** reported on Tahoe (#188, app-dependent). A WindowServer memory leak was partly fixed by Apple in 26.2 (#128).
6. **Build breakage** with CLT 26 SDK headers (#190).
7. **macOS 27 "Golden Gate"**: one report says it shipped 2026‑09‑14, Apple Silicon only. Untested here. Open reports: intermittent missing border after leaving fullscreen (#201); iTerm/Firefox misbehaviour (#200 comments).

Still open on all versions:
- Logout leaves the login window unresponsive while `borders` runs (#118, Sonoma/Sequoia, unexplained).
- Stage Manager / Mission Control stale borders (#29, #147).

---

## Part B: Evidence gathered on this Mac

All probes were read-only throwaway programs in the session scratchpad. "No-permission context" means an ad-hoc-signed `.app` launched with `open`, so it runs as its own process with no TCC grants (`AXIsProcessTrusted=0`, `CGPreflightScreenCaptureAccess=0`). The terminal context (Ghostty) has Accessibility but no Screen Recording.

### B1. Symbol availability & identity probe (macOS 26.6.2)

- **Symbols**: all ~60 SkyLight symbols JankyBorders uses resolve via `dlsym`, with one exception: `CGSGetConnectionPortById`, which is not exported. That confirms why JankyBorders walks the symbol table.
- **Not present**: `SLSWindowIteratorGetSubLevel`, `SLSWindowIteratorGetSpaceID`.
- **Present** (alternatives): `SLSCopyWindowProperty`, `SLSGetActiveSpace`, `SLSWindowIteratorGetPID`, `SLSWindowIteratorGetBounds`.

Identity results:

| | Terminal context (AX on) | No-permission context |
|---|---|---|
| Owner PID SkyLight == CG | 4/4 | 4/4 |
| Bundle ID resolved | 4/4 | 4/4 |
| `kCGWindowName` title | 0/4 | 0/4 |
| SkyLight `kCGSWindowTitle` | 0/4 | 0/4 |
| AX `kAXWindowsAttribute` | 4/4 OK | 0/4 (`-25211`) |
| Front app via SkyLight == `NSWorkspace.frontmostApplication` | yes | yes |
| Corner radius (iterator) | Chrome/Brave 20, Ghostty 16 | same |

### B2. Event probe (separate helper process with one test window)

The listener registered for SkyLight notifications 0–2000. Results were **identical in the terminal and no-permission contexts**:

| Helper action | Event(s) observed | Approx. latency |
|---|---|---|
| window created | `1325` (12-byte payload: space ID + window ID) | immediate |
| move | `806` | immediate |
| resize | `806` + `807` | immediate |
| miniaturize | `816` | **~0.6 s** (after the genie animation) |
| deminiaturize | `815` | ~0.1 s (at the start of the restore) |
| orderOut | `816` | ~0.4 s (Tahoe hide animation) |
| orderFront | `815` | ~0.15 s |
| close | `816` | ~0.5 s |
| process exit | `804` + `1326`, plus `1508` | immediate |

`808`, `1322` and `723` were not exercised by this scenario.

### B3. Sub-level: narrow binary check

- **Question**: is exported `SLSGetWindowSubLevel` equivalent to JankyBorders' hand-built `mach_msg`?
- **REA not used**: on macOS 26, SkyLight exists only inside the multi-GB dyld shared cache, and extracting it for one function was disproportionate. I disassembled just the function in a live process with `lldb` instead.
- **Observed (`SLSGetWindowSubLevel`, 26.6.2)**:
  - The first argument is ignored; the function uses `SLSMainConnection()`.
  - It calls `CGSWindowGetMappedImpl(conn, wid, 1)`.
  - It sends a MIG request to `CGSGetConnectionPortById(window->cid)` and checks the reply ID `0x7747`, i.e. request `0x76e3`. That's the **same server request** JankyBorders builds by hand.
  - It returns the `int32` sub-level directly.
- **Observed (`CGSWindowGetMappedImpl`)**: with flag `1`, a window not in the local table is fetched from the server with another MIG request, so foreign windows can be mapped on demand.
- **Empirical**: across all 283 windows on this system, the exported call took ~55 µs each (a real round trip) and **every sub-level was 0**.
- **Conclusion (inference)**:
  - Normal app windows have sub-level 0.
  - The exported function appears to be a viable, far less fragile replacement for the raw MIG path.
  - The reason for the author's revert is unknown, so re-validate it in our spike.
  - Either way, we can avoid raw MIG and hidden symbols.

---

## Part C: Licensing & independent implementation

- **JankyBorders is GPL-3.0.** Copying or closely paraphrasing its code would make our app a GPL-3.0 derivative, with source-distribution obligations for the whole app.
- **What we actually need is not its code**:
  1. Private API names, signatures and constants (event IDs, tag meanings). These are interface facts. They're also documented in **yabai, which is MIT-licensed** (confirmed via GitHub API), and verifiable directly, as the probes above show.
  2. General techniques: a separate overlay window ordered relative to the target, bound to its Space, updated on WindowServer notifications. These are ideas, not protected expression.
- **Our design differs anyway**: a rule engine, a Swift/AppKit/SwiftUI app shell, a different module split, single-threaded event handling, no CLI/Mach IPC and no yabai proxying. Writing it from scratch is natural, not a contortion.
- **Caveat**: I've read the JankyBorders source in this session, so this isn't a formal clean room. For stronger provenance:
  - implement from a **facts-only spec** (API signatures, event IDs, behaviours, no code);
  - cross-check declarations against yabai's MIT headers and our own probes;
  - credit JankyBorders and yabai as references in the README.

  This is not legal advice.

---

## Part D: Recommendations

### D1. Feasibility

| Area | Confidence |
|---|---|
| Core (per-app coloured borders, focus states, follow move/resize/hide/close, Spaces, multi-display, persistence, menu-bar GUI) | **High** |
| Title rules (with optional AX) | **Medium-high** |
| Polish (animation timing, Mission Control/Stage Manager, odd window types) | **Medium**: known open problems even in JankyBorders |
| Long-term maintenance | Expect a **check-and-fix cycle for each macOS major release**, because of the private APIs |

### D2. Recommended architecture

**Single process**: one menu-bar app (`LSUIElement`) that contains the engine as a C module.

```
┌────────────────────────── BorderColor.app (LSUIElement, one process) ──────────────────────────┐
│ AppKit shell (Swift): NSStatusItem + NSMenu, app lifecycle, login item, permission prompts      │
│        │ opens on demand                                                                        │
│        ▼                                                                                        │
│ Settings window (SwiftUI hosted in NSWindow) ── edits ──► ConfigStore (Swift, Codable JSON)     │
│                                                              │ save (atomic)   │ compile        │
│                                                              ▼                 ▼                │
│                                          ~/Library/Application Support/…/config.json            │
│                                                                    engine_set_config(rules…)    │
│                                                                                 │               │
│ Border engine (C11 + small Obj-C; no Swift) ◄───────────────────────────────────┘               │
│   skylight loader (dlsym + capability check) · events · window tracker · focus · identity       │
│   (pid→bundle id) · optional AX title provider · rule matcher · overlay renderer                 │
│        │                                                                                        │
│        ▼                                                                                        │
│ SkyLight (private) + CoreGraphics + CoreFoundation (+ ApplicationServices AX, optional)        │
└────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Why single process:
- **No IPC layer**: the GUI calls `engine_set_config()` directly.
- **No daemon**: no launchd plist and no "is the engine running?" synchronisation.
- **Light when the settings window is closed**: the SwiftUI view tree is torn down on close. What stays resident is the status item plus the engine.

The menu-bar icon must stay running anyway, which is your preferred model.

Two details make single-process safe:
- **Overlays are raw SkyLight windows on dedicated connections, not `NSWindow`s.** AppKit therefore never reorders them when our app activates, e.g. when the settings window opens.
- **The engine exposes a narrow C API** (`border_engine.h`). If we later want "borders without a menu-bar icon", the engine can be lifted into a tiny standalone binary watching the same JSON file, with no redesign.

Engine principles:
- **`dlsym` every private symbol at startup**; never hard-link SkyLight. If a *required* symbol is missing, the engine disables itself and the GUI shows "unsupported macOS version" instead of crashing at launch.
- **No raw MIG and no hidden-symbol walking**. Use exported `SLSGetWindowSubLevel` (or skip sub-levels; all observed are 0).
- **Single-threaded on the main run loop**:
  - Moves: a cheap transaction per event.
  - Resizes: coalesced to at most one redraw per frame, using freeze/thaw.
  - AX calls: on a background queue with a short `AXUIElementSetMessagingTimeout` (≈0.25 s), so a hung app can't stall borders. The AX default timeout is several seconds.
- **Bundle ID cache** per PID, invalidated by `NSWorkspace.didTerminateApplicationNotification`.
- **Front-app changes**: public `NSWorkspace.didActivateApplicationNotification`, with SkyLight `1508` as backup.
- **Window-level focus**: SkyLight z-order heuristic by default; AX focused window when Accessibility is granted.

### D3. Proposed GUI architecture

- **Status menu** (AppKit):
  - `Borders enabled` (toggle)
  - `Settings…`
  - `Launch at login` (toggle, `SMAppService.mainApp`)
  - an Accessibility status row, shown only when title rules exist
  - `Quit`
- **Settings window** (SwiftUI in an `NSWindow` via `NSHostingController`, created on open, released on close):
  - **Sidebar**:
    - `Default`
    - `Rules`: app rules shown with app icon + name, never raw bundle IDs (e.g. Cursor's `com.todesktop.230313mzl4w4u92`)
    - `+` menu: *Add running app…* (from `NSWorkspace.runningApplications`, `.regular` policy, with icons), *Choose app…* (`NSOpenPanel` on `/Applications`; bundle ID read from the bundle)
  - **Detail editor**:
    - border enabled toggle
    - active colour (`ColorPicker` → native `NSColorPanel`, opacity supported)
    - inactive colour: *Use dimmed active colour* (default) or a custom colour
    - width (stepper/slider, 1–20 pt)
    - optional *Window title contains…* field, with an AX warning badge and a *Grant…* button when not trusted
    - live preview (a mock window drawn with the resolved style)
    - *Reset to default* and *Remove rule*
  - **Live apply**: edits go to the engine after a ~100 ms debounce, then save to JSON. There's no Save button.
- **View model**: one `@Observable` `ConfigStore` (macOS 14+), the single source of truth. It owns load, save and compile-to-engine.

### D4. Native UI technology

**AppKit shell + SwiftUI content** is the recommendation.

- **SwiftUI for the settings content**: `Form`, `List`, `ColorPicker`, `Stepper`, `Toggle` and `NavigationSplitView` give a native look, including Liquid Glass on 26, for a fraction of the AppKit code. The alternative is `NSTableView` + `NSColorWell` + bindings.
- **AppKit for `NSStatusItem`, `NSMenu`, the window lifecycle and activation**: about 60 lines, and predictable.
- **SwiftUI `MenuBarExtra` is not recommended** as the main surface:
  - `.window`-style extras dismiss when focus moves to `NSColorPanel`.
  - Opening `Settings` from an `LSUIElement` app needs activation-policy workarounds.
  - A plain status menu plus a real settings window avoids both.
- **Pure AppKit** is viable but more code for no user-visible gain. Electron or web views are not considered, per your constraints.

### D5. APIs involved (planned surface, smaller than JankyBorders')

- **Public**:
  - CoreGraphics (drawing, `CGWindowListCopyWindowInfo` as a fallback/diagnostic)
  - CoreFoundation
  - AppKit (`NSRunningApplication`, `NSWorkspace` notifications, `NSColorPanel`)
  - ServiceManagement (`SMAppService`)
  - ApplicationServices AX (optional), `libproc`
- **Private (SkyLight)**:
  - **events**: `SLSMainConnectionID`, `SLSRegisterNotifyProc`, `SLSRequestNotificationsForWindows`, `SLSGetEventPort` / `SLEventCreateNextEvent`
  - **query**: `SLSWindowQueryWindows` + iterator getters (incl. `…GetCornerRadii` on 26+), `SLSGetWindowBounds`, `SLSWindowIsOrderedIn`, `SLSCopyWindowsWithOptionsAndTags`, `SLSGetWindowOwner`, `SLSConnectionGetPID`
  - **Spaces/displays**: `SLSCopyManagedDisplaySpaces`, `SLSCopyManagedDisplays`, `SLSManagedDisplayGetCurrentSpace`, `SLSCopySpacesForWindows`, `SLSMoveWindowsToManagedSpace`
  - **overlays**: `SLSNewConnection`, `SLSNewWindow`, `SLSReleaseWindow`, `SLSSetWindowShape`, `CGSNewRegionWithRect`, `SLSSetWindowTags`, `SLSSetWindowResolution`, `SLSSetWindowOpacity`, `SLSWindowSetShadowProperties`, `SLWindowContextCreate`, `SLSFlushWindowContentRegion`, `SLSWindowFreezeWithOptions` / `Thaw`, `SLSDisableUpdate` / `ReenableUpdate`
  - **transactions**: `SLSTransactionCreate`, `…MoveWindowWithGroup`, `…OrderWindow`, `…SetWindowLevel`, `…SetWindowSubLevel`, `…Commit`
  - **optional**: `SLSGetWindowSubLevel`, `_AXUIElementGetWindow` (to map AX windows to window IDs)
- **Explicitly avoided**: raw MIG messages, `CGSGetConnectionPortById`, `_CFMachPortSetOptions` (unless the spike shows it's needed), Mach bootstrap services, CVDisplayLink proxies.

### D6. Permission requirements

| Capability | Permission | When asked |
|---|---|---|
| Everything in the MVP | **None** | — |
| Title rules, exact focused window | Accessibility | Only when the first title rule is created; the UI explains why |
| Launch at login | None (macOS shows a "background item added" notice) | When the toggle is enabled |
| Screen Recording | **Never** | — |

Distribution and development:
- **Distribution**: outside the App Store, signed with Developer ID and notarized. Private APIs and cross-process window operations rule out the App Sandbox; notarization doesn't block private API use.
- **Development**: sign with a stable identity (Apple Development cert), so the Accessibility grant survives rebuilds. TCC ties the grant to the code signature.

### D7. Identifying applications and windows

| Thing | Key | Notes |
|---|---|---|
| Window (runtime) | `CGWindowID` (`uint32`) | Unique for the window's lifetime; never persisted |
| Process (runtime) | PID via `SLSGetWindowOwner` → `SLSConnectionGetPID` | Cache per window |
| Application (persisted in rules) | **Bundle identifier** | Stable across relaunches, updates and renames. Display name and icon are resolved live for the UI only |
| Fallback app key | Executable path (`proc_pidpath`) | For unbundled processes; stored as `"path"` instead of `"bundleId"` |
| Window title (optional) | AX `kAXTitleAttribute`, re-read on SkyLight `1322` (debounced) | Never persisted; matched at runtime |

### D8. Rule matching design

- **Rule shape**:
  - `{ id, enabled, name?, bundleId?, titleContains?, style: { enabled, color, inactiveColor?, width? } }`
  - `bundleId` is optional, so a rule like *"any window whose title contains 'Production'"* works across browsers.
- **Precedence** (most specific wins; ties go to list order):
  1. `bundleId` + `titleContains`
  2. `titleContains` only
  3. `bundleId` only
  4. `default`
- **Field fallback**:
  - missing `width` → default width
  - missing `inactiveColor` → derived from the global inactive policy
  - `style.enabled = false` → **no border** for matching windows (per-app opt-out)
- **Inactive policy** (global, overridable per rule): `dim` (same colour at ~50% alpha, the default), `same`, `custom` or `hidden`.
  - Qubes-style identification needs the colour visible on **background** windows too, so focus is shown by intensity rather than by hiding.
  - JankyBorders' default is to hide inactive borders; ours is the opposite.
- **Title rules without AX**: never match. The rule shows a warning, and the window falls through to the next rule.
- **Evaluation triggers**: window create, app launch, title change, config change. The result is cached per window, and the window is redrawn only when the resolved style changes.
- **Matching details**: case-insensitive Unicode substring via `CFStringFind(…, kCFCompareCaseInsensitive)`. Regex is out of the MVP, but the schema leaves room for `titleRegex`.

### D9. Configuration storage

- **Format**: JSON at `~/Library/Application Support/<bundle-id>/config.json`.
- **Writes**: atomic (`Data.write(options: .atomic)`).
- **Schema**: versioned, with a tolerant decoder.
- **Why JSON over UserDefaults or a plist**: human-readable, diffable, easy to export/back up, and editable by power users. The UI never requires opening it.

```json
{
  "version": 1,
  "enabled": true,
  "default": { "enabled": true, "color": "#777777", "width": 3 },
  "inactive": { "mode": "dim", "opacity": 0.5 },
  "rules": [
    { "id": "6F1C…", "name": "Terminal",           "bundleId": "com.apple.Terminal",   "style": { "enabled": true, "color": "#A855F7", "width": 3 } },
    { "id": "0B7A…", "name": "VS Code",            "bundleId": "com.microsoft.VSCode", "style": { "enabled": true, "color": "#22C55E", "width": 3 } },
    { "id": "93D2…", "name": "Chrome",             "bundleId": "com.google.Chrome",    "style": { "enabled": true, "color": "#3B82F6", "width": 3 } },
    { "id": "C41E…", "name": "Production browser", "bundleId": "com.google.Chrome", "titleContains": "Production",
      "style": { "enabled": true, "color": "#FF0000", "inactiveColor": "#FF000080", "width": 5 } }
  ]
}
```

- **Colours**: `#RRGGBB` or `#RRGGBBAA` (sRGB).
- **Width**: in points, clamped to 1–20.

### D10. Known risks & fragility

1. **Private API drift each macOS release**: symbols, signatures, event numbers, tag bits.
   - Mitigation: `dlsym` + capability check, one isolated `skylight` module, a test pass on each June beta.
2. **Window-filter heuristics** (undocumented tags) can include junk windows or miss real ones (cf. JankyBorders 1.9.0 regression #200).
   - Mitigation: a conservative filter, per-app on/off in rules, and a hidden "debug: show window info" menu item.
3. **Tahoe window animations**: borders linger ~0.4–0.6 s on close/minimize and appear ~0.1–0.2 s early on restore (measured).
   - Possible improvement: watch `SLSGetWindowTransform` / alpha only while an animation is in flight. Not MVP.
4. **Mission Control / Exposé / Stage Manager**: stale borders, because there's no notification for WindowServer-driven transforms. Unknown whether a cheap detection signal exists. Not MVP.
5. **WindowServer memory/GPU cost**: full-rect transparent backing stores per border (inference from JankyBorders' design; reports #128, #188).
   - Spike will measure: full rect vs ring-shaped region vs four edge strips, and 1× vs 2× backing.
6. **Logout hang** reported with JankyBorders (#118, unexplained). Our spike must test logout early.
7. **Click-through**: the overlay extends past the window edge, where macOS resize handles live. It must not intercept clicks. Verify the tag/event-ignore setting in the spike.
8. **Focus heuristic errors** without AX: panels, multiple windows, apps with odd window structures.
9. **AX hangs**: a frozen app can block AX calls. Mitigation: short messaging timeout, background queue.
10. **No App Store distribution** (private APIs); Developer ID + notarization only.
11. **Title churn**: browser titles change per tab, so frequent re-matching. Debounce and cache.

### D11. Compatibility across macOS versions

- **Minimum target: macOS 14 Sonoma** (Observation, modern SwiftUI `Form`/`ColorPicker`, `SMAppService`). **Primary: 26**. Also test 15.
- **≤ 15**: corner radius fixed (~9–10 pt); `SLSWindowIteratorGetCornerRadii` absent, so use the fallback constant.
- **26**:
  - per-window radii via `dlsym`
  - resize freeze/reshape path
  - delayed hide events
  - glass windows (radius 16–20+)
- **27**:
  - reportedly released and Apple Silicon only
  - untested; JankyBorders has open 27 issues (#200 comments, #201)
  - needs a validation pass before we claim support
- **Never hard-link SkyLight.** JankyBorders hit both link-time (#174) and header-level (#190) build breakage from SDK changes.

### D12. Proposed project structure

```
border-color-macos/
├── Package.swift                 # SwiftPM: C engine target + Swift app target (no third-party deps)
├── Makefile                      # build → assemble .app → codesign → run (no hand-edited .xcodeproj)
├── Resources/
│   ├── Info.plist                # LSUIElement=YES, bundle id, LSMinimumSystemVersion=14.0
│   └── BorderColor.entitlements  # (none needed beyond hardened runtime)
├── Sources/
│   ├── BorderEngine/             # C11 + minimal Obj-C, no Swift
│   │   ├── include/border_engine.h   # the ONLY public API (start/stop/set_config/enable/snapshot)
│   │   ├── skylight.h / skylight.c   # private decls + dlsym loader + capability report
│   │   ├── events.c                  # notification registration & dispatch
│   │   ├── tracker.c / tracker.h     # window table, discovery, filter, spaces, focus
│   │   ├── overlay.c / overlay.h     # border window create/draw/reshape/move/order/hide
│   │   ├── identity.m                # pid → bundle id (NSRunningApplication), AX title provider
│   │   └── rules.c / rules.h         # compiled rules + precedence matching
│   └── BorderColor/              # Swift app
│       ├── AppDelegate.swift, StatusMenu.swift, LoginItem.swift, Permissions.swift
│       ├── Engine.swift              # thin Swift wrapper over border_engine.h
│       ├── Config/ConfigModel.swift, ConfigStore.swift
│       └── Settings/SettingsWindowController.swift, SettingsView.swift,
│                    RuleListView.swift, RuleEditorView.swift, AppPicker.swift, BorderPreview.swift
├── Tests/
│   ├── RulesTests/               # precedence & fallback (C via Swift test target)
│   └── ConfigTests/              # decode/encode/migration
└── docs/INVESTIGATION.md
```

Xcode can open `Package.swift` directly for debugging. If you'd rather have a classic `.xcodeproj`, that's a fine alternative.

### D13. MVP scope

**Step 0: engine spike (go/no-go)**. A command-line engine that draws a fixed-colour border on every window. It validates:
- events, ordering, click-through
- Spaces, fullscreen, minimize/hide, multi-display
- idle and drag CPU/GPU, and WindowServer memory (full-rect vs ring/edge overlays)
- logout behaviour
- `SLSGetWindowSubLevel`

**MVP (your list, confirmed):**
- [ ] Menu-bar app (`LSUIElement`); engine starts automatically at launch
- [ ] Status menu: enable/disable, Settings…, Launch at login, Quit
- [ ] Borders follow move/resize/hide/minimize/close, Spaces, multiple displays; native fullscreen = no visible border
- [ ] Default border: colour, width, enabled
- [ ] Per-app rules by bundle ID: colour, width, enabled
- [ ] Add rules from running apps (with icons) or by choosing an app bundle
- [ ] Native colour picker; live preview; on-screen borders update immediately
- [ ] Active/inactive handling: global inactive policy (dimmed by default) + optional per-rule inactive colour
- [ ] Reset rule to default; remove rule
- [ ] JSON persistence; rules survive restart

**Phase 2 (deliberately not MVP):**
- Title rules with the on-demand Accessibility flow. The schema supports them from day 1, so no migration is needed.
- AX-exact focus.
- Rule reordering.

**Later:**
- regex titles
- Mission Control / Stage Manager handling
- animation-accurate hide/show
- gradient/glow styles
- import/export
- hide the menu-bar icon (needs a standalone engine)
- per-display settings

---

## Appendix: SkyLight notification IDs used

| ID | Meaning | Payload | Verified on 26.6.2 |
|---|---|---|---|
| 723 | window update (focus hint) | `uint32 wid` | no |
| 804 | window closed | `uint32 wid` | yes |
| 806 | window moved | `uint32 wid` | yes |
| 807 | window resized | `uint32 wid` | yes |
| 808 | window reordered | `uint32 wid` | no |
| 811 | window level changed | `uint32 wid` | no |
| 815 | window unhidden / ordered in | `uint32 wid` | yes |
| 816 | window hidden / ordered out | `uint32 wid` | yes |
| 1322 | window title changed | `uint32 wid` | no |
| 1325 | window created / added to Space | `uint64 sid, uint32 wid` | yes |
| 1326 | window destroyed / removed from Space | `uint64 sid, uint32 wid` | yes |
| 1401 | Space changed | — | no |
| 1508 | front application changed | — | yes |

Window-specific events (8xx, 1322) are only delivered for windows registered via `SLSRequestNotificationsForWindows`.

## Sources

- JankyBorders source @ `a7297ca`; commits `f969290`, `1dcf347`, `bb3e255`, `13c2e69`, `ef99c4d`, `3a9d75c`, `7892941`, `a56a76a`
- JankyBorders issues: #29, #118, #128, #147, #160, #172, #174, #182, #188, #190, #199, #200, #201 (github.com/FelixKratz/JankyBorders/issues)
- yabai licence (MIT): github.com/koekeishiya/yabai
- macOS 27 release reports: [blakecrosley.com, macOS 27 release notes](https://blakecrosley.com/blog/macos-27-release-notes), [Macworld macOS versions list](https://www.macworld.com/article/672681/list-of-all-macos-versions-including-the-latest-macos.html), [vnreview: macOS 27 Mac compatibility](https://vnreview.vn/threads/apple-xac-nhan-macos-27-se-khong-tuong-thich-voi-4-may-mac-nay.84164/latest)
- Local probes (scratchpad, not in repo): `probe.m` (symbols/identity/titles), `events.m` (notifications), `sublevel.m`, `lldb` disassembly of `SLSGetWindowSubLevel` / `CGSWindowGetMappedImpl`
