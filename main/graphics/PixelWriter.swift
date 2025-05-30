fileprivate let Log = Logger(tag: "PixelWriter")

struct PixelWriter {

    static var defaultFont: Font?

    let buffer: UnsafeMutableBufferPointer<UInt16>
    let screenSize: Size
    var rect: Rect?
    var font: Font? = nil

    private var offset: Point { rect?.origin ?? .zero }
    private func getRect() -> Rect { rect ?? Rect(origin: .zero, size: screenSize) }

    func clear(color: Color = .black) {
        buffer.initialize(repeating: color.rgb565)
    }

    func drawPixel(at point: Point, color: Color) {
        let point = point + offset
        if getRect().contains(point) {
            buffer[Int(point.y * screenSize.width) + Int(point.x)] = color.rgb565
        }
    }

    func drawLine(from: Point, to: Point, color: Color) {
        let from = from + offset
        let to = to + offset
        let color = color.rgb565
        if from.x == to.x {
            let startY = min(from.y, to.y, 0)
            let endY = max(from.y, to.y, screenSize.height - 1)
            for y in startY...endY {
                buffer[Int(y * screenSize.width) + Int(from.x)] = color
            }
        } else if from.y == to.y {
            let startX = min(from.x, to.x, 0)
            let endX = max(from.x, to.x, screenSize.width - 1)
            for x in startX...endX {
                buffer[Int(from.y) * screenSize.width + Int(x)] = color
            }
        } else {
            Log.error("Only horizontal or vertical lines are supported.")
        }
    }

    func drawRect(rect: Rect, color: Color) {
        let rect = Rect(origin: rect.origin + offset, size: rect.size)
        let startX = max(0, rect.minX)
        let endX = min(screenSize.width, rect.maxX)
        let startY = max(0, rect.minY)
        let endY = min(screenSize.height, rect.maxY)
        let color = color.rgb565

        for x in startX..<endX {
            buffer[Int(startY) * Int(screenSize.width) + x] = color
            buffer[Int(endY - 1) * Int(screenSize.width) + x] = color
        }
        for y in startY..<endY {
            buffer[y * Int(screenSize.width) + startX] = color
            buffer[y * Int(screenSize.width) + endX - 1] = color
        }
    }

    func fillRect(rect: Rect, color: Color) {
        let rect = Rect(origin: rect.origin + offset, size: rect.size)
        let startX = max(0, rect.minX)
        let endX = min(screenSize.width, rect.maxX)
        let startY = max(0, rect.minY)
        let endY = min(screenSize.height, rect.maxY)
        let color = color.rgb565

        for y in startY..<endY {
            for x in startX..<endX {
                buffer[y * Int(screenSize.width) + x] = color
            }
        }
    }

    func drawText(_ text: String, at point: Point, fontSize: Int, color: Color) {
        guard let font = font ?? PixelWriter.defaultFont else {
            return
        }
        let color = color.rgb565
        let point = point + offset
        font.fontSize = fontSize
        font.drawBitmap(text, maxWidth: screenSize.width - point.x) { (pixelPoint, value) in
            let point = pixelPoint + point
            if value > 0 {
                buffer[point.y * screenSize.width + point.x] = color
            }
        }
    }

    func drawBitmap(_ data: (size: Size, bitmap: [UInt32]), at point: Point, color: Color) {
        let rowCount = (data.size.width + 31) / 32
        var row = 0
        for y in 0..<data.size.height {
            for x in 0..<data.size.width {
                let pixelIndex = row + x / 32
                let bitIndex = x % 32
                if (data.bitmap[pixelIndex] & (1 << (31 - bitIndex))) != 0 {
                    buffer[Int((point.y + y) * screenSize.width) + Int(point.x + x)] = color.rgb565
                }
            }
            row += rowCount
        }
    }
}
