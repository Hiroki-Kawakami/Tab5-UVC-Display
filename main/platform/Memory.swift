class Memory {
    struct Capability: OptionSet {
        let rawValue: UInt32

        static let exec = Capability(rawValue: UInt32(MALLOC_CAP_EXEC))
        static let allow32bit = Capability(rawValue: UInt32(MALLOC_CAP_32BIT))
        static let allow8bit = Capability(rawValue: UInt32(MALLOC_CAP_8BIT))
        static let dma = Capability(rawValue: UInt32(MALLOC_CAP_DMA))
        static let pid2 = Capability(rawValue: UInt32(MALLOC_CAP_PID2))
        static let pid3 = Capability(rawValue: UInt32(MALLOC_CAP_PID3))
        static let pid4 = Capability(rawValue: UInt32(MALLOC_CAP_PID4))
        static let pid5 = Capability(rawValue: UInt32(MALLOC_CAP_PID5))
        static let pid6 = Capability(rawValue: UInt32(MALLOC_CAP_PID6))
        static let pid7 = Capability(rawValue: UInt32(MALLOC_CAP_PID7))
        static let spiram = Capability(rawValue: UInt32(MALLOC_CAP_SPIRAM))
        static let `internal` = Capability(rawValue: UInt32(MALLOC_CAP_INTERNAL))
        static let `default` = Capability(rawValue: UInt32(MALLOC_CAP_DEFAULT))
        static let iram8bit = Capability(rawValue: UInt32(MALLOC_CAP_IRAM_8BIT))
        static let retention = Capability(rawValue: UInt32(MALLOC_CAP_RETENTION))
        static let rtcram = Capability(rawValue: UInt32(MALLOC_CAP_RTCRAM))
        static let tcm = Capability(rawValue: UInt32(MALLOC_CAP_TCM))
        static let dmaDescAHB = Capability(rawValue: UInt32(MALLOC_CAP_DMA_DESC_AHB))
        static let dmaDescAXI = Capability(rawValue: UInt32(MALLOC_CAP_DMA_DESC_AXI))
        static let cacheAligned = Capability(rawValue: UInt32(MALLOC_CAP_CACHE_ALIGNED))
        static let simd = Capability(rawValue: UInt32(MALLOC_CAP_SIMD))
    }

    static func allocate<T>(type: T.Type, capacity: Int, capability: Capability = []) -> UnsafeMutableBufferPointer<T>? {
        let size = MemoryLayout<T>.size * capacity
        let pointer = heap_caps_malloc(size, capability.rawValue)
        if pointer == nil {
            return nil
        }
        return UnsafeMutableBufferPointer<T>(start: pointer?.bindMemory(to: type, capacity: capacity), count: capacity)
    }

    static func allocateRaw(size: Int, capability: Capability = []) -> UnsafeMutableRawBufferPointer? {
        let pointer = heap_caps_malloc(size, capability.rawValue)
        if pointer == nil {
            return nil
        }
        return UnsafeMutableRawBufferPointer(start: pointer, count: size)
    }
}
