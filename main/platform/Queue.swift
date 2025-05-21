class Queue<T>: Sequence {
    private let queue: QueueHandle_t
    init?(capacity: UInt32) {
        let queue = xQueueGenericCreate(capacity, UInt32(MemoryLayout<T>.size), queueQUEUE_TYPE_BASE)
        if queue == nil {
            return nil
        }
        self.queue = queue!
    }

    deinit {
        vQueueDelete(queue)
    }

    @discardableResult
    func send(_ item: T, timeout: UInt32 = portMAX_DELAY) -> Bool {
        var item = item
        let res = withUnsafePointer(to: &item) {
            xQueueGenericSend(queue, $0, timeout, queueSEND_TO_BACK)
        }
        return res == pdPASS
    }

    func receive(timeout: UInt32 = portMAX_DELAY) -> T? {
        withUnsafeTemporaryAllocation(of: T.self, capacity: 1) {
            let res = xQueueReceive(queue, $0.baseAddress, timeout)
            if res == pdPASS {
                return $0.baseAddress?.pointee
            } else {
                return nil
            }
        }
    }

    struct Iterator: IteratorProtocol {
        private let queue: Queue<T>
        init(queue: Queue<T>) {
            self.queue = queue
        }
        mutating func next() -> T? {
            return queue.receive()
        }
    }
    func makeIterator() -> Iterator {
        return Iterator(queue: self)
    }
}
