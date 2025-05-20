class Memory {
    static func allocate<T>(type: T.Type, capacity: Int) -> UnsafeMutableBufferPointer<T>? {
        let size = MemoryLayout<T>.size * capacity
        let pointer = heap_caps_malloc(size, 0)
        if pointer == nil {
            return nil
        }
        return UnsafeMutableBufferPointer<T>(start: pointer?.assumingMemoryBound(to: type), count: capacity)
    }
}
