fileprivate let Log = Logger(tag: "M5StackTab5")

class M5StackTab5 {

    static func begin() throws(IDF.Error) -> M5StackTab5 {
        let i2c = try IDF.I2C(num: 0, scl: .gpio32, sda: .gpio31)
        let pi4io = [
            try PI4IO(i2c: i2c, address: 0x43, values: [
                (.IO_DIR    , 0b01111111),
                (.OUT_H_IM  , 0b00000000),
                (.PULL_SEL  , 0b01111111),
                (.PULL_EN   , 0b01111111),
                (.OUT_SET   , 0b01110110)
            ]),
            try PI4IO(i2c: i2c, address: 0x44, values: [
                (.IO_DIR    , 0b10111001),
                (.OUT_H_IM  , 0b00000110),
                (.PULL_SEL  , 0b10111001),
                (.PULL_EN   , 0b11111001),
                (.IN_DEF_STA, 0b01000000),
                (.INT_MASK  , 0b10111111),
                (.OUT_SET   , 0b00001001)
            ]),
        ]
        try Touch.reset(pi4io: pi4io[0], int: .gpio23)

        let display = try Display(
            backlightGpio: .gpio22,
            mipiDsiPhyPowerLdo: (channel: 3, voltageMv: 2500),
            numDataLanes: 2,
            laneBitRateMbps: 730, // 720*1280 RGB24 60Hz
            width: 720,
            height: 1280
        )
        let touch = try Touch(
            i2c: i2c,
            size: (width: 720, height: 1280),
            int: .gpio23,
            rst: nil,
            sclSpeedHz: 100000
        )
        let audio = try Audio(
            num: 1,
            mclk: .gpio30, bclk: .gpio27, ws: .gpio29, dout: .gpio26, din: .gpio28,
            i2c: i2c
        )
        let sdcard = try SDCard(
            ldo: (channel: 4, voltageMv: 3300),
            slot: .default(
                busWidth: 4,
                clk: .gpio43, cmd: .gpio44,
                data0: .gpio39, data1: .gpio40, data2: .gpio41, data3: .gpio42,
            )
        )
        return M5StackTab5(
            i2c: i2c,
            pi4io: pi4io,
            display: display,
            touch: touch,
            audio: audio,
            sdcard: sdcard
        )
    }

    let i2c: IDF.I2C
    let pi4io: [PI4IO]
    let display: Display
    let touch: Touch
    let audio: Audio
    let sdcard: SDCard
    private init(
        i2c: IDF.I2C,
        pi4io: [PI4IO],
        display: Display,
        touch: Touch,
        audio: Audio,
        sdcard: SDCard
    ) {
        self.i2c = i2c
        self.pi4io = pi4io
        self.display = display
        self.touch = touch
        self.audio = audio
        self.sdcard = sdcard
    }

    /*
     * MARK: PI4IO
     */
    class PI4IO {
        enum Register: UInt8 {
            case CHIP_RESET = 0x01
            case IO_DIR = 0x03
            case OUT_SET = 0x05
            case OUT_H_IM = 0x07
            case IN_DEF_STA = 0x09
            case PULL_EN = 0x0B
            case PULL_SEL = 0x0D
            case IN_STA = 0x0F
            case INT_MASK = 0x11
            case IRQ_STA = 0x13
        }

        let device: IDF.I2C.Device
        init(i2c: IDF.I2C, address: UInt8, values: [(Register, UInt8)]) throws(IDF.Error) {
            device = try i2c.addDevice(address: address, sclSpeedHz: 400000)
            try device.transmit([Register.CHIP_RESET.rawValue, 0xFF])
            let _ = try device.transmitReceive([Register.CHIP_RESET.rawValue], readSize: 1)
            for (reg, value) in values {
                try device.transmit([reg.rawValue, value])
            }
        }

        var output: UInt8 {
            get {
                let data = try! device.transmitReceive([Register.OUT_SET.rawValue], readSize: 1)
                return data[0]
            }
            set {
                try! device.transmit([Register.OUT_SET.rawValue, newValue])
            }
        }
    }

    /*
     * MARK: Display (ILI9881C)
     */
    class Display {
        private let ledcTimer: IDF.LEDControl.Timer
        private let backlight: IDF.LEDControl
        private let phyPowerChannel: esp_ldo_channel_handle_t?
        private let mipiDsiBus: esp_lcd_dsi_bus_handle_t
        private let io: esp_lcd_panel_io_handle_t
        let panel: esp_lcd_panel_handle_t
        let width: Int
        let height: Int

        init(
            backlightGpio: IDF.GPIO.Pin,
            mipiDsiPhyPowerLdo: (channel: Int32, voltageMv: Int32)?,
            numDataLanes: UInt8,
            laneBitRateMbps: UInt32,
            width: UInt32,
            height: UInt32,
        ) throws(IDF.Error) {
            // Setup Backlight
            ledcTimer = try IDF.LEDControl.makeTimer(dutyResolution: 12, freqHz: 5000)
            backlight = try IDF.LEDControl(gpio: backlightGpio, timer: ledcTimer)

            // Enable DSI PHY power
            if let (channel, voltageMv) = mipiDsiPhyPowerLdo {
                var ldoConfig = esp_ldo_channel_config_t(
                    chan_id: channel,
                    voltage_mv: voltageMv,
                    flags: ldo_extra_flags()
                )
                var phyPowerChannel: esp_ldo_channel_handle_t?
                try IDF.Error.check(esp_ldo_acquire_channel(&ldoConfig, &phyPowerChannel))
                self.phyPowerChannel = phyPowerChannel
            } else {
                self.phyPowerChannel = nil
            }

            // Create MIPI DSI Bus
            var busConfig = esp_lcd_dsi_bus_config_t(
                bus_id: 0,
                num_data_lanes: numDataLanes,
                phy_clk_src: MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                lane_bit_rate_mbps: laneBitRateMbps,
            )
            var mipiDsiBus: esp_lcd_dsi_bus_handle_t?
            try IDF.Error.check(esp_lcd_new_dsi_bus(&busConfig, &mipiDsiBus))
            self.mipiDsiBus = mipiDsiBus!

            // Install MIPI DSI LCD control panel
            var dbiConfig = esp_lcd_dbi_io_config_t(virtual_channel: 0, lcd_cmd_bits: 8, lcd_param_bits: 8)
            var io: esp_lcd_panel_io_handle_t?
            try IDF.Error.check(esp_lcd_new_panel_io_dbi(mipiDsiBus, &dbiConfig, &io))
            self.io = io!

            // Install LCD Driver of ILI9881C
            var dpiConfig = esp_lcd_dpi_panel_config_t(
                virtual_channel: 0,
                dpi_clk_src: MIPI_DSI_DPI_CLK_SRC_DEFAULT,
                dpi_clock_freq_mhz: 60,
                pixel_format: LCD_COLOR_PIXEL_FORMAT_RGB565,
                in_color_format: lcd_color_format_t(rawValue: 0),
                out_color_format: lcd_color_format_t(rawValue: 0),
                num_fbs: 1,
                video_timing: esp_lcd_video_timing_t(
                    h_size: width,
                    v_size: height,
                    hsync_pulse_width: 40,
                    hsync_back_porch: 140,
                    hsync_front_porch: 40,
                    vsync_pulse_width: 4,
                    vsync_back_porch: 20,
                    vsync_front_porch: 20
                ),
                flags: extra_dpi_panel_flags(use_dma2d: 1, disable_lp: 0)
            )
            self.panel = try withUnsafePointer(to: &dpiConfig) { ptr throws(IDF.Error) -> esp_lcd_panel_handle_t in
                var vendorConfig = ili9881c_vendor_config_t(
                    init_cmds: tab5_lcd_ili9881c_specific_init_code_default_ptr,
                    init_cmds_size: tab5_lcd_ili9881c_specific_init_code_default_num,
                    mipi_config: ili9881c_vendor_config_t.__Unnamed_struct_mipi_config(
                        dsi_bus: mipiDsiBus,
                        dpi_config: ptr,
                        lane_num: 2
                    )
                )
                return try withUnsafeMutablePointer(to: &vendorConfig) { ptr throws(IDF.Error) -> esp_lcd_panel_handle_t in
                    var lcdDevConfig = esp_lcd_panel_dev_config_t(
                        reset_gpio_num: -1,
                        esp_lcd_panel_dev_config_t.__Unnamed_union___Anonymous_field1(
                            rgb_ele_order: LCD_RGB_ELEMENT_ORDER_RGB
                        ),
                        data_endian: LCD_RGB_DATA_ENDIAN_BIG,
                        bits_per_pixel: 16,
                        flags: esp_lcd_panel_dev_config_t.__Unnamed_struct_flags(),
                        vendor_config: ptr
                    )

                    var dispPanel: esp_lcd_panel_handle_t?
                    try IDF.Error.check(esp_lcd_new_panel_ili9881c(io, &lcdDevConfig, &dispPanel))
                    try IDF.Error.check(esp_lcd_panel_reset(dispPanel))
                    try IDF.Error.check(esp_lcd_panel_init(dispPanel))
                    try IDF.Error.check(esp_lcd_panel_disp_on_off(dispPanel, true))
                    return dispPanel!
                }
            }
            self.width = Int(width)
            self.height = Int(height)
        }

        var brightness: Float = 0 {
            didSet {
                backlight.setDutyFloat(brightness)
            }
        }

        var frameBuffer: UnsafeMutableBufferPointer<UInt16> {
            get {
                var fb: UnsafeMutableRawPointer?
                esp_lcd_dpi_panel_get_first_frame_buffer(panel, &fb)
                let typedPointer = fb!.bindMemory(to: UInt16.self, capacity: width * height)
                return UnsafeMutableBufferPointer<UInt16>(start: typedPointer, count: width * height)
            }
        }

        func drawBitmap(start: (Int32, Int32), end: (Int32, Int32), data: UnsafeRawPointer) {
            esp_lcd_panel_draw_bitmap(panel, start.0, start.1, end.0, end.1, data)
        }
    }

    /*
     * MARK: Touch (GT911)
     */
    class Touch {
        static func reset(pi4io: PI4IO, int: IDF.GPIO.Pin) throws(IDF.Error) {
            try IDF.GPIO.reset(pin: int)
            let current = pi4io.output
            pi4io.output = current & ~(0b11 << 4)
            Task.delay(100)
            pi4io.output = current |  (0b11 << 4)
            Task.delay(100)
        }

        var ioHandle: esp_lcd_panel_io_handle_t
        var handle: esp_lcd_touch_handle_t
        let interrupt: (semaphore: Semaphore, gpio: IDF.GPIO.Pin)?

        init(
            i2c: IDF.I2C,
            size: (width: UInt16, height: UInt16),
            int: IDF.GPIO.Pin?,
            rst: IDF.GPIO.Pin?,
            sclSpeedHz: UInt32,
        ) throws(IDF.Error) {
            // Setup IO
            var ioHandle: esp_lcd_panel_handle_t?
            var ioConfig = esp_lcd_panel_io_i2c_config_t()
            _ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(&ioConfig)
            ioConfig.dev_addr = UInt32(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)
            ioConfig.scl_speed_hz = sclSpeedHz
            try IDF.Error.check(esp_lcd_new_panel_io_i2c_v2(i2c.handle, &ioConfig, &ioHandle))
            self.ioHandle = ioHandle!

            // Init GT911
            var config = esp_lcd_touch_config_t()
            config.x_max = size.width
            config.y_max = size.height
            config.rst_gpio_num = rst?.value ?? GPIO_NUM_NC
            config.int_gpio_num = int?.value ?? GPIO_NUM_NC

            var handle: esp_lcd_touch_handle_t?
            try IDF.Error.check(esp_lcd_touch_new_i2c_gt911(ioHandle, &config, &handle))
            self.handle = handle!

            if let intGpio = int {
                let semaphore = Semaphore.createBinary()
                if semaphore == nil {
                    throw IDF.Error(ESP_ERR_NO_MEM)
                }
                self.interrupt = (semaphore: semaphore!, gpio: intGpio)

                try exitSleep()
                try IDF.Error.check(esp_lcd_touch_register_interrupt_callback_with_data(handle, {
                    let semaphore = Unmanaged<Semaphore>.fromOpaque($0!.pointee.config.user_data!).takeUnretainedValue()
                    semaphore.give()
                }, Unmanaged.passUnretained(semaphore!).toOpaque()))
            } else {
                self.interrupt = nil
                try exitSleep()
            }
        }

        private func exitSleep() throws(IDF.Error) {
            try IDF.Error.check(esp_lcd_touch_exit_sleep(handle))
            if let int = interrupt?.gpio {
                var gpioConfig = gpio_config_t()
                gpioConfig.mode = GPIO_MODE_INPUT
                gpioConfig.pin_bit_mask = 1 << int.rawValue
                gpioConfig.intr_type = GPIO_INTR_NEGEDGE
                try IDF.Error.check(gpio_config(&gpioConfig))
            }
        }

        func waitInterrupt() {
            interrupt?.semaphore.take()
        }

        private var callback: ((Touch) -> Void)? = nil
        func onInterrupt(callback: @escaping (Touch) -> Void) {
            self.callback = callback
        }

        var coordinates: [Point] {
            get throws(IDF.Error) {
                try IDF.Error.check(esp_lcd_touch_read_data(handle))
                return withUnsafeTemporaryAllocation(of: UInt16.self, capacity: 10) { ptr in
                    let touchX = ptr.baseAddress!
                    let touchY = ptr.baseAddress!.advanced(by: 5)
                    var touchCount: UInt8 = 0
                    esp_lcd_touch_get_coordinates(handle, touchX, touchY, nil, &touchCount, 5)
                    return (0..<Int(touchCount)).map { Point(x: Int(touchX[$0]), y: Int(touchY[$0])) }
                }
            }
        }
    }

    /*
     * MARK: Audio (ES8388)
     */
    class Audio {
        let i2s: IDF.I2S
        let outputDevice: esp_codec_dev_handle_t

        init(
            num: UInt32? = nil,
            mclk: IDF.GPIO.Pin, bclk: IDF.GPIO.Pin, ws: IDF.GPIO.Pin, dout: IDF.GPIO.Pin?, din: IDF.GPIO.Pin?,
            i2c: IDF.I2C,
        ) throws(IDF.Error) {
            // I2S Setup
            i2s = try IDF.I2S(
                num: 0,
                role: .master,
                autoClear: (beforeCb: false, afterCb: true),
                format: (
                    tx: .std(
                        clock: .default(sampleRate: 48000),
                        slot: .philipsDefault(
                            dataBitWidth: I2S_DATA_BIT_WIDTH_16BIT,
                            slotMode: I2S_SLOT_MODE_MONO
                        ),
                        gpio: .init(mclk: mclk, bclk: bclk, ws: ws, dout: dout, din: din)
                    ),
                    rx: .tdm(
                        clock: .init(
                            sampleRate: 48000,
                            clkSrc: I2S_CLK_SRC_DEFAULT,
                            extClkFreq: 0,
                            mclkMultiple: I2S_MCLK_MULTIPLE_256,
                            bclkDiv: 0
                        ),
                        slot: .init(
                            dataBitWidth: I2S_DATA_BIT_WIDTH_16BIT,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: I2S_SLOT_MODE_STEREO,
                            slotMask: [.slot0, .slot1, .slot2, .slot3],
                            wsWidth: UInt32(I2S_TDM_AUTO_WS_WIDTH),
                            wsPol: false,
                            bitShift: true,
                            leftAlign: false,
                            bigEndian: false,
                            bitOrderLsb: false,
                            skipMask: false,
                            totalSlot: UInt32(I2S_TDM_AUTO_SLOT_NUM)
                        ),
                        gpio: .init(mclk: mclk, bclk: bclk, ws: ws, dout: dout, din: din)
                    ),
                )
            )
            outputDevice = Audio.initSpeaker(i2s: i2s, i2c: i2c)
            volume = 0
            try reconfigOutput(rate: 48000, bps: 16, ch: 2)
        }

        static func initSpeaker(i2s: IDF.I2S, i2c: IDF.I2C) -> esp_codec_dev_handle_t {
            // let gpioInterface = audio_codec_new_gpio()
            //     .unwrap(errMsg: { "Failed to create audio codec GPIO Interface" })
            var i2cConfig = audio_codec_i2c_cfg_t(
                port: UInt8(i2c.portNumber),
                addr: UInt8(ES8388_CODEC_DEFAULT_ADDR),
                bus_handle: UnsafeMutableRawPointer(i2c.handle)
            )
            let i2cControlInterface = audio_codec_new_i2c_ctrl(&i2cConfig)
                .unwrap(errMsg: { "Failed to create Speaker codec I2C Interface" })

            // let gain = esp_codec_dev_hw_gain_t(
            //     pa_voltage: 5.0,
            //     codec_dac_voltage: 3.3,
            //     pa_gain: 0
            // )
            var es8388Config = es8388_codec_cfg_t()
            es8388Config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC
            es8388Config.master_mode = false
            es8388Config.ctrl_if = i2cControlInterface
            es8388Config.pa_pin = -1
            let es8388Dev = es8388_codec_new(&es8388Config)
                .unwrap(errMsg: { "Failed to create ES8388 codec" })

            var codecDevConfig = esp_codec_dev_cfg_t(
                dev_type: ESP_CODEC_DEV_TYPE_OUT,
                codec_if: es8388Dev,
                data_if: i2s.interface
            )
            return esp_codec_dev_new(&codecDevConfig)
                .unwrap(errMsg: { "Failed to create Speaker codec device" })
        }

        func reconfigOutput(rate: UInt32, bps: UInt8, ch: UInt8) throws(IDF.Error) {
            var fs = esp_codec_dev_sample_info_t()
            fs.sample_rate = rate
            fs.channel = ch
            fs.bits_per_sample = bps
            try IDF.Error.check(esp_codec_dev_close(outputDevice))
            try IDF.Error.check(esp_codec_dev_open(outputDevice, &fs))
        }
        func write(_ data: UnsafeMutableRawBufferPointer) throws(IDF.Error) {
            try IDF.Error.check(esp_codec_dev_write(outputDevice, data.baseAddress!, Int32(data.count)))
        }

        var volume: Int = -1 {
            didSet {
                volume = max(0, min(100, volume))
                do throws(IDF.Error) {
                    if (volume <= 0) {
                        try IDF.Error.check(esp_codec_dev_set_out_mute(outputDevice, true))
                    } else {
                        try IDF.Error.check(esp_codec_dev_set_out_mute(outputDevice, false))
                        try IDF.Error.check(esp_codec_dev_set_out_vol(outputDevice, Int32(volume)))
                    }
                } catch {
                    Log.error("Failed to set volume: \(error)")
                }
            }
        }
    }

    /*
     * MARK: SDCard
     */
    class SDCard {
        let powerControl: sd_pwr_ctrl_handle_t
        var sdmmc: IDF.SDMMC? = nil
        let slotConfig: IDF.SDMMC.SlotConfig

        init(
            ldo: (channel: Int32, voltageMv: Int32),
            slot: IDF.SDMMC.SlotConfig
        ) throws(IDF.Error) {
            // Setup SDCard Power (LDO)
            var ldoConfig = sd_pwr_ctrl_ldo_config_t(ldo_chan_id: ldo.channel)
            var powerControl: sd_pwr_ctrl_handle_t?
            try IDF.Error.check(sd_pwr_ctrl_new_on_chip_ldo(&ldoConfig, &powerControl))
            self.powerControl = powerControl!
            self.slotConfig = slot
        }

        var isMounted: Bool {
            return sdmmc != nil
        }

        func mount(path: String, maxFiles: Int32) throws(IDF.Error) {
            var host = IDF.SDMMC.HostConfig.default
            host.slot = SDMMC_HOST_SLOT_0
            host.max_freq_khz = SDMMC_FREQ_HIGHSPEED
            sdmmc = try IDF.SDMMC.mount(
                path: path,
                host: host,
                slot: slotConfig,
                maxFiles: maxFiles
            )
            sdmmc!.printInfo()
        }
    }
}
