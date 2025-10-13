struct Point: Equatable, CustomStringConvertible {
    var x: Int
    var y: Int

    static let zero = Point(x: 0, y: 0)

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.x == rhs.x && lhs.y == rhs.y
    }
    static func + (lhs: Self, rhs: Self) -> Self {
        return Self(x: lhs.x + rhs.x, y: lhs.y + rhs.y)
    }
    static func - (lhs: Self, rhs: Self) -> Self {
        return Self(x: lhs.x - rhs.x, y: lhs.y - rhs.y)
    }

    var description: String {
        return "(\(x), \(y))"
    }
}

struct Size: Equatable, CustomStringConvertible {
    var width: Int
    var height: Int
    var area: Int {
        return width * height
    }

    static let zero = Size(width: 0, height: 0)

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.width == rhs.width && lhs.height == rhs.height
    }

    var description: String {
        return "(\(width), \(height))"
    }
}

struct Rect: Equatable, CustomStringConvertible {
    var origin: Point
    var size: Size

    static let zero = Rect(origin: Point.zero, size: Size.zero)

    var width: Int {
        return size.width
    }
    var height: Int {
        return size.height
    }
    var minX: Int {
        return origin.x
    }
    var minY: Int {
        return origin.y
    }
    var maxX: Int {
        return origin.x + size.width
    }
    var maxY: Int {
        return origin.y + size.height
    }
    var center: Point {
        return Point(x: origin.x + size.width / 2, y: origin.y + size.height / 2)
    }
    var isEmpty: Bool {
        return size.width <= 0 || size.height <= 0
    }
    init(origin: Point, size: Size) {
        self.origin = origin
        self.size = size
    }
    init(center: Point, size: Size) {
        self.origin = Point(x: center.x - size.width / 2, y: center.y - size.height / 2)
        self.size = size
    }
    init(x: Int, y: Int, width: Int, height: Int) {
        self.origin = Point(x: x, y: y)
        self.size = Size(width: width, height: height)
    }
    init(leftCenter: Point, size: Size) {
        self.origin = Point(x: leftCenter.x, y: leftCenter.y - size.height / 2)
        self.size = size
    }
    init(rightCenter: Point, size: Size) {
        self.origin = Point(x: rightCenter.x - size.width, y: rightCenter.y - size.height / 2)
        self.size = size
    }
    init(topCenter: Point, size: Size) {
        self.origin = Point(x: topCenter.x - size.width / 2, y: topCenter.y)
        self.size = size
    }
    init(bottomCenter: Point, size: Size) {
        self.origin = Point(x: bottomCenter.x - size.width / 2, y: bottomCenter.y - size.height)
        self.size = size
    }

    func contains(_ point: Point) -> Bool {
        return point.x >= minX && point.x < maxX && point.y >= minY && point.y < maxY
    }

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.origin == rhs.origin && lhs.size == rhs.size
    }

    var description: String {
        return "(\(origin), \(size))"
    }
}

enum Color: Equatable {
    case rgb565(UInt16)
    case rgb888(red: UInt8, green: UInt8, blue: UInt8)

    static let white = Color.rgb565(0xFFFF)
    static let red = Color.rgb565(0xF800)
    static let green = Color.rgb565(0x07E0)
    static let blue = Color.rgb565(0x001F)
    static let yellow = Color.rgb565(0xFFE0)
    static let cyan = Color.rgb565(0x07FF)
    static let magenta = Color.rgb565(0xF81F)
    static let black = Color.rgb565(0x0000)
    static let gray = Color.rgb565(0x7BEF)

    @inline(__always) var red: UInt8 {
        switch self {
        case .rgb565(let value):
            return UInt8((value & 0xF800) >> 8)
        case .rgb888(let red, _, _):
            return red
        }
    }
    @inline(__always) var green: UInt8 {
        switch self {
        case .rgb565(let value):
            return UInt8((value & 0x07E0) >> 3)
        case .rgb888(_, let green, _):
            return green
        }
    }
    @inline(__always) var blue: UInt8 {
        switch self {
        case .rgb565(let value):
            return UInt8((value & 0x001F) << 3)
        case .rgb888(_, _, let blue):
            return blue
        }
    }

    @inline(__always) func pixel<T: Pixel>(type: T.Type) -> T {
        return T(red: red, green: green, blue: blue)
    }

    static func ==(lhs: Color, rhs: Color) -> Bool {
        return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue
    }
}

protocol Pixel {
    @inline(__always) init(red: UInt8, green: UInt8, blue: UInt8)
    @inline(__always) var red: UInt8 { get }
    @inline(__always) var green: UInt8 { get }
    @inline(__always) var blue: UInt8 { get }
    static var black: Self { get }
    static var white: Self { get }
    static var colorSpace: ColorSpace { get }
}

struct RGB565: Pixel, RawRepresentable {
    var rawValue: UInt16

    @inline(__always) init(rawValue: UInt16) {
        self.rawValue = rawValue
    }
    @inline(__always) init(red: UInt8, green: UInt8, blue: UInt8) {
        self.rawValue = (UInt16(red & 0xF8) << 8) | (UInt16(green & 0xFC) << 3) | (UInt16(blue & 0xF8) >> 3)
    }
    @inline(__always) var red: UInt8 { UInt8((rawValue & 0xF800) >> 8) }
    @inline(__always) var green: UInt8 { UInt8((rawValue & 0x07E0) >> 3) }
    @inline(__always) var blue: UInt8 { UInt8((rawValue & 0x001F) << 3) }

    static var black: RGB565 { RGB565(rawValue: 0x0000) }
    static var white: RGB565 { RGB565(rawValue: 0xFFFF) }
    static var colorSpace: ColorSpace { .rgb565 }
}

struct RGB888: Pixel, Equatable {
    var blue, green, red: UInt8

    @inline(__always) init(red: UInt8, green: UInt8, blue: UInt8) {
        self.red = red
        self.green = green
        self.blue = blue
    }

    static var black: RGB888 { RGB888(red: 0, green: 0, blue: 0) }
    static var white: RGB888 { RGB888(red: 255, green: 255, blue: 255) }
    static var colorSpace: ColorSpace { .rgb888 }

    static func ==(lhs: RGB888, rhs: RGB888) -> Bool {
        return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue
    }
}

enum ColorSpace {
    case rgb565
    case rgb888
    case yuv420
}
