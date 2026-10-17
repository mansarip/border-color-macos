import AppKit

/// A menu row with a titled slider that snaps to whole numbers. Borders update while
/// dragging, so the menu can stay open while you find the right value.
@MainActor
final class MenuSlider: NSView {
    private let slider: NSSlider
    private let valueLabel = NSTextField(labelWithString: "")
    private let format: (Double) -> String
    private let onChange: (Double) -> Void

    init(title: String, value: Double, range: ClosedRange<Double>,
         format: @escaping (Double) -> String, onChange: @escaping (Double) -> Void) {
        self.format = format
        self.onChange = onChange
        slider = NSSlider(value: value, minValue: range.lowerBound, maxValue: range.upperBound,
                          target: nil, action: nil)
        super.init(frame: NSRect(x: 0, y: 0, width: 250, height: 52))

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .menuFont(ofSize: 0)
        titleLabel.frame = NSRect(x: 20, y: 28, width: 150, height: 18)
        valueLabel.font = .menuFont(ofSize: 0)
        valueLabel.textColor = .secondaryLabelColor
        valueLabel.alignment = .right
        valueLabel.frame = NSRect(x: 170, y: 28, width: 60, height: 18)
        slider.frame = NSRect(x: 18, y: 6, width: 214, height: 20)
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(sliderMoved)
        [titleLabel, valueLabel, slider].forEach(addSubview)
        valueLabel.stringValue = format(slider.doubleValue.rounded())
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    @objc private func sliderMoved() {
        let value = slider.doubleValue.rounded()
        guard valueLabel.stringValue != format(value) else { return }
        valueLabel.stringValue = format(value)
        onChange(value)
    }
}
