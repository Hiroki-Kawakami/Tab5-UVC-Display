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

    static func == (lhs: Self, rhs: Self) -> Bool {
        return lhs.origin == rhs.origin && lhs.size == rhs.size
    }
}
