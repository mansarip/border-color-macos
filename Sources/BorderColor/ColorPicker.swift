import AppKit

/// Drives the shared macOS colour panel for one app at a time; changes apply live.
@MainActor
final class ColorPicker: NSObject {
    private let store: ColorStore
    private var bundleID: String?

    init(store: ColorStore) {
        self.store = store
    }

    func show(bundleID: String, appName: String) {
        let panel = NSColorPanel.shared
        // Detach first so setting the new app's colour doesn't recolour the previous app.
        panel.setTarget(nil)
        panel.setAction(nil)
        self.bundleID = bundleID
        panel.showsAlpha = false
        panel.isContinuous = true
        panel.color = store.color(forBundleID: bundleID)
        panel.title = "\(appName) Border"
        panel.setTarget(self)
        panel.setAction(#selector(colorChanged(_:)))
        NSApp.activate()
        panel.makeKeyAndOrderFront(nil)
    }

    @objc private func colorChanged(_ sender: NSColorPanel) {
        guard let bundleID else { return }
        store.setColor(sender.color, forBundleID: bundleID)
        Engine.refreshColors()
    }
}
