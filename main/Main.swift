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
    let drawable = tab5.display.drawable
    tab5.display.brightness = 100

    let multiTouch: MultiTouch = MultiTouch()
    multiTouch.task(xCoreID: 1) {
        tab5.touch.waitInterrupt()
        return try! tab5.touch.coordinates
    }

    let fontPartition = IDF.Partition(type: 0x40, subtype: 0)!
    guard let fontFamily = FontFamily(from: fontPartition) else {
        Log.error("Failed to load font from partition")
        return
    }
    FontFamily.default = fontFamily

    // let controlView = ControlView(size: Size(width: 380, height: 720), font: font, volume: 70, brightness: 100)
    // controlView.setVolume = { volume in
    //     controlView.volume = max(0, min(100, volume))
    //     tab5.audio.volume = controlView.volume
    // }
    // controlView.setBrightness = { brightness in
    //     controlView.brightness = max(10, min(100, brightness))
    //     tab5.display.brightness = controlView.brightness
    // }

    let controlWidth = 380
    var showControl = false
    let frameBufferMutex = Semaphore.createMutex()!
    // multiTouch.onEvent { event in
    //     switch event {
    //     case .tap(let point):
    //         if showControl && point.y < controlWidth {
    //             controlView.onTap(point: Point(x: controlWidth - point.y, y: point.x))
    //         } else {
    //             showControl.toggle()
    //         }
    //         if showControl {
    //             frameBufferMutex.take()
    //             controlView.draw(into: frameBuffer, frameBufferSize: Size(width: 720, height: 1280))
    //             frameBufferMutex.give()
    //         }
    //     default:
    //         break
    //     }
    // }

    // let frameBuffer = Memory.allocate(type: UInt16.self, capacity: 1280 * 720, capability: [.cacheAligned, .spiram])!
    let bufferPool = Queue<UnsafeMutableRawBufferPointer>(capacity: 2)!
    let decodedBuffers = Queue<UnsafeMutableRawBufferPointer>(capacity: 2)!
    for _ in 0..<2 {
        if let buffer = IDF.JPEG.Decoder.allocateOutputBuffer(size: tab5.display.pixels * 3) {
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
    try usbHost.install()
    try uvcDriver.install(taskStackSize: 6 * 1024, taskPriority: 6, xCoreID: 0)
    try uacDriver.install(taskStackSize: 4 * 1024, taskPriority: 5, xCoreID: 0)
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
        let decoder = try! IDF.JPEG.Decoder(outputFormat: .yuv422)
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
        for buffer in decodedBuffers {
            do throws(IDF.Error) {
                frameBufferMutex.take()
                defer { frameBufferMutex.give() }
                try drawable.drawBufferFit(buffer: UnsafeRawBufferPointer(buffer), size: Size(width: 1280, height: 720))
                drawable.flush()
                // try ppa.rotate90WithMargin(
                //     inputBuffer: buffer, outputBuffer: frameBuffer,
                //     size: (width: 1280, height: 720), margin: showControl ? UInt32(controlWidth) : 0
                // )
                // tab5.display.drawBitmap(rect: Rect(origin: .zero, size: tab5.display.size), data: frameBuffer.baseAddress!)
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
                // tab5.audio.volume = controlView.volume
                tab5.audio.volume = 40
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

// class ControlView {

//     let size: Size
//     let font: Font
//     private let viewBuffer: UnsafeMutableBufferPointer<UInt16>

//     var volume: Int
//     var brightness: Int

//     init(size: Size, font: Font, volume: Int, brightness: Int) {
//         self.size = size
//         self.font = font
//         self.volume = volume
//         self.brightness = brightness

//         let bufferSize = Int(size.width * size.height)
//         viewBuffer = Memory.allocate(type: UInt16.self, capacity: bufferSize, capability: [.cacheAligned, .spiram])!
//     }

//     deinit {
//         viewBuffer.deallocate()
//     }

//     func drawLine(from: Point, to: Point, color: Color) {
//         if from.x == to.x {
//             let startY = min(from.y, to.y, 0)
//             let endY = max(from.y, to.y, size.height - 1)
//             for y in startY...endY {
//                 viewBuffer[Int(y * size.width) + Int(from.x)] = color.rgb565
//             }
//         } else if from.y == to.y {
//             let startX = min(from.x, to.x, 0)
//             let endX = max(from.x, to.x, size.width - 1)
//             for x in startX...endX {
//                 viewBuffer[Int(from.y) * 720 + Int(x)] = color.rgb565
//             }
//         } else {
//             Log.error("Only horizontal or vertical lines are supported.")
//         }
//     }

//     func drawRect(rect: Rect, color: Color) {
//         let startX = max(0, rect.minX)
//         let endX = min(size.width, rect.maxX)
//         let startY = max(0, rect.minY)
//         let endY = min(size.height, rect.maxY)

//         for x in startX..<endX {
//             viewBuffer[Int(startY) * Int(size.width) + x] = color.rgb565
//             viewBuffer[Int(endY - 1) * Int(size.width) + x] = color.rgb565
//         }
//         for y in startY..<endY {
//             viewBuffer[y * Int(size.width) + startX] = color.rgb565
//             viewBuffer[y * Int(size.width) + endX - 1] = color.rgb565
//         }
//     }

//     func drawText(_ text: String, at point: Point, fontSize: Int, color: Color) {
//         let labelSize = Size(width: font.width(of: text, fontSize: fontSize), height: fontSize)
//         let rect = Rect(origin: Point(x: point.x - labelSize.width / 2, y: point.y), size: labelSize)
//         font.drawBitmap(text) { (point, value) in
//             let point = rect.origin + point
//             viewBuffer[point.y * size.width + point.x] = value == 0 ? 0x0000 : 0xFFFF
//         }
//     }

//     func drawButton(label: String, rect: Rect, fontSize: Int) {
//         drawRect(rect: rect, color: .white)
//         drawText(label, at: Point(x: rect.origin.x + rect.width / 2, y: rect.origin.y + (rect.height - fontSize) / 2), fontSize: fontSize, color: .white)
//     }

//     func drawStepper(label: String, value: Int, offsetY: Int) -> (Rect, Rect) {
//         drawText(label, at: Point(x: size.width / 2, y: offsetY), fontSize: 42, color: .white)
//         drawText("\(value)", at: Point(x: size.width / 2, y: offsetY + 50), fontSize: 60, color: .white)

//         let buttonSize = Size(width: 120, height: 70)
//         let minusRect = Rect(origin: Point(x: size.width / 2 - 130, y: offsetY + 120), size: buttonSize)
//         let plusRect = Rect(origin: Point(x: size.width / 2 + 10, y: offsetY + 120), size: buttonSize)
//         drawButton(label: "-", rect: minusRect, fontSize: 50)
//         drawButton(label: "+", rect: plusRect, fontSize: 50)
//         return (minusRect, plusRect)
//     }

//     func drawControl() {
//         viewBuffer.initialize(repeating: 0)
//         drawLine(from: Point(x: 0, y: 0), to: Point(x: 0, y: size.height - 1), color: .white)

//         (volMinusRect, volPlusRect) = drawStepper(label: "Volume", value: volume, offsetY: 80)
//         (briMinusRect, briPlusRect) = drawStepper(label: "Brightness", value: brightness, offsetY: 360)
//     }

//     func draw(into frameBuffer: UnsafeMutableBufferPointer<UInt16>, frameBufferSize: Size) {
//         drawControl()
//         for viewY in 0..<size.height {
//             let frameX = viewY
//             for viewX in 0..<size.width {
//                 let frameY = size.width - 1 - viewX
//                 let viewIndex = viewY * size.width + viewX
//                 let frameIndex = frameY * frameBufferSize.width + frameX
//                 frameBuffer[frameIndex] = viewBuffer[viewIndex]
//             }
//         }
//     }

//     var volMinusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
//     var volPlusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
//     var briMinusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
//     var briPlusRect = Rect(origin: Point(x: 0, y: 0), size: Size(width: 0, height: 0))
//     var setVolume: ((Int) -> Void)?
//     var setBrightness: ((Int) -> Void)?

//     func onTap(point: Point) {
//         if volMinusRect.contains(point) {
//             setVolume?(volume - 10)
//         } else if volPlusRect.contains(point) {
//             setVolume?(volume + 10)
//         } else if briMinusRect.contains(point) {
//             setBrightness?(brightness - 10)
//         } else if briPlusRect.contains(point) {
//             setBrightness?(brightness + 10)
//         }
//     }
// }
