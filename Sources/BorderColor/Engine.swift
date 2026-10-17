import AppKit
import BorderEngine

/// Swift side of the C border engine. The engine asks for each window's colour through
/// a C callback, answered from the ColorStore.
@MainActor
enum Engine {
    private static var store: ColorStore?

    /// Starts drawing borders. Returns a message if this macOS version lacks a private
    /// API the engine needs.
    static func start(store: ColorStore) -> String? {
        self.store = store
        be_set_width(store.width)
        be_set_corner_radius(store.cornerRadius ?? -1)
        be_set_dim_inactive(store.dimInactive)
        let started = be_start({ pid, color, _ in
            MainActor.assumeIsolated {
                guard let rgb = Engine.store?.borderColor(forPID: pid)?.usingColorSpace(.sRGB) else {
                    return false
                }
                color?.pointee = be_color(r: rgb.redComponent, g: rgb.greenComponent, b: rgb.blueComponent, a: 1)
                return true
            }
        }, nil)
        guard started else {
            return "Missing system API: \(String(cString: be_missing_symbol()))"
        }
        be_set_enabled(store.enabled)
        return nil
    }

    static func setEnabled(_ enabled: Bool) {
        be_set_enabled(enabled)
    }

    static func setWidth(_ width: Double) {
        be_set_width(width)
    }

    /// nil follows each window's own corners.
    static func setCornerRadius(_ radius: Double?) {
        be_set_corner_radius(radius ?? -1)
    }

    static func setDimInactive(_ dim: Bool) {
        be_set_dim_inactive(dim)
    }

    static func refreshColors() {
        be_refresh_colors()
    }
}
