struct Point: Equatable, CustomStringConvertible {
    var x: Int
    var y: Int

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

enum Color {
    case rgb565(UInt16)

    static let white = Color.rgb565(0xFFFF)
    static let red = Color.rgb565(0xF800)
    static let green = Color.rgb565(0x07E0)
    static let blue = Color.rgb565(0x001F)
    static let yellow = Color.rgb565(0xFFE0)
    static let cyan = Color.rgb565(0x07FF)
    static let magenta = Color.rgb565(0xF81F)
    static let black = Color.rgb565(0x0000)

    var rgb565: UInt16 {
        switch self {
        case .rgb565(let value):
            return value
        }
    }
}
