class IDF {

    struct Error: Swift.Error, CustomStringConvertible {
        let rawValue: esp_err_t
        init(_ rawValue: esp_err_t) {
            self.rawValue = rawValue
        }

        static func check(_ result: esp_err_t) throws(IDF.Error) {
            if result != ESP_OK {
                throw IDF.Error(result)
            }
        }

        var description: String {
            String(cString: esp_err_to_name(rawValue))
        }
    }

    struct ResourcePool {
        let max: UInt32
        var used: UInt32 = 0
        init(max: UInt32) {
            self.max = max
        }

        mutating func take(_ value: UInt32? = nil) -> UInt32 {
            if let value = value {
                if value >= max {
                    fatalError("Resource out of range")
                }
                if used & (1 << value) != 0 {
                    fatalError("Resource already taken")
                }
                used |= (1 << value)
                return value
            } else {
                for i in 0..<max {
                    if used & (1 << i) == 0 {
                        used |= (1 << i)
                        return i
                    }
                }
                fatalError("No more resources available")
            }
        }
    }

    /*
     * MARK: GPIO
     */
    class GPIO {
        enum Pin: Int32 {
            case gpio0 = 0
            case gpio1 = 1
            case gpio2 = 2
            case gpio3 = 3
            case gpio4 = 4
            case gpio5 = 5
            case gpio6 = 6
            case gpio7 = 7
            case gpio8 = 8
            case gpio9 = 9
            case gpio10 = 10
            case gpio11 = 11
            case gpio12 = 12
            case gpio13 = 13
            case gpio14 = 14
            case gpio15 = 15
            case gpio16 = 16
            case gpio17 = 17
            case gpio18 = 18
            case gpio19 = 19
            case gpio20 = 20
            case gpio21 = 21
            case gpio22 = 22
            case gpio23 = 23
            case gpio24 = 24
            case gpio25 = 25
            case gpio26 = 26
            case gpio27 = 27
            case gpio28 = 28
            case gpio29 = 29
            case gpio30 = 30
            case gpio31 = 31
            case gpio32 = 32
            case gpio33 = 33
            case gpio34 = 34
            case gpio35 = 35
            case gpio36 = 36
            case gpio37 = 37
            case gpio38 = 38
            case gpio39 = 39
            case gpio40 = 40
            case gpio41 = 41
            case gpio42 = 42
            case gpio43 = 43
            case gpio44 = 44
            case gpio45 = 45
            case gpio46 = 46
            case gpio47 = 47
            case gpio48 = 48
            case gpio49 = 49
            case gpio50 = 50
            case gpio51 = 51
            case gpio52 = 52
            case gpio53 = 53
            case gpio54 = 54

            var value: gpio_num_t {
                return gpio_num_t(rawValue)
            }
        }

        static func reset(pin: Pin) throws(IDF.Error) {
            try IDF.Error.check(gpio_reset_pin(pin.value))
        }
    }

    /*
     * MARK: LED Control
     */
    class LEDControl {
        class Timer {
            let rawValue: ledc_timer_t;
            let dutyResolution: UInt32
            init(rawValue: ledc_timer_t, dutyResolution: UInt32) {
                self.rawValue = rawValue
                self.dutyResolution = dutyResolution
            }
        }
        private static var timerPool: IDF.ResourcePool = IDF.ResourcePool(max: LEDC_TIMER_MAX.rawValue)
        static func makeTimer(dutyResolution: UInt32, freqHz: UInt32) throws(IDF.Error) -> Timer {
            let timer = ledc_timer_t(timerPool.take())
            var config = ledc_timer_config_t(
                speed_mode: LEDC_LOW_SPEED_MODE,
                duty_resolution: ledc_timer_bit_t(rawValue: dutyResolution),
                timer_num: timer,
                freq_hz: freqHz,
                clk_cfg: LEDC_AUTO_CLK,
                deconfigure: false
            )
            try IDF.Error.check(ledc_timer_config(&config))
            return Timer(rawValue: timer, dutyResolution: dutyResolution)
        }

        private static var channelPool = IDF.ResourcePool(max: LEDC_CHANNEL_MAX.rawValue)
        let channel: ledc_channel_t
        let timer: Timer

        init(gpio: GPIO.Pin, timer: Timer) throws(IDF.Error) {
            channel = ledc_channel_t(Self.channelPool.take())
            self.timer = timer
            var config = ledc_channel_config_t(
                gpio_num: gpio.rawValue,
                speed_mode: LEDC_LOW_SPEED_MODE,
                channel: channel,
                intr_type: LEDC_INTR_DISABLE,
                timer_sel: timer.rawValue,
                duty: 0,
                hpoint: 0,
                sleep_mode: LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
                flags: ledc_channel_config_t.__Unnamed_struct_flags()
            )
            try IDF.Error.check(ledc_channel_config(&config))
        }

        var duty: UInt32 = 0 {
            didSet {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty)
                ledc_update_duty(LEDC_LOW_SPEED_MODE, channel)
            }
        }

        func setDutyFloat(_ duty: Float) {
            let duty = duty.clamp(minValue: 0.0, maxValue: 1.0)
            let maxDuty = (1 << timer.dutyResolution) - 1
            self.duty = UInt32(Float(maxDuty) * duty)
        }
    }

    /*
     * MARK: I2C
     */
    class I2C {
        private static var i2cPool = IDF.ResourcePool(max: SOC_I2C_NUM)
        let portNumber: i2c_port_num_t
        let handle: i2c_master_bus_handle_t
        init(num: UInt32? = nil, scl: GPIO.Pin, sda: GPIO.Pin) throws(IDF.Error) {
            portNumber = i2c_port_num_t(Self.i2cPool.take(num))
            var config = i2c_master_bus_config_t(
                i2c_port: portNumber,
                sda_io_num: sda.value,
                scl_io_num: scl.value,
                i2c_master_bus_config_t.__Unnamed_union___Anonymous_field3(clk_source: I2C_CLK_SRC_DEFAULT),
                glitch_ignore_cnt: 0,
                intr_priority: 0,
                trans_queue_depth: 0,
                flags: i2c_master_bus_config_t.__Unnamed_struct_flags(
                    enable_internal_pullup: 1,
                    allow_pd: 0,
                )
            )
            var handle: i2c_master_bus_handle_t? = nil
            try IDF.Error.check(i2c_new_master_bus(&config, &handle))
            self.handle = handle!
        }

        class Device {
            let handle: i2c_master_dev_handle_t
            init(handle: i2c_master_dev_handle_t) {
                self.handle = handle
            }

            func transmit(_ data: [UInt8], timeoutMs: Int32 = 50) throws(IDF.Error) {
                var data = data
                try IDF.Error.check(i2c_master_transmit(handle, &data, data.count, timeoutMs))
            }

            func transmitReceive(_ data: [UInt8], readSize: Int, timeoutMs: Int32 = 50) throws(IDF.Error) -> [UInt8] {
                var data = data
                var readData = [UInt8](repeating: 0, count: readSize)
                try IDF.Error.check(i2c_master_transmit_receive(handle, &data, data.count, &readData, readSize, timeoutMs))
                return readData
            }
        }
        func addDevice(address: UInt8, sclSpeedHz: UInt32, sclWaitUs: UInt32 = 0) throws(IDF.Error) -> Device {
            var config = i2c_device_config_t(
                dev_addr_length: I2C_ADDR_BIT_LEN_7,
                device_address: UInt16(address),
                scl_speed_hz: sclSpeedHz,
                scl_wait_us: sclWaitUs,
                flags: i2c_device_config_t.__Unnamed_struct_flags()
            )
            var handle: i2c_master_dev_handle_t? = nil
            try IDF.Error.check(i2c_master_bus_add_device(self.handle, &config, &handle))
            return Device(handle: handle!)
        }
    }

    /*
     * MARK: I2S
     */
    class I2S {

        enum Role {
            case master
            case slave

            var value: i2s_role_t {
                switch self {
                case .master: return I2S_ROLE_MASTER
                case .slave: return I2S_ROLE_SLAVE
                }
            }
        }

        enum Format {
            class STD {
                struct ClockConfig {
                    let sampleRate: UInt32
                    let clkSrc: soc_periph_i2s_clk_src_t
                    let extClkFreq: UInt32
                    let mclkMultiple: i2s_mclk_multiple_t

                    static func `default`(sampleRate: UInt32) -> ClockConfig {
                        return ClockConfig(
                            sampleRate: sampleRate,
                            clkSrc: I2S_CLK_SRC_DEFAULT,
                            extClkFreq: 0,
                            mclkMultiple: I2S_MCLK_MULTIPLE_256
                        )
                    }

                    var value: i2s_std_clk_config_t {
                        return i2s_std_clk_config_t(
                            sample_rate_hz: sampleRate,
                            clk_src: clkSrc,
                            ext_clk_freq_hz: extClkFreq,
                            mclk_multiple: mclkMultiple
                        )
                    }
                }
                struct SlotConfig {
                    let dataBitWidth: i2s_data_bit_width_t
                    let slotBitWidth: i2s_slot_bit_width_t
                    let slotMode: i2s_slot_mode_t
                    let slotMask: i2s_std_slot_mask_t
                    let wsWidth: UInt32
                    let wsPol: Bool
                    let bitShift: Bool
                    let leftAlign: Bool
                    let bigEndian: Bool
                    let bitOrderLsb: Bool

                    static func philipsDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: I2S_STD_SLOT_BOTH,
                            wsWidth: dataBitWidth.rawValue,
                            wsPol: false,
                            bitShift: true,
                            leftAlign: true,
                            bigEndian: false,
                            bitOrderLsb: false
                        )
                    }
                    static func pcmDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: I2S_STD_SLOT_BOTH,
                            wsWidth: 1,
                            wsPol: true,
                            bitShift: true,
                            leftAlign: true,
                            bigEndian: false,
                            bitOrderLsb: false
                        )
                    }
                    static func msbDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: I2S_STD_SLOT_BOTH,
                            wsWidth: dataBitWidth.rawValue,
                            wsPol: false,
                            bitShift: false,
                            leftAlign: true,
                            bigEndian: false,
                            bitOrderLsb: false
                        )
                    }

                    var value: i2s_std_slot_config_t {
                        return i2s_std_slot_config_t(
                            data_bit_width: dataBitWidth,
                            slot_bit_width: slotBitWidth,
                            slot_mode: slotMode,
                            slot_mask: slotMask,
                            ws_width: wsWidth,
                            ws_pol: wsPol,
                            bit_shift: bitShift,
                            left_align: leftAlign,
                            big_endian: bigEndian,
                            bit_order_lsb: bitOrderLsb
                        )
                    }
                }
                struct GPIOConfig {
                    let mclk: GPIO.Pin?
                    let bclk: GPIO.Pin
                    let ws: GPIO.Pin
                    let dout: GPIO.Pin?
                    let din: GPIO.Pin?

                    var value: i2s_std_gpio_config_t {
                        var config = i2s_std_gpio_config_t()
                        config.mclk = mclk?.value ?? GPIO_NUM_NC
                        config.bclk = bclk.value
                        config.ws = ws.value
                        config.dout = dout?.value ?? GPIO_NUM_NC
                        config.din = din?.value ?? GPIO_NUM_NC
                        return config
                    }
                }
            }
            case std(clock: STD.ClockConfig, slot: STD.SlotConfig, gpio: STD.GPIOConfig)

            var i2sConfig: i2s_std_config_t? {
                guard case let .std(clock, slot, gpio) = self else { return nil }
                return i2s_std_config_t(
                    clk_cfg: clock.value,
                    slot_cfg: slot.value,
                    gpio_cfg: gpio.value
                )
            }

            class TDM {
                struct ClockConfig {
                    let sampleRate: UInt32
                    let clkSrc: i2s_clock_src_t
                    let extClkFreq: UInt32
                    let mclkMultiple: i2s_mclk_multiple_t
                    let bclkDiv: UInt32

                    static func `default`(sampleRate: UInt32) -> ClockConfig {
                        return ClockConfig(
                            sampleRate: sampleRate,
                            clkSrc: I2S_CLK_SRC_DEFAULT,
                            extClkFreq: 0,
                            mclkMultiple: I2S_MCLK_MULTIPLE_256,
                            bclkDiv: 0
                        )
                    }

                    var value: i2s_tdm_clk_config_t {
                        return i2s_tdm_clk_config_t(
                            sample_rate_hz: sampleRate,
                            clk_src: clkSrc,
                            ext_clk_freq_hz: extClkFreq,
                            mclk_multiple: mclkMultiple,
                            bclk_div: bclkDiv
                        )
                    }
                }
                struct SlotConfig {

                    struct SlotMask: OptionSet {
                        let rawValue: UInt32
                        static let slot0 = SlotMask(rawValue: 1 << 0)
                        static let slot1 = SlotMask(rawValue: 1 << 1)
                        static let slot2 = SlotMask(rawValue: 1 << 2)
                        static let slot3 = SlotMask(rawValue: 1 << 3)
                        static let slot4 = SlotMask(rawValue: 1 << 4)
                        static let slot5 = SlotMask(rawValue: 1 << 5)
                        static let slot6 = SlotMask(rawValue: 1 << 6)
                        static let slot7 = SlotMask(rawValue: 1 << 7)
                        static let slot8 = SlotMask(rawValue: 1 << 8)
                        static let slot9 = SlotMask(rawValue: 1 << 9)
                        static let slot10 = SlotMask(rawValue: 1 << 10)
                        static let slot11 = SlotMask(rawValue: 1 << 11)
                        static let slot12 = SlotMask(rawValue: 1 << 12)
                        static let slot13 = SlotMask(rawValue: 1 << 13)
                        static let slot14 = SlotMask(rawValue: 1 << 14)
                        static let slot15 = SlotMask(rawValue: 1 << 15)
                    }

                    let dataBitWidth: i2s_data_bit_width_t
                    let slotBitWidth: i2s_slot_bit_width_t
                    let slotMode: i2s_slot_mode_t
                    let slotMask: SlotMask
                    let wsWidth: UInt32
                    let wsPol: Bool
                    let bitShift: Bool
                    let leftAlign: Bool
                    let bigEndian: Bool
                    let bitOrderLsb: Bool
                    let skipMask: Bool
                    let totalSlot: UInt32

                    static func philipsDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                        slotMask: SlotMask
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: slotMask,
                            wsWidth: UInt32(I2S_TDM_AUTO_WS_WIDTH),
                            wsPol: false,
                            bitShift: true,
                            leftAlign: false,
                            bigEndian: false,
                            bitOrderLsb: false,
                            skipMask: false,
                            totalSlot: UInt32(I2S_TDM_AUTO_SLOT_NUM)
                        )
                    }
                    static func msbDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                        slotMask: SlotMask
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: slotMask,
                            wsWidth: UInt32(I2S_TDM_AUTO_WS_WIDTH),
                            wsPol: false,
                            bitShift: false,
                            leftAlign: false,
                            bigEndian: false,
                            bitOrderLsb: false,
                            skipMask: false,
                            totalSlot: UInt32(I2S_TDM_AUTO_SLOT_NUM)
                        )
                    }
                    static func pcmShortDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                        slotMask: SlotMask
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: slotMask,
                            wsWidth: 1,
                            wsPol: true,
                            bitShift: true,
                            leftAlign: false,
                            bigEndian: false,
                            bitOrderLsb: false,
                            skipMask: false,
                            totalSlot: UInt32(I2S_TDM_AUTO_SLOT_NUM)
                        )
                    }
                    static func pcmLongDefault(
                        dataBitWidth: i2s_data_bit_width_t,
                        slotMode: i2s_slot_mode_t,
                        slotMask: SlotMask
                    ) -> SlotConfig {
                        return SlotConfig(
                            dataBitWidth: dataBitWidth,
                            slotBitWidth: I2S_SLOT_BIT_WIDTH_AUTO,
                            slotMode: slotMode,
                            slotMask: slotMask,
                            wsWidth: dataBitWidth.rawValue,
                            wsPol: true,
                            bitShift: true,
                            leftAlign: false,
                            bigEndian: false,
                            bitOrderLsb: false,
                            skipMask: false,
                            totalSlot: UInt32(I2S_TDM_AUTO_SLOT_NUM)
                        )
                    }

                    var value: i2s_tdm_slot_config_t {
                        return i2s_tdm_slot_config_t(
                            data_bit_width: dataBitWidth,
                            slot_bit_width: slotBitWidth,
                            slot_mode: slotMode,
                            slot_mask: i2s_tdm_slot_mask_t(rawValue: slotMask.rawValue),
                            ws_width: wsWidth,
                            ws_pol: wsPol,
                            bit_shift: bitShift,
                            left_align: leftAlign,
                            big_endian: bigEndian,
                            bit_order_lsb: bitOrderLsb,
                            skip_mask: skipMask,
                            total_slot: totalSlot
                        )
                    }
                }
                struct GPIOConfig {
                    let mclk: GPIO.Pin?
                    let bclk: GPIO.Pin
                    let ws: GPIO.Pin
                    let dout: GPIO.Pin?
                    let din: GPIO.Pin?

                    var value: i2s_tdm_gpio_config_t {
                        var config = i2s_tdm_gpio_config_t()
                        config.mclk = mclk?.value ?? GPIO_NUM_NC
                        config.bclk = bclk.value
                        config.ws = ws.value
                        config.dout = dout?.value ?? GPIO_NUM_NC
                        config.din = din?.value ?? GPIO_NUM_NC
                        return config
                    }
                }
            }
            case tdm(clock: TDM.ClockConfig, slot: TDM.SlotConfig, gpio: TDM.GPIOConfig)

            var tdmConfig: i2s_tdm_config_t? {
                guard case let .tdm(clock, slot, gpio) = self else { return nil }
                return i2s_tdm_config_t(
                    clk_cfg: clock.value,
                    slot_cfg: slot.value,
                    gpio_cfg: gpio.value
                )
            }
        }

        private static var i2sPool = IDF.ResourcePool(max: SOC_I2S_NUM)

        let port: i2s_port_t
        let channels: (tx: i2s_chan_handle_t, rx: i2s_chan_handle_t)
        let interface: UnsafePointer<audio_codec_data_if_t>

        init(
            num: UInt32? = nil, role: Role = .master,
            dmaDescNum: UInt32 = 6, dmaFrameNum: UInt32 = 240,
            autoClear: (beforeCb: Bool, afterCb: Bool) = (false, false),
            allowPd: Bool = false, intrPriority: Int32 = 0,
            format: (tx: Format, rx: Format),
        ) throws(IDF.Error) {
            port = i2s_port_t(Self.i2sPool.take(num))
            var channelConfig = i2s_chan_config_t()
            channelConfig.id = port
            channelConfig.role = role.value
            channelConfig.dma_desc_num = dmaDescNum
            channelConfig.dma_frame_num = dmaFrameNum
            channelConfig.auto_clear_after_cb = autoClear.afterCb
            channelConfig.auto_clear_before_cb = autoClear.beforeCb
            channelConfig.allow_pd = allowPd
            channelConfig.intr_priority = intrPriority

            var tx: i2s_chan_handle_t?
            var rx: i2s_chan_handle_t?
            try IDF.Error.check(i2s_new_channel(&channelConfig, &tx, &rx))
            self.channels = (tx: tx!, rx: rx!)

            let initFormatMode: (i2s_chan_handle_t, Format) throws(IDF.Error) -> Void = {
                if var i2sConfig = $1.i2sConfig {
                    try IDF.Error.check(i2s_channel_init_std_mode($0, &i2sConfig))
                }
                if var tdmConfig = $1.tdmConfig {
                    try IDF.Error.check(i2s_channel_init_tdm_mode($0, &tdmConfig))
                }
                try IDF.Error.check(i2s_channel_enable($0))
            }
            try initFormatMode(channels.tx, format.tx)
            try initFormatMode(channels.rx, format.rx)

            var i2sConfig = audio_codec_i2s_cfg_t(
                port: UInt8(port.rawValue),
                rx_handle: UnsafeMutableRawPointer(channels.rx),
                tx_handle: UnsafeMutableRawPointer(channels.tx),
            )
            interface = audio_codec_new_i2s_data(&i2sConfig)
        }
    }

    /*
     * MARK: JPEG
     */
    class JPEG {

        enum DecoderOutFormat {
            case rgb888
            case rgb565
            case gray
            case yuv444
            case yuv422
            case yuv420

            var value: jpeg_dec_output_format_t {
                switch self {
                case .rgb888: return JPEG_DECODE_OUT_FORMAT_RGB888
                case .rgb565: return JPEG_DECODE_OUT_FORMAT_RGB565
                case .gray  : return JPEG_DECODE_OUT_FORMAT_GRAY
                case .yuv444: return JPEG_DECODE_OUT_FORMAT_YUV444
                case .yuv422: return JPEG_DECODE_OUT_FORMAT_YUV422
                case .yuv420: return JPEG_DECODE_OUT_FORMAT_YUV420
                }
            }
        }

        enum DecoderRGBConversion {
            case bt601
            case bt709

            var value: jpeg_yuv_rgb_conv_std_t {
                switch self {
                case .bt601: return JPEG_YUV_RGB_CONV_STD_BT601
                case .bt709: return JPEG_YUV_RGB_CONV_STD_BT709
                }
            }
        }

        enum DecoderRGBElementOrder {
            case rgb
            case bgr

            var value: jpeg_dec_rgb_element_order_t {
                switch self {
                case .rgb: return JPEG_DEC_RGB_ELEMENT_ORDER_RGB
                case .bgr: return JPEG_DEC_RGB_ELEMENT_ORDER_BGR
                }
            }
        }

        enum DecoderBufferDirection {
            case input
            case output

            var value: jpeg_dec_buffer_alloc_direction_t {
                switch self {
                case .input: return JPEG_DEC_ALLOC_INPUT_BUFFER
                case .output: return JPEG_DEC_ALLOC_OUTPUT_BUFFER
                }
            }
        }

        static func createDecoderRgb565(
            rgbElementOrder: DecoderRGBElementOrder,
            rgbConversion: DecoderRGBConversion = .bt601,
            intrPriority: Int32 = 0,
            timeout: Int32 = 100,
        ) throws(IDF.Error) -> Decoder<UInt16> {
            let decodeConfig = jpeg_decode_cfg_t(
                output_format: DecoderOutFormat.rgb565.value,
                rgb_order: rgbElementOrder.value,
                conv_std: rgbConversion.value
            )
            return try Decoder<UInt16>(intrPriority: intrPriority, timeout: timeout, decodeConfig: decodeConfig)
        }

        class Decoder<E> {
            private let engine: jpeg_decoder_handle_t
            private var decodeConfig: jpeg_decode_cfg_t

            fileprivate init(intrPriority: Int32, timeout: Int32, decodeConfig: jpeg_decode_cfg_t) throws(IDF.Error) {
                var engine: jpeg_decoder_handle_t?
                var config = jpeg_decode_engine_cfg_t(
                    intr_priority: intrPriority,
                    timeout_ms: timeout
                )
                try IDF.Error.check(jpeg_new_decoder_engine(&config, &engine))
                self.engine = engine!
                self.decodeConfig = decodeConfig
            }

            deinit {
                jpeg_del_decoder_engine(engine)
            }

            static func allocateOutputBuffer(capacity: Int) -> UnsafeMutableBufferPointer<E>? {
                let size = MemoryLayout<E>.size * capacity
                var allocatedSize = 0
                var allocConfig = jpeg_decode_memory_alloc_cfg_t(buffer_direction: JPEG_DEC_ALLOC_OUTPUT_BUFFER)
                let pointer = jpeg_alloc_decoder_mem(size, &allocConfig, &allocatedSize)
                if pointer == nil {
                    return nil
                }
                return UnsafeMutableBufferPointer<E>(
                    start: pointer?.bindMemory(to: E.self, capacity: allocatedSize / MemoryLayout<E>.size),
                    count: allocatedSize / MemoryLayout<E>.size
                )
            }

            func decode(inputBuffer: UnsafeRawBufferPointer, outputBuffer: UnsafeMutableBufferPointer<E>) throws(IDF.Error) -> UInt32 {
                var decodeSize: UInt32 = 0
                try IDF.Error.check(
                    jpeg_decoder_process(
                        engine, &decodeConfig,
                        inputBuffer.baseAddress, UInt32(inputBuffer.count),
                        outputBuffer.baseAddress, UInt32(MemoryLayout<E>.size * outputBuffer.count),
                        &decodeSize
                    )
                )
                return decodeSize
            }
        }
    }

    /*
     * MARK: Pixel-Processing Accelerator (PPA)
     */
    class PPAClient {
        let client: ppa_client_handle_t

        enum Operation {
            case srm
            case blend
            case fill

            var value: ppa_operation_t {
                switch self {
                case .srm: return PPA_OPERATION_SRM
                case .blend: return PPA_OPERATION_BLEND
                case .fill: return PPA_OPERATION_FILL
                }
            }
        }

        init(operType: Operation) throws(IDF.Error) {
            var client: ppa_client_handle_t?
            var config = ppa_client_config_t()
            config.oper_type = operType.value
            try IDF.Error.check(ppa_register_client(&config, &client))
            self.client = client!
        }

        func rotate90(
            inputBuffer: UnsafeMutableBufferPointer<UInt16>,
            outputBuffer: UnsafeMutableBufferPointer<UInt16>,
            size: (width: UInt32, height: UInt32),
        ) throws(IDF.Error) {
            var config = ppa_srm_oper_config_t()
            config.in.buffer = UnsafeRawPointer(inputBuffer.baseAddress)
            config.in.pic_w = size.width
            config.in.pic_h = size.height
            config.in.block_w = size.width
            config.in.block_h = size.height
            config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565
            config.out.buffer = UnsafeMutableRawPointer(outputBuffer.baseAddress)
            config.out.buffer_size = UInt32(outputBuffer.count * MemoryLayout<UInt16>.size)
            config.out.pic_w = size.height
            config.out.pic_h = size.width
            config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565
            config.rotation_angle = PPA_SRM_ROTATION_ANGLE_90
            config.scale_x = 1
            config.scale_y = 1
            config.mode = PPA_TRANS_MODE_BLOCKING
            try IDF.Error.check(ppa_do_scale_rotate_mirror(client, &config))
        }
    }
}

extension Comparable {
    func clamp(minValue: Self, maxValue: Self) -> Self {
        min(max(minValue, self), maxValue)
    }
}
