struct Point: Equatable {
    var x: Int16
    var y: Int16

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.x == rhs.x && lhs.y == rhs.y
    }
}

struct Size: Equatable {
    var width: Int16
    var height: Int16

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.width == rhs.width && lhs.height == rhs.height
    }
}

struct Rect: Equatable {
    var origin: Point
    var size: Size

    var width: Int16 {
        return size.width
    }
    var height: Int16 {
        return size.height
    }
    var minX: Int16 {
        return origin.x
    }
    var minY: Int16 {
        return origin.y
    }
    var maxX: Int16 {
        return origin.x + size.width
    }
    var maxY: Int16 {
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
    init(x: Int16, y: Int16, width: Int16, height: Int16) {
        self.origin = Point(x: x, y: y)
        self.size = Size(width: width, height: height)
    }

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.origin == rhs.origin && lhs.size == rhs.size
    }
}
