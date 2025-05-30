class Semaphore {
    private let semaphore: SemaphoreHandle_t

    static func createBinary() -> Semaphore? {
        let semaphore = xQueueGenericCreate(
            1,
            UBaseType_t(semSEMAPHORE_QUEUE_ITEM_LENGTH),
            queueQUEUE_TYPE_BINARY_SEMAPHORE
        )
        if semaphore == nil {
            return nil
        }
        return Semaphore(semaphore: semaphore!)
    }

    static func createMutex() -> Semaphore? {
        let semaphore = xQueueCreateMutex(queueQUEUE_TYPE_MUTEX)
        if semaphore == nil {
            return nil
        }
        return Semaphore(semaphore: semaphore!)
    }

    private init(semaphore: SemaphoreHandle_t) {
        self.semaphore = semaphore
    }

    @discardableResult
    func take(timeout: UInt32 = portMAX_DELAY) -> Bool {
        let res = xQueueSemaphoreTake(semaphore, timeout)
        return res == pdPASS
    }

    @discardableResult
    func give() -> Bool {
        let res = xQueueGenericSend(semaphore, nil, semGIVE_BLOCK_TIME, queueSEND_TO_BACK)
        return res == pdPASS
    }

    @discardableResult
    func giveFromISR() -> Bool {
        var higherPriorityTaskWoken: BaseType_t = 0
        let res = xQueueGiveFromISR(semaphore, &higherPriorityTaskWoken)
        return res == pdPASS
    }
}
