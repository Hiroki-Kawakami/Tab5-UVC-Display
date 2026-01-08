fileprivate let Log = Logger(tag: "main")

@_cdecl("app_main")
func app_main() {
    do {
        try main(pixelFormat: RGB565.self)
    } catch {
        Log.error("Main Function Exit with Error: \(error)")
    }
}
func main<PixelFormat: Pixel>(pixelFormat: PixelFormat.Type) throws(IDF.Error) {
    let tab5 = try M5StackTab5.begin(
        pixelFormat: PixelFormat.self,
        frameBufferNum: 3,
        usbHost: true,
    )
    try LVGL.begin()
    let controlView = try ControlView(tab5: tab5) {}
    let frameBuffers = tab5.display.frameBuffers
    tab5.display.brightness = 100

    let frameQueue = Queue<UnsafePointer<uvc_host_frame_t>>(capacity: 1)!
    let jpegDecoder = try IDF.JPEG.Decoder(outputFormat:
        PixelFormat.self == RGB888.self ? .rgb888(elementOrder: .bgr, conversion: .bt601) : .rgb565(elementOrder: .bgr, conversion: .bt601)
    )

    let usbHost = USBHost()
    let uvcDriver = USBHost.UVC()
    let uacDriver = USBHost.UAC()
    try usbHost.install()
    try uvcDriver.install(taskStackSize: 6 * 1024, taskPriority: 6, xCoreID: 0)
    try uacDriver.install(taskStackSize: 4 * 1024, taskPriority: 5, xCoreID: 0)
    uvcDriver.onFrame { frame in
        switch USBHost.UVC.StreamFormat(frame.pointee.vs_format.format) {
        case .mjpeg:
            if !frameQueue.send(frame, timeout: 0) {
                Log.warn("FRAME DROPPED!")
                return true
            }
            return false
        default:
            Log.error("Unsupported UVC Stream Format")
            return true
        }
    }

    let ppa = try IDF.PPAClient(operType: .srm)
    let decodeBuffer = Memory.allocate(type: PixelFormat.self, capacity: 1280 * 720, capability: [.cacheAligned, .spiram])!
    let audioBuffer = Memory.allocateRaw(size: 16 * 1024, capability: [.cacheAligned, .spiram])!
    let timer = try IDF.GeneralPurposeTimer()

    Task(name: "Decoder", priority: 15, xCoreID: 0) { _ in
        var frameBufferIndex = 0
        var frameCount = 0
        var start = timer.count
        var decodeDurationMax: UInt64 = 0
        while true {
            guard let frame = frameQueue.receive(timeout: 4) else { continue }
            defer { uvcDriver.returnFrame(frame) }
            let jpegData = UnsafeRawBufferPointer(
                start: frame.pointee.data,
                count: frame.pointee.data_len
            )

            let nextFrameBufferIndex = (frameBufferIndex + 1) % frameBuffers.count
            let decodeStart = timer.count
            let decodeBuffer = UnsafeMutableRawBufferPointer(decodeBuffer)
            guard let _ = try? jpegDecoder.decode(
                inputBuffer: jpegData,
                outputBuffer: UnsafeMutableRawBufferPointer(decodeBuffer),
            ) else {
                continue
            }
            let colorMode: IDF.PPAClient.SRMColorMode = PixelFormat.self == RGB565.self ? .rgb565 : .rgb888
            try? ppa.srm(
                input: (buffer: UnsafeRawBufferPointer(decodeBuffer), size: Size(width: 1280, height: 720), block: nil, colorMode: colorMode),
                output: (buffer: UnsafeMutableRawBufferPointer(frameBuffers[nextFrameBufferIndex]), size: Size(width: 720, height: 1280), block: nil, colorMode: colorMode),
                rotate: 90
            )
            let decodeDuration = timer.duration(from: decodeStart)
            if decodeDuration > decodeDurationMax { decodeDurationMax = decodeDuration }

            if controlView.visible {
                LVGL.withLock {
                    controlView.push(fbIndex: nextFrameBufferIndex)
                }
            }

            tab5.display.flush(fbNum: nextFrameBufferIndex)
            frameBufferIndex = nextFrameBufferIndex

            frameCount += 1
            let now = timer.count
            if (now - start) >= 1000000 {
                Log.info("\(frameCount)fps, decode: \(decodeDurationMax)")
                frameCount = 0
                start = now
                decodeDurationMax = 0
            }
        }
    }

    let uvcStreamSemaphore = Semaphore.createBinary()!
    while true {
        Log.info("Opening the stream...")
        do throws(IDF.Error) {
            // Start UVC Video Stream
            try uvcDriver.open(
                resolution: (width: 1280, height: 720),
                frameRate: 30,
                pixelFormat: .mjpeg,
                numberOfFrameBuffers: 2
            )
            uvcDriver.onDisconnect {
                uvcStreamSemaphore.give()
            }
            try uvcDriver.start()

            // Start UAC Audio Stream
            if let audioInput = try uacDriver.open(
                direction: .rx,
                bufferSize: UInt32(audioBuffer.count),
                bufferThreshold: 4096
            ) {
                let deviceInfo = try audioInput.deviceInfo
                var config = (sampleRate: 48000, channels: 2, bitWidth: 16)
                if deviceInfo.VID == 0x534d && deviceInfo.PID == 0x2109 { // MS2109
                    config = (sampleRate: 96000, channels: 1, bitWidth: 16)
                }

                try audioInput.start(sampleRate: config.sampleRate, channels: config.channels, bitWidth: config.bitWidth)
                // tab5.audio.volume = controlView.volume
                tab5.audio.volume = 50
                audioInput.onRxDone { _ in
                    do throws(IDF.Error) {
                        let readSize = try audioInput.read(buffer: audioBuffer)
                        try tab5.audio.write(UnsafeMutableRawBufferPointer(start: audioBuffer.baseAddress!, count: Int(readSize)))
                    } catch {
                        Log.error("Failed to read/play audio data: \(error)")
                    }
                }
            }

            uvcStreamSemaphore.take()
            tab5.display.brightness = 0
            tab5.audio.volume = 0
            esp_restart()
        } catch {
            Log.error("Failed to start UVC/UAC stream: \(error)")
        }
    }
}
