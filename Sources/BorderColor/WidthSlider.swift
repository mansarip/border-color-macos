import AppKit

/// A menu row with a slider for the border width. Borders update while dragging, so the
/// menu can stay open while you find the right thickness.
@MainActor
final class WidthSlider: NSView {
    static let range = 1.0...20.0

    private let slider: NSSlider
    private let valueLabel = NSTextField(labelWithString: "")
    private let onChange: (Double) -> Void

    init(width: Double, onChange: @escaping (Double) -> Void) {
        self.onChange = onChange
        slider = NSSlider(value: width, minValue: Self.range.lowerBound, maxValue: Self.range.upperBound,
                          target: nil, action: nil)
        super.init(frame: NSRect(x: 0, y: 0, width: 250, height: 52))

        let title = NSTextField(labelWithString: "Border Width")
        title.font = .menuFont(ofSize: 0)
        title.frame = NSRect(x: 20, y: 28, width: 150, height: 18)
        valueLabel.font = .menuFont(ofSize: 0)
        valueLabel.textColor = .secondaryLabelColor
        valueLabel.alignment = .right
        valueLabel.frame = NSRect(x: 170, y: 28, width: 60, height: 18)
        slider.frame = NSRect(x: 18, y: 6, width: 214, height: 20)
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(sliderMoved)
        [title, valueLabel, slider].forEach(addSubview)
        updateLabel(width)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    @objc private func sliderMoved() {
        let width = slider.doubleValue.rounded()
        guard valueLabel.stringValue != label(for: width) else { return }
        updateLabel(width)
        onChange(width)
    }

    private func updateLabel(_ width: Double) {
        valueLabel.stringValue = label(for: width)
    }

    private func label(for width: Double) -> String {
        "\(Int(width)) pt"
    }
}
