// Draws the app icon (three overlapping windows, each with its own border colour) into an
// .iconset folder; `make icon` turns that into Resources/AppIcon.icns.
// Usage: swift scripts/make-icon.swift <output.iconset>
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let canvas: CGFloat = 1024

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

/// The macOS icon outline: a superellipse filling the standard 824 pt tile.
func squircle(in rect: CGRect) -> CGPath {
    let path = CGMutablePath()
    let exponent: CGFloat = 2 / 5
    for step in 0...720 {
        let t = CGFloat(step) / 720 * 2 * .pi
        let c = cos(t), s = sin(t)
        let x = rect.midX + rect.width / 2 * (c < 0 ? -1 : 1) * pow(abs(c), exponent)
        let y = rect.midY + rect.height / 2 * (s < 0 ? -1 : 1) * pow(abs(s), exponent)
        step == 0 ? path.move(to: CGPoint(x: x, y: y)) : path.addLine(to: CGPoint(x: x, y: y))
    }
    path.closeSubpath()
    return path
}

/// Rect given in top-left coordinates on the 1024 canvas, converted to CoreGraphics' bottom-left.
func box(_ x: CGFloat, _ top: CGFloat, _ width: CGFloat, _ height: CGFloat) -> CGRect {
    CGRect(x: x, y: canvas - top - height, width: width, height: height)
}

func roundedRect(_ rect: CGRect, _ radius: CGFloat) -> CGPath {
    CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil)
}

func drawWindow(_ ctx: CGContext, frame: CGRect, border: UInt32, front: Bool = false) {
    let radius: CGFloat = 46, borderWidth: CGFloat = 30, titleHeight: CGFloat = 54

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -14), blur: 36, color: rgb(0x000000, 0.45))
    ctx.addPath(roundedRect(frame, radius))
    ctx.setFillColor(rgb(border))
    ctx.fillPath()
    ctx.restoreGState()

    let inner = frame.insetBy(dx: borderWidth, dy: borderWidth)
    let innerPath = roundedRect(inner, radius - borderWidth)
    ctx.saveGState()
    ctx.addPath(innerPath)
    ctx.clip()
    ctx.setFillColor(rgb(0xF8FAFC))
    ctx.fill(inner)
    let title = CGRect(x: inner.minX, y: inner.maxY - titleHeight, width: inner.width, height: titleHeight)
    ctx.setFillColor(rgb(0xE2E8F0))
    ctx.fill(title)
    ctx.setFillColor(rgb(0xB6C2D2))
    for i in 0..<3 {
        let d: CGFloat = 18
        ctx.fillEllipse(in: CGRect(x: inner.minX + 26 + CGFloat(i) * 30, y: title.midY - d / 2, width: d, height: d))
    }
    ctx.setFillColor(rgb(0xDCE3EC))
    // Content lines only on the front window; on the others they would peek out as stubs.
    let lines: [CGFloat] = front ? [0.62, 0.78, 0.48] : []
    for (i, fraction) in lines.enumerated() {
        ctx.addPath(roundedRect(CGRect(x: inner.minX + 30, y: title.minY - 52 - CGFloat(i) * 42,
                                       width: (inner.width - 60) * fraction, height: 18), 9))
    }
    ctx.fillPath()
    ctx.restoreGState()
}

func renderIcon(pixels: Int) -> CGImage {
    let ctx = CGContext(data: nil, width: pixels, height: pixels, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.scaleBy(x: CGFloat(pixels) / canvas, y: CGFloat(pixels) / canvas)
    ctx.interpolationQuality = .high

    let tile = squircle(in: box(100, 100, 824, 824))
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 20, color: rgb(0x000000, 0.3))
    ctx.addPath(tile)
    ctx.setFillColor(rgb(0x1E293B))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(tile)
    ctx.clip()
    let background = CGGradient(colorsSpace: nil, colors: [rgb(0x334766), rgb(0x111827)] as CFArray,
                                locations: [0, 1])!
    ctx.drawLinearGradient(background, start: CGPoint(x: 0, y: 924), end: CGPoint(x: 0, y: 100), options: [])
    drawWindow(ctx, frame: box(186, 214, 500, 370), border: 0xF97316)
    drawWindow(ctx, frame: box(262, 330, 500, 370), border: 0x22C55E)
    drawWindow(ctx, frame: box(338, 446, 500, 370), border: 0x3B82F6, front: true)
    ctx.restoreGState()

    ctx.addPath(tile)
    ctx.setStrokeColor(rgb(0xFFFFFF, 0.12))
    ctx.setLineWidth(3)
    ctx.strokePath()
    return ctx.makeImage()!
}

guard CommandLine.arguments.count == 2 else {
    FileHandle.standardError.write("usage: swift make-icon.swift <output.iconset>\n".data(using: .utf8)!)
    exit(1)
}
let output = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = scale == 1 ? "icon_\(points)x\(points).png" : "icon_\(points)x\(points)@2x.png"
        let destination = CGImageDestinationCreateWithURL(output.appendingPathComponent(name) as CFURL,
                                                          UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, renderIcon(pixels: points * scale), nil)
        guard CGImageDestinationFinalize(destination) else { fatalError("could not write \(name)") }
    }
}
