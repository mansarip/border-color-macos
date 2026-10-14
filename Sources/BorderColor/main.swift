import AppKit

let delegate = AppDelegate()
NSApplication.shared.delegate = delegate
NSApplication.shared.setActivationPolicy(.accessory)  // menu bar only, no Dock icon
NSApplication.shared.run()
