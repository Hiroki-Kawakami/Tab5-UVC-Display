fileprivate let Log = Logger(tag: "Drawable")

class Drawable<P: Pixel> {

    let buffer: UnsafeMutableBufferPointer<P>
    let screenSize: Size

    init(buffer: UnsafeMutablePointer<P>, screenSize: Size) {
        self.buffer = UnsafeMutableBufferPointer<P>(start: buffer, count: screenSize.width * screenSize.height)
        self.screenSize = screenSize
    }

    subscript(_ point: Point) -> P {
        get {
            let index = point.y * screenSize.width + point.x
            return buffer[index]
        }
        set {
            let index = point.y * screenSize.width + point.x
            buffer[index] = newValue
        }
    }
    subscript(x: Int, y: Int) -> P {
        get { return self[Point(x: x, y: y)] }
        set { self[Point(x: x, y: y)] = newValue }
    }

    func clear(color: Color = .black) {
        if color == .black {
            buffer.initialize(repeating: P.black)
        } else if color == .white {
            buffer.initialize(repeating: P.white)
        } else {
            let pixelColor = color.pixel(type: P.self)
            buffer.initialize(repeating: pixelColor)
        }
    }

    func drawPixel(at point: Point, color: Color) {
        if point.x < 0 || point.x >= screenSize.width || point.y < 0 || point.y >= screenSize.height {
            return
        }
        self[point] = color.pixel(type: P.self)
    }

    func drawLine(from: Point, to: Point, color: Color) {
        let color = color.pixel(type: P.self)
        if from.x == to.x {
            let startY = min(from.y, to.y, 0)
            let endY = max(from.y, to.y, screenSize.height - 1)
            for y in startY...endY {
                self[from.x, y] = color
            }
        } else if from.y == to.y {
            let startX = min(from.x, to.x, 0)
            let endX = max(from.x, to.x, screenSize.width - 1)
            for x in startX...endX {
                self[x, from.y] = color
            }
        } else {
            Log.error("Only horizontal or vertical lines are supported.")
        }
    }

    func drawRect(rect: Rect, color: Color) {
        let startX = max(0, rect.minX)
        let endX = min(screenSize.width, rect.maxX)
        let startY = max(0, rect.minY)
        let endY = min(screenSize.height, rect.maxY)
        let color = color.pixel(type: P.self)

        for x in startX..<endX {
            self[x, startY] = color
            self[x, endY - 1] = color
        }
        for y in startY..<endY {
            self[startX, y] = color
            self[endX - 1, y] = color
        }
    }

    func fillRect(rect: Rect, color: Color) {
        let startX = max(0, rect.minX)
        let endX = min(screenSize.width, rect.maxX)
        let startY = max(0, rect.minY)
        let endY = min(screenSize.height, rect.maxY)
        let color = color.pixel(type: P.self)

        for y in startY..<endY {
            for x in startX..<endX {
                self[x, y] = color
            }
        }
    }

    func drawText(_ text: String, at point: Point, font: Font, color: Color) {
        let color = color.pixel(type: P.self)
        font.drawBitmap(text, maxWidth: screenSize.width - point.x) { (pixelPoint, value) in
            if value > 0 {
                self[pixelPoint + point] = color
            }
        }
    }

    func drawTextCenter(_ text: String, at point: Point, font: Font, color: Color) {
        let color = color.pixel(type: P.self)
        let labelWidth = font.width(of: text)
        font.drawBitmap(text, maxWidth: screenSize.width - point.x) { (pixelPoint, value) in
            if value > 0 {
                self[pixelPoint + point - Point(x: labelWidth / 2, y: 0)] = color
            }
        }
    }

    func drawBitmap(_ data: (size: Size, bitmap: [UInt32]), at point: Point, color: Color) {
        let rowCount = (data.size.width + 31) / 32
        var row = 0
        let color = color.pixel(type: P.self)
        for y in 0..<data.size.height {
            for x in 0..<data.size.width {
                let pixelIndex = row + x / 32
                let bitIndex = x % 32
                if (data.bitmap[pixelIndex] & (1 << (31 - bitIndex))) != 0 {
                    self[point.x + x, point.y + y] = color
                }
            }
            row += rowCount
        }
    }

    func drawBuffer(
        buffer: UnsafeRawBufferPointer,
        size: Size
    ) {
        memcpy(self.buffer.baseAddress!, buffer.baseAddress, size.area * MemoryLayout<P>.size)
    }
    func flush() {}
}

fileprivate extension ColorSpace {
    var ppaSrmColorMode: ppa_srm_color_mode_t {
        switch self {
        case .rgb565: return PPA_SRM_COLOR_MODE_RGB565
        case .rgb888: return PPA_SRM_COLOR_MODE_RGB888
        case .yuv420: return PPA_SRM_COLOR_MODE_YUV420
        }
    }
}
