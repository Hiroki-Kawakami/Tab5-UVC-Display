fileprivate let Log = Logger(tag: "main")

@_cdecl("app_main")
func main() {
    let tab5 = try! M5StackTab5.begin()
    tab5.display.brightness = 1.0

    // let frameBuffer = Memory.allocate(type: UInt16.self, capacity: 1280 * 720, capability: [.cacheAligned, .spiram])!
    let frameBuffer = tab5.display.frameBuffer
    let bufferPool = Queue<UnsafeMutableBufferPointer<UInt16>>(capacity: 2)!
    let decodedBuffers = Queue<UnsafeMutableBufferPointer<UInt16>>(capacity: 2)!
    for _ in 0..<2 {
        if let buffer = IDF.JPEG.Decoder<UInt16>.allocateOutputBuffer(capacity: Int(tab5.display.width * tab5.display.height)) {
            bufferPool.send(buffer)
        } else {
            Log.error("Failed to allocate memory for buffer")
            return
        }
    }
    let audioBuffer = Memory.allocateRaw(size: 16 * 1024, capability: [.cacheAligned, .spiram])!

    let usbHost = USBHost()
    let uvcDriver = USBHost.UVC()
    let uacDriver = USBHost.UAC()
    let frameQueue = Queue<UnsafePointer<uvc_host_frame_t>>(capacity: 2)!
    try! usbHost.install()
    try! uvcDriver.install(taskStackSize: 6 * 1024, taskPriority: 6, xCoreID: 0)
    try! uacDriver.install(taskStackSize: 4 * 1024, taskPriority: 5, xCoreID: 0)
    uvcDriver.onFrame { frame in
        switch USBHost.UVC.StreamFormat(frame.pointee.vs_format.format) {
        case .mjpeg:
            frameQueue.send(frame)
            return false
        default:
            Log.error("Unsupported UVC Stream Format")
            return false
        }
    }

    Task(name: "Decode", priority: 15) { _ in
        var lastTick: UInt32? = nil
        var frameCount = 0
        let decoder = try! IDF.JPEG.createDecoderRgb565(rgbElementOrder: .bgr, rgbConversion: .bt709)
        for frame in frameQueue {
            let outputBuffer = bufferPool.receive()!
            let inputBuffer = UnsafeRawBufferPointer(
                start: frame.pointee.data,
                count: Int(frame.pointee.data_len)
            )

            frameCount += 1
            if let _lastTick = lastTick {
                let currentTick = Task.tickCount
                let elapsed = currentTick - _lastTick
                if elapsed >= Task.ticks(1000) {
                    Log.info("FPS: \(frameCount)")
                    frameCount = 0
                    lastTick = currentTick
                }
            } else {
                frameCount = 0
                lastTick = Task.tickCount
            }

            do throws(IDF.Error) {
                let _ = try decoder.decode(inputBuffer: inputBuffer, outputBuffer: outputBuffer)
                decodedBuffers.send(outputBuffer)
            } catch {
                Log.error("Failed to decode JPEG: \(error)")
                bufferPool.send(outputBuffer)
            }
            uvcDriver.returnFrame(frame)
        }
    }
    Task(name: "Draw", priority: 16) { _ in
        let ppa = try! IDF.PPAClient(operType: .srm)
        var firstFrame = true
        for buffer in decodedBuffers {
            do throws(IDF.Error) {
                firstFrame.toggle()
                try ppa.rotate90(inputBuffer: buffer, outputBuffer: frameBuffer, size: (width: 1280, height: 720))
                tab5.display.drawBitmap(start: (0, 0), end: (720, 1280), data: frameBuffer.baseAddress!)
            } catch {
                Log.error("Failed to draw image: \(error)")
            }
            bufferPool.send(buffer)
        }
    }

    let uvcStreamSemaphore = Semaphore.createBinary()!
    while true {
        Log.info("Opening the stream...")
        do throws(IDF.Error) {
            // Check Device Connected
            // guard let frameInfoList = try uvcDriver.frameInfoList else {
            //     if checkDeviceCount % 50 == 0 {
            //         Log.error("UVC Device not found.")
            //         checkDeviceCount = 0
            //     }
            //     checkDeviceCount += 1
            //     Task.delay(100)
            //     continue
            // }

            // for var frameInfo in frameInfoList {
            //     Log.info("Format: \(frameInfo.format.rawValue), \(frameInfo.h_res)x\(frameInfo.v_res)")
            //     let intervalType = frameInfo.interval_type
            //     if intervalType > 0 {
            //         withUnsafeBytes(of: &frameInfo.interval) {
            //             let list: UnsafePointer<UInt32> = $0.baseAddress!.assumingMemoryBound(to: UInt32.self)
            //             for i in 0..<min(Int(intervalType), Int(CONFIG_UVC_INTERVAL_ARRAY_SIZE)) {
            //                 let fps = Int(1e7 / Double(list[i]))
            //                 Log.info("  \(fps)Hz")
            //             }
            //         }
            //     }
            // }
            // Task.delay(300)

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
                tab5.audio.volume = 70
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
