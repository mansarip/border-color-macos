import AppKit
import ServiceManagement

/// The menu bar item. Each running app is a row showing its icon framed in its border
/// colour; choosing a row opens the colour panel for that app.
@MainActor
final class StatusMenu: NSObject, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let store: ColorStore
    private let picker: ColorPicker
    private let engineError: String?

    init(store: ColorStore, engineError: String?) {
        self.store = store
        self.engineError = engineError
        picker = ColorPicker(store: store)
        super.init()
        statusItem.button?.image = NSImage(systemSymbolName: "square.dashed", accessibilityDescription: "Border Color")
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        if let engineError {
            menu.addItem(disabledItem("Borders are unavailable on this macOS version"))
            menu.addItem(disabledItem(engineError))
            menu.addItem(.separator())
        }

        menu.addItem(.sectionHeader(title: "Border Colours"))
        for app in runningApps() {
            guard let bundleID = app.bundleIdentifier else { continue }
            let item = NSMenuItem(title: app.localizedName ?? bundleID, action: #selector(chooseColor(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = bundleID
            item.image = framedIcon(app.icon, color: store.color(forBundleID: bundleID))
            menu.addItem(item)
        }

        menu.addItem(.separator())
        let width = NSMenuItem()
        width.view = WidthSlider(width: store.width) { [store] value in
            store.width = value
            Engine.setWidth(value)
        }
        menu.addItem(width)

        menu.addItem(.separator())
        let borders = NSMenuItem(title: "Show Borders", action: #selector(toggleBorders), keyEquivalent: "")
        borders.target = self
        borders.state = store.enabled ? .on : .off
        menu.addItem(borders)

        let dim = NSMenuItem(title: "Dim Inactive Borders", action: #selector(toggleDimInactive), keyEquivalent: "")
        dim.target = self
        dim.state = store.dimInactive ? .on : .off
        menu.addItem(dim)

        let login = NSMenuItem(title: "Launch at Login", action: #selector(toggleLaunchAtLogin), keyEquivalent: "")
        login.target = self
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(login)

        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit Border Color", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    @objc private func chooseColor(_ sender: NSMenuItem) {
        guard let bundleID = sender.representedObject as? String else { return }
        picker.show(bundleID: bundleID, appName: sender.title)
    }

    @objc private func toggleBorders() {
        store.enabled.toggle()
        Engine.setEnabled(store.enabled)
    }

    @objc private func toggleDimInactive() {
        store.dimInactive.toggle()
        Engine.setDimInactive(store.dimInactive)
    }

    @objc private func toggleLaunchAtLogin() {
        let service = SMAppService.mainApp
        do {
            if service.status == .enabled {
                try service.unregister()
            } else {
                try service.register()
            }
        } catch {
            NSLog("BorderColor: launch at login change failed: \(error)")
        }
        if service.status == .requiresApproval {
            SMAppService.openSystemSettingsLoginItems()
        }
    }

    private func runningApps() -> [NSRunningApplication] {
        var seen = Set<String>()
        return NSWorkspace.shared.runningApplications
            .filter { app in
                guard app.activationPolicy == .regular, app.processIdentifier != getpid(),
                      let bundleID = app.bundleIdentifier else { return false }
                return seen.insert(bundleID).inserted
            }
            .sorted { ($0.localizedName ?? "").localizedCaseInsensitiveCompare($1.localizedName ?? "") == .orderedAscending }
    }

    private func framedIcon(_ icon: NSImage?, color: NSColor) -> NSImage {
        let image = NSImage(size: NSSize(width: 22, height: 22))
        image.lockFocus()
        let bounds = NSRect(origin: .zero, size: image.size)
        color.setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 6, yRadius: 6).fill()
        icon?.draw(in: bounds.insetBy(dx: 3, dy: 3))
        image.unlockFocus()
        return image
    }

    private func disabledItem(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }
}
