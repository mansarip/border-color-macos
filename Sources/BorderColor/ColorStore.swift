import AppKit

/// Border colour per app, keyed by bundle identifier and saved as JSON in
/// ~/Library/Application Support/BorderColor/config.json. An app gets the next unused
/// palette colour the first time it is seen; the user can change it from the menu.
@MainActor
final class ColorStore {
    private struct Config: Codable {
        var version = 1
        var enabled = true
        var width = 4.0  // points
        var colors: [String: String] = [:]  // bundle id → "#RRGGBB"

        init() {}

        init(from decoder: Decoder) throws {
            let container = try decoder.container(keyedBy: CodingKeys.self)
            version = try container.decodeIfPresent(Int.self, forKey: .version) ?? 1
            enabled = try container.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
            width = try container.decodeIfPresent(Double.self, forKey: .width) ?? 4
            colors = try container.decodeIfPresent([String: String].self, forKey: .colors) ?? [:]
        }
    }

    static let palette = [
        "#3B82F6", "#22C55E", "#A855F7", "#F97316", "#EF4444", "#14B8A6",
        "#EAB308", "#EC4899", "#6366F1", "#84CC16", "#06B6D4", "#F43F5E",
    ]

    private var config = Config()
    private let fileURL: URL
    private var saveTask: Task<Void, Never>?

    init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        fileURL = support.appendingPathComponent("BorderColor/config.json")
        if let data = try? Data(contentsOf: fileURL),
           let loaded = try? JSONDecoder().decode(Config.self, from: data) {
            config = loaded
        }
    }

    var enabled: Bool {
        get { config.enabled }
        set {
            config.enabled = newValue
            scheduleSave()
        }
    }

    var width: Double {
        get { config.width }
        set {
            config.width = newValue
            scheduleSave()
        }
    }

    /// The colour for windows of `pid`, or nil if it should get no border
    /// (background processes, menu bar apps, this app).
    func borderColor(forPID pid: pid_t) -> NSColor? {
        guard pid != getpid(),
              let app = NSRunningApplication(processIdentifier: pid),
              app.activationPolicy == .regular,
              let bundleID = app.bundleIdentifier else { return nil }
        return color(forBundleID: bundleID)
    }

    func color(forBundleID bundleID: String) -> NSColor {
        if let hex = config.colors[bundleID], let color = NSColor(hex: hex) {
            return color
        }
        let hex = nextPaletteColor()
        config.colors[bundleID] = hex
        scheduleSave()
        return NSColor(hex: hex) ?? .systemBlue
    }

    func setColor(_ color: NSColor, forBundleID bundleID: String) {
        config.colors[bundleID] = color.hexString
        scheduleSave()
    }

    func saveNow() {
        saveTask?.cancel()
        save()
    }

    private func nextPaletteColor() -> String {
        let used = Set(config.colors.values.map { $0.uppercased() })
        return Self.palette.first { !used.contains($0) }
            ?? Self.palette[config.colors.count % Self.palette.count]
    }

    // The colour panel reports changes continuously while dragging; write once it settles.
    private func scheduleSave() {
        saveTask?.cancel()
        saveTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(500))
            guard !Task.isCancelled else { return }
            self?.save()
        }
    }

    private func save() {
        do {
            try FileManager.default.createDirectory(
                at: fileURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(config).write(to: fileURL, options: .atomic)
        } catch {
            NSLog("BorderColor: could not save settings: \(error)")
        }
    }
}

extension NSColor {
    convenience init?(hex: String) {
        let digits = hex.hasPrefix("#") ? String(hex.dropFirst()) : hex
        guard digits.count == 6, let value = UInt32(digits, radix: 16) else { return nil }
        self.init(srgbRed: CGFloat((value >> 16) & 0xFF) / 255,
                  green: CGFloat((value >> 8) & 0xFF) / 255,
                  blue: CGFloat(value & 0xFF) / 255,
                  alpha: 1)
    }

    var hexString: String {
        let rgb = usingColorSpace(.sRGB) ?? self
        func byte(_ component: CGFloat) -> Int { Int((min(max(component, 0), 1) * 255).rounded()) }
        return String(format: "#%02X%02X%02X", byte(rgb.redComponent), byte(rgb.greenComponent), byte(rgb.blueComponent))
    }
}
