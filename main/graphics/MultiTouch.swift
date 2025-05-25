class MultiTouch {

    enum State {
        case idle
        case down(point: Point, duration: UInt16)
        case longDown(point: Point, duration: UInt16)
        case touchMove(point: Point)
    }

    enum Event {
        case tap(point: Point)
        case longPress(point: Point)
        case longTap(point: Point)
        case drag(from: Point, to: Point)
        case dragEnd(at: Point)
    }

    private var touchPoint: State = .idle

    func onTouch(coordinates: [Point]) {
        switch touchPoint {
        case .idle:
            if let first = coordinates.first {
                touchPoint = .down(point: first, duration: 0)
            }
        case .down(point: let point, duration: let duration):
            if let first = coordinates.first {
                if point != first {
                    touchPoint = .touchMove(point: first)
                    triggerEvent(.drag(from: point, to: first))
                } else if duration < 15 {
                    touchPoint = .down(point: point, duration: duration + 1)
                } else {
                    touchPoint = .longDown(point: point, duration: duration + 1)
                    triggerEvent(.longPress(point: point))
                }
            } else {
                touchPoint = .idle
                triggerEvent(.tap(point: point))
            }
        case .longDown(point: let point, duration: let duration):
            if let first = coordinates.first {
                if point != first {
                    touchPoint = .touchMove(point: first)
                    triggerEvent(.drag(from: point, to: first))
                } else {
                    touchPoint = .longDown(point: point, duration: duration + 1)
                }
            } else {
                touchPoint = .idle
                triggerEvent(.longTap(point: point))
            }
        case .touchMove(point: let point):
            if let first = coordinates.first {
                if point != first {
                    touchPoint = .touchMove(point: first)
                    triggerEvent(.drag(from: point, to: first))
                }
            } else {
                touchPoint = .idle
                triggerEvent(.dragEnd(at: point))
            }
        }
    }

    var eventListener: ((Event) -> Void)? = nil
    func onEvent(_ event: @escaping (Event) -> Void) {
        self.eventListener = event
    }
    private func triggerEvent(_ event: Event) {
        eventListener?(event)
    }

    func task(xCoreID: BaseType_t = 0, _ read: @escaping () -> [Point]) {
        Task(name: "MultiTouch", priority: 20, xCoreID: xCoreID) { _ in
            while true {
                let coordinates = read()
                self.onTouch(coordinates: coordinates)
            }
        }
    }
}
