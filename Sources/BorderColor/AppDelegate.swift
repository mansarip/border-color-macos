import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let store = ColorStore()
    private var statusMenu: StatusMenu?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let engineError = Engine.start(store: store)
        statusMenu = StatusMenu(store: store, engineError: engineError)
    }

    func applicationWillTerminate(_ notification: Notification) {
        store.saveNow()
    }
}
