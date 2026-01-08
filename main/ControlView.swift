class ControlView<PixelFormat: Pixel> {

    var visible: Bool = true

    private var currentBuffer: UnsafeMutablePointer<UInt8>?
    private var guiBuffers: [UnsafeMutableBufferPointer<lv_color_t>]
    private var tab5: M5StackTab5<PixelFormat>
    private let ppa: IDF.PPAClient
    private let saveSettings: () -> ()

    init(tab5: M5StackTab5<PixelFormat>, saveSettings: @escaping () -> ()) throws(IDF.Error) {
        self.guiBuffers = [
            Memory.allocate(type: lv_color_t.self, capacity: 320 * 480, capability: .spiram)!,
        ]
        self.tab5 = tab5
        ppa = try IDF.PPAClient(operType: .srm)
        self.saveSettings = saveSettings
        LVGL.withLock { createDisplay() }
    }

    private func createDisplay() {
        let display = LVGL.Display.createDirectBufferDisplay(
            buffer: guiBuffers[0].baseAddress,
            size: Size(width: 320, height: 480)
        ) { display, buffer in
            self.currentBuffer = buffer
            display.flushReady()
        }
        display.setDefault()

        let touch = TouchStateMachine()
        touch.onEvent { event in
            guard case .tap(_) = event else { return }
            self.visible = !self.visible
            print("Control Visible: \(self.visible)")
        }
        let _ = LVGL.Indev.createPollingPointerDevice { indev, data in
            guard let point = (try? self.tab5.touch.coordinates)?.first else {
                data.pointee.state = .released
                touch.onTouch(coordinates: [])
                return
            }
            if self.visible && point.y < 480 {
                data.pointee.point.x = 320 - Int32(point.y) * 320 / 480
                data.pointee.point.y = Int32(point.x) * 480 / 720
                data.pointee.state = .pressed
            } else {
                touch.onTouch(coordinates: [point])
            }
        }

        let screen = LVGL.Screen.active
        screen.setStyleBgColor(LVGL.Color(hex: 0xCCCCCC))

        let brightnessLabel = LVGL.Label(parent: screen)
        brightnessLabel.setText("Brightness")
        brightnessLabel.setWidth(280)
        brightnessLabel.align(.topMid, yOffset: 20)
        let brightnessSlider = LVGL.Slider(parent: screen)
        brightnessSlider.setWidth(280)
        brightnessSlider.alignTo(base: brightnessLabel, align: .outBottomMid, yOffset: 20)
        brightnessSlider.setRange(min: 1, max: 100)
        brightnessSlider.setValue(Int32(tab5.display.brightness), anim: false)
        let valueChanged = FFI.Wrapper<() -> Void> {
            self.tab5.display.brightness = Int(brightnessSlider.getValue())
        }
        let released = FFI.Wrapper<() -> Void> {
            self.saveSettings()
        }
        brightnessSlider.addEventCb({ obj in
            let event = LVGL.Event(e: obj!)
            Unmanaged<FFI.Wrapper<() -> Void>>.fromOpaque(event.getUserData()).takeUnretainedValue().value()
        }, filter: .valueChanged, userData: Unmanaged.passRetained(valueChanged).toOpaque())
        brightnessSlider.addEventCb({ obj in
            let event = LVGL.Event(e: obj!)
            Unmanaged<FFI.Wrapper<() -> Void>>.fromOpaque(event.getUserData()).takeUnretainedValue().value()
        }, filter: .released, userData: Unmanaged.passRetained(released).toOpaque())
    }

    func push(fbIndex: Int) {
        let colorMode: IDF.PPAClient.SRMColorMode = MemoryLayout<PixelFormat>.size == 2 ? .rgb565 : .rgb888
        try? ppa.srm(
            input: (buffer: UnsafeRawBufferPointer(start: currentBuffer, count: self.guiBuffers[0].count), size: Size(width: 320, height: 480), block: nil, colorMode: .rgb565),
            output: (buffer: UnsafeMutableRawBufferPointer(tab5.display.frameBuffers[fbIndex]), size: Size(width: 720, height: 1280), block: Rect(x: 0, y: 0, width: 720, height: 480), colorMode: colorMode),
            rotate: 90
        )
    }
}
