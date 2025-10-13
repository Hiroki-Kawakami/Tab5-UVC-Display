fileprivate let Log = Logger(tag: "main")

@_cdecl("app_main")
func app_main() {
    do {
        try main()
    } catch {
        Log.error("Main Function Exit with Error: \(error)")
    }
}
func main() throws(IDF.Error) {
    let tab5 = try M5StackTab5.begin()
    let frameBuffers = tab5.display.frameBuffers
    tab5.display.brightness = 100

    let multiTouch: MultiTouch = MultiTouch()
    multiTouch.task(xCoreID: 1) {
        tab5.touch.waitInterrupt()
        return try! tab5.touch.coordinates
    }

    let fontPartition = IDF.Partition(type: 0x40, subtype: 0)!
    FontFamily.default = FontFamily(from: fontPartition)
    for frameBuffer in frameBuffers {
        let drawable = tab5.display.drawable(frameBuffer: frameBuffer)
        drawable.clear(color: .black);
        drawable.flush()
    }

    let controlView = ControlView(size: Size(width: 380, height: 720), volume: 70, brightness: 100)
    var controlDrawn: [Bool] = [false, false]
    func setNeedsDrawControl() {
        controlView.drawControl()
        for i in 0..<controlDrawn.count {
            controlDrawn[i] = false
        }
    }
    controlView.setVolume = { volume in
        controlView.volume = max(0, min(100, volume))
        tab5.audio.volume = controlView.volume
    }
    controlView.setBrightness = { brightness in
        controlView.brightness = max(10, min(100, brightness))
        tab5.display.brightness = controlView.brightness
    }

    let controlWidth = 380
    var showControl = false
    multiTouch.onEvent { event in
        switch event {
        case .tap(let point):
            if showControl && point.y < controlWidth {
                controlView.onTap(point: Point(x: controlWidth - point.y, y: point.x))
            } else {
                showControl.toggle()
            }
            if showControl {
                setNeedsDrawControl()
            }
        default:
            break
        }
    }

    let imageBuffers = Queue<UnsafeMutableRawBufferPointer>(capacity: 2)!
    for _ in 0..<2 {
        if let buffer = IDF.JPEG.Decoder.allocateOutputBuffer(size: 1280 * 720 * 2) {
            imageBuffers.send(buffer)
        } else {
            Log.error("Failed to allocate jpeg decoder output buffer")
            return
        }
    }
    let audioBuffer = Memory.allocateRaw(size: 16 * 1024, capability: [.cacheAligned, .spiram])!
    let frameQueue = Queue<UnsafePointer<uvc_host_frame_t>>(capacity: 1)!
    let drawQueue = Queue<UnsafeMutableRawBufferPointer>(capacity: 1)!

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

    let timer = try IDF.Timer()
    let jpegDecoder = try IDF.JPEG.Decoder(outputFormat: .rgb565(elementOrder: .bgr, conversion: .bt709))
    Task(name: "Decode", priority: 15) { _ in
        for frame in frameQueue {
            defer { uvcDriver.returnFrame(frame) }
            guard let imageBuffer = imageBuffers.receive(timeout: 0) else { continue }
            guard let _ = try? jpegDecoder.decode(
                inputBuffer: UnsafeRawBufferPointer(
                    start: frame.pointee.data,
                    count: frame.pointee.data_len
                ),
                outputBuffer: imageBuffer
            ) else {
                imageBuffers.send(imageBuffer)
                continue
            }
            if !drawQueue.send(imageBuffer, timeout: 0) {
                imageBuffers.send(imageBuffer)
            }
        }
    }

    let ppa = try IDF.PPAClient(operType: .srm)
    Task(name: "Draw", priority: 14) { _ in
        var bufferIndex = 0
        var frameCount = 0
        var start = timer.count
        while true {
            var success = false, needFlush = true
            if showControl && !controlDrawn[bufferIndex] {
                controlView.draw(into: frameBuffers[bufferIndex], ppa: ppa)
                controlDrawn[bufferIndex] = true
            } else if let imageBuffer = drawQueue.receive(timeout: 50) {
                defer { imageBuffers.send(imageBuffer) }
                do throws(IDF.Error) {
                    try ppa.rotate90WithMargin(
                        inputBuffer: UnsafeRawBufferPointer(imageBuffer),
                        outputBuffer: UnsafeMutableRawBufferPointer(
                            start: frameBuffers[bufferIndex].baseAddress,
                            count: frameBuffers[bufferIndex].count * 2
                        ),
                        size: (width: 1280, height: 720),
                        margin: showControl ? UInt32(controlWidth) : 0
                    )
                    frameCount += 1
                    success = true
                } catch {
                    Log.error("Failed to draw image: \(error)")
                }
            } else {
                needFlush = false
            }

            if needFlush {
                esp_lcd_panel_draw_bitmap(tab5.display.panel, 0, 0, 720, 1280, frameBuffers[bufferIndex].baseAddress)
            }
            if success {
                bufferIndex = bufferIndex == 0 ? 1 : 0
            }

            let now = timer.count
            if (now - start) >= 1000000 {
                if frameCount > 0 {
                    Log.info("\(frameCount)fps")
                }
                frameCount = 0
                start = now
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
                frameRate: 20,
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
                tab5.audio.volume = controlView.volume
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

class ControlView {

    let size: Size
    private let drawable: Drawable<RGB565>

    var volume: Int
    var brightness: Int

    init(size: Size, volume: Int, brightness: Int) {
        self.size = size
        self.volume = volume
        self.brightness = brightness

        let bufferSize = Int(size.width * size.height)
        let viewBuffer = Memory.allocate(type: RGB565.self, capacity: bufferSize, capability: [.cacheAligned, .spiram])!
        self.drawable = Drawable(buffer: viewBuffer.baseAddress!, screenSize: size)
    }

    deinit {
        drawable.buffer.deallocate()
    }

    func drawButton(label: String, rect: Rect, fontSize: Int) {
        drawable.drawRect(rect: rect, color: .white)
        drawable.drawTextCenter(label, at: Point(x: rect.origin.x + rect.width / 2, y: rect.origin.y + (rect.height - fontSize) / 2), font: FontFamily.default.font(size: fontSize), color: .white)
    }

    func drawStepper(label: String, value: Int, offsetY: Int) -> (Rect, Rect) {
        drawable.drawTextCenter(label, at: Point(x: size.width / 2, y: offsetY), font: FontFamily.default.font(size: 42), color: .white)
        drawable.drawTextCenter("\(value)", at: Point(x: size.width / 2, y: offsetY + 50), font: FontFamily.default.font(size: 60), color: .white)

        let buttonSize = Size(width: 120, height: 70)
        let minusRect = Rect(origin: Point(x: size.width / 2 - 130, y: offsetY + 120), size: buttonSize)
        let plusRect = Rect(origin: Point(x: size.width / 2 + 10, y: offsetY + 120), size: buttonSize)
        drawButton(label: "-", rect: minusRect, fontSize: 50)
        drawButton(label: "+", rect: plusRect, fontSize: 50)
        return (minusRect, plusRect)
    }

    func drawControl() {
        drawable.clear()
        drawable.drawLine(from: Point(x: 0, y: 0), to: Point(x: 0, y: size.height - 1), color: .white)

        (volMinusRect, volPlusRect) = drawStepper(label: "Volume", value: volume, offsetY: 80)
        (briMinusRect, briPlusRect) = drawStepper(label: "Brightness", value: brightness, offsetY: 360)
    }

    func draw(into frameBuffer: UnsafeMutableBufferPointer<RGB565>, ppa: IDF.PPAClient) {
        do throws(IDF.Error) {
            try ppa.rotate90WithMargin(
                inputBuffer: UnsafeRawBufferPointer(
                    start: drawable.buffer.baseAddress!,
                    count: drawable.buffer.count * 2
                ),
                outputBuffer: UnsafeMutableRawBufferPointer(
                    start: frameBuffer.baseAddress!,
                    count: frameBuffer.count * 2
                ),
                size: (width: UInt32(size.width), height: UInt32(size.height)),
                margin: 0
            )
        } catch {
            Log.error("Failed to render control view!")
        }
        // for viewY in 0..<size.height {
        //     let frameX = viewY
        //     for viewX in 0..<size.width {
        //         let frameY = size.width - 1 - viewX
        //         let viewIndex = viewY * size.width + viewX
        //         let frameIndex = frameY * size.height + frameX
        //         frameBuffer[frameIndex] = drawable.buffer[viewIndex]
        //     }
        // }
    }

    var volMinusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
    var volPlusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
    var briMinusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
    var briPlusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
    var setVolume: ((Int) -> Void)?
    var setBrightness: ((Int) -> Void)?

    func onTap(point: Point) {
        if volMinusRect.contains(point) {
            setVolume?(volume - 10)
        } else if volPlusRect.contains(point) {
            setVolume?(volume + 10)
        } else if briMinusRect.contains(point) {
            setBrightness?(brightness - 10)
        } else if briPlusRect.contains(point) {
            setBrightness?(brightness + 10)
        }
    }
}
