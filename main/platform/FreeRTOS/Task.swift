class Task {
    static func delay(_ ms: UInt32) {
        vTaskDelay(ticks(ms))
    }

    static func ticks(_ ms: UInt32) -> UInt32 {
        return ms / (1000 / UInt32(configTICK_RATE_HZ))
    }

    static var tickCount: UInt32 {
        return xTaskGetTickCount()
    }

    let proc: (Task) -> Void
    var handle: TaskHandle_t? = nil
    private(set) var shouldStop: Bool = false

    @discardableResult
    init(name: UnsafePointer<CChar>, stackDepth: UInt32 = 4096, priority: UInt32, xCoreID: BaseType_t = 0, proc: @escaping (Task) -> Void) {
        self.proc = proc

        let res = xTaskCreatePinnedToCore(
            { (pvParameters: UnsafeMutableRawPointer?) in
                let task = Unmanaged<Task>.fromOpaque(pvParameters!).takeRetainedValue()
                task.proc(task)
                task.handle = nil
                vTaskDelete(nil)
            },
            name,
            stackDepth,
            Unmanaged.passRetained(self).toOpaque(),
            priority,
            &self.handle,
            xCoreID
        )
        if res != pdPASS {
            fatalError("Failed to create task: \(res)")
        }
    }

    func stop() {
        shouldStop = true
    }
}
