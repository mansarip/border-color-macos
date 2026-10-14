# Border Color

A coloured border around every app window, so you can tell windows apart at a glance when many are open. Inspired by Qubes OS, but the colours are just your choice per app, not a security boundary.

A small menu bar app: each app gets a colour automatically. Click the app in the menu to choose a different one.

## Build and run

Requires macOS 14+ and the Swift 6 toolchain (Xcode or Command Line Tools).

```sh
make run     # build build/BorderColor.app and launch it
make debug   # same, in the foreground with engine logging
make clean
```

## Use

- **Menu bar icon → app name:** opens the colour panel for that app. Changes apply immediately.
- **Border Width:** slider (1–20 pt); borders update while you drag.
- **Show Borders:** turns all borders on or off.
- **Dim Inactive Borders:** when on (default), only the focused window's border is at full colour and the others are half transparent. Turn it off to make every border solid.
- **Launch at Login:** starts the app when you log in.

Settings live in `~/Library/Application Support/BorderColor/config.json` (width, dimming, and bundle id → colour). You never need to edit it.

## How it works

- `Sources/BorderEngine` (C) talks to the WindowServer through private SkyLight functions, resolved at runtime. No permissions are needed: no Accessibility, no Screen Recording.
- Each border is four thin click-through windows around the target window, stacked directly below it and kept on its Space. Strips instead of one big transparent window keep memory small (~3 MB vs ~38 MB for a large Retina window).
- WindowServer notifications drive the borders: move, resize, hide/minimize, restore, close, Space changes and focus changes.
- While the Dock animates a window (genie minimize or restore), its border is hidden. The animation's start event doesn't name the window, but the animating window carries a tag bit until the matching end event.
- Mission Control and App Exposé shrink windows into thumbnails without any event, and fade the normal borders out. When they open, the Dock creates a full-screen backdrop window; while it is up, the engine polls the window list (which reports each thumbnail's on-screen rect) at 120 Hz and draws a second, full-opacity border above each thumbnail. Polling stops when the backdrop goes away.
- `Sources/BorderColor` (Swift, AppKit) contains the menu, the colour panel and the JSON settings.

## Known limitations

- Closing or hiding a window (no genie) still leaves its border on screen for ~0.3 s. macOS sends no event when those animations start, only when they end.
- Stage Manager isn't handled.
- Mission Control detection relies on Dock behaviour observed on macOS 26 (a new full-screen Dock window that rises to level 18), so it may need adjusting on other versions.
- Private APIs can change with macOS updates, and the app can't be distributed through the Mac App Store.

## Credits

[JankyBorders](https://github.com/FelixKratz/JankyBorders) (GPL-3.0) and [yabai](https://github.com/koekeishiya/yabai) (MIT) were studied as references for the WindowServer APIs. This is an independent implementation; see `docs/INVESTIGATION.md`.
