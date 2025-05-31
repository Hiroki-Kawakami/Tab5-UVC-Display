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

    private var srmClient: IDF.PPAClient?
    private var srm: IDF.PPAClient {
        if let srm = srmClient { return srm }
        do {
            srmClient = try IDF.PPAClient(operType: .srm)
            return srmClient!
        } catch {
            Log.error("Failed to create PPA SRM client: \(error)")
            fatalError("PPA SRM client initialization failed")
        }
    }
    func drawBufferFit(
        buffer: UnsafeRawBufferPointer,
        size: Size
    ) throws(IDF.Error) {
        try srm.fitScreen(
            input: (
                buffer: buffer,
                size: size,
                colorMode: PPA_SRM_COLOR_MODE_YUV422
            ),
            output: (
                buffer: UnsafeMutableRawBufferPointer(self.buffer),
                size: self.screenSize,
                colorMode: PPA_SRM_COLOR_MODE_RGB888
            )
        )
    }

    func flush() {}
}
