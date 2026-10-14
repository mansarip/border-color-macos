// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BorderColor",
    platforms: [.macOS(.v14)],
    targets: [
        // C engine: talks to the WindowServer through private SkyLight APIs (resolved at runtime).
        .target(
            name: "BorderEngine",
            linkerSettings: [.linkedFramework("CoreGraphics"), .linkedFramework("CoreFoundation")]
        ),
        // Menu bar app: colour picker and settings.
        .executableTarget(
            name: "BorderColor",
            dependencies: ["BorderEngine"]
        ),
    ]
)
