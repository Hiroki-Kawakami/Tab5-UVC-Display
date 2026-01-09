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

    var volume: Int = 50 {
        didSet { self.tab5.audio.volume = volume }
    }
    private var volumeSlider: LVGL.Slider!
    private lazy var onVolumeSliderValueChanged = FFI.Wrapper {
        self.volume = Int(self.volumeSlider.getValue())
    }

    var brightness: Int = 50 {
        didSet { self.tab5.display.brightness = brightness }
    }
    private var brightnessSlider: LVGL.Slider!
    private lazy var onBrightnessSliderValueChanged = FFI.Wrapper {
        self.brightness = Int(self.brightnessSlider.getValue())
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

        let volumeLabel = LVGL.Label(parent: screen)
        volumeLabel.setText("Volume")
        volumeLabel.setWidth(280)
        volumeLabel.align(.topMid, yOffset: 20)
        volumeSlider = LVGL.Slider(parent: screen)
        volumeSlider.setWidth(280)
        volumeSlider.alignTo(base: volumeLabel, align: .outBottomMid, yOffset: 20)
        volumeSlider.setRange(min: 1, max: 100)
        volumeSlider.setValue(Int32(volume), anim: false)
        volumeSlider.addEventCallback(filter: .valueChanged, callback: onVolumeSliderValueChanged)

        let brightnessLabel = LVGL.Label(parent: screen)
        brightnessLabel.setText("Brightness")
        brightnessLabel.setWidth(280)
        brightnessLabel.alignTo(base: volumeSlider, align: .outBottomMid, yOffset: 40)
        brightnessSlider = LVGL.Slider(parent: screen)
        brightnessSlider.setWidth(280)
        brightnessSlider.alignTo(base: brightnessLabel, align: .outBottomMid, yOffset: 20)
        brightnessSlider.setRange(min: 1, max: 100)
        brightnessSlider.setValue(Int32(brightness), anim: false)
        brightnessSlider.addEventCallback(filter: .valueChanged, callback: onBrightnessSliderValueChanged)
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

fileprivate extension LVGL.ObjectProtocol {
    func addEventCallback(filter: lv_event_code_t, callback: FFI.Wrapper<() -> ()>) {
        addEventCb({
            let event = LVGL.Event(e: $0!)
            FFI.Wrapper<() -> ()>.unretained(event.getUserData())()
        }, filter: filter, userData: callback.passUnretained())
    }
}
