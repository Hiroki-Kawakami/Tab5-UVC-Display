fileprivate let LOG = Logger(tag: "main")

@_cdecl("app_main")
func main() {
    let tab5 = try! M5StackTab5.begin()
    tab5.display.brightness = 1.0

    let frameBuffer = tab5.display.frameBuffer
    let bufferPool = Queue<UnsafeMutableBufferPointer<UInt16>>(capacity: 2)!
    let decodedBuffers = Queue<UnsafeMutableBufferPointer<UInt16>>(capacity: 2)!
    for _ in 0..<2 {
        if let buffer = IDF.JPEG.Decoder<UInt16>.allocateOutputBuffer(capacity: Int(tab5.display.width * tab5.display.height)) {
            LOG.info("Buffer allocated: \(buffer.baseAddress!)")
            bufferPool.send(buffer)
        } else {
            LOG.error("Failed to allocate memory for buffer")
            return
        }
    }

    let usbHost = USBHost()
    let uvcDriver = USBHost.UVC()
    let frameQueue = Queue<UnsafePointer<uvc_host_frame_t>>(capacity: 2)!
    try! usbHost.install()
    try! uvcDriver.install(taskStackSize: 6 * 1024, taskPriority: 6)
    uvcDriver.onFrame { frame in
        switch USBHost.UVC.StreamFormat(frame.pointee.vs_format.format) {
        case .mjpeg:
            frameQueue.send(frame)
            return false
        default:
            LOG.error("Unsupported UVC Stream Format")
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
                    LOG.info("FPS: \(frameCount)")
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
                LOG.error("Failed to decode JPEG: \(error)")
                bufferPool.send(outputBuffer)
            }
            uvcDriver.returnFrame(frame)
        }
    }
    Task(name: "Draw", priority: 16) { _ in
        let ppa = try! IDF.PPAClient(operType: .srm)
        for buffer in decodedBuffers {
            do throws(IDF.Error) {
                try ppa.rotate90(inputBuffer: buffer, outputBuffer: frameBuffer, size: (width: 1280, height: 720))
                tab5.display.drawBitmap(start: (0, 0), end: (720, 1280), data: frameBuffer.baseAddress!)
            } catch {
                LOG.error("Failed to draw image: \(error)")
            }
            bufferPool.send(buffer)
        }
    }

    let uvcStreamSemaphore = Semaphore.createBinary()!
    while true {
        LOG.info("Opeining the stream...")
        do throws(IDF.Error) {
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
            uvcStreamSemaphore.take()
        } catch {
            LOG.error("Failed to start UVC stream: \(error)")
        }
    }
}
