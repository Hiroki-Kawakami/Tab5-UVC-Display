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
        static func reset(pin: gpio_num_t) throws(IDF.Error) {
            try IDF.Error.check(gpio_reset_pin(pin))
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

        init(gpio_num: gpio_num_t, timer: Timer) throws(IDF.Error) {
            channel = ledc_channel_t(Self.channelPool.take())
            self.timer = timer
            var config = ledc_channel_config_t(
                gpio_num: gpio_num.rawValue,
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
        private static var i2cPool = IDF.ResourcePool(max: I2C_NUM_MAX.rawValue)
        let portNumber: i2c_port_num_t
        let handle: i2c_master_bus_handle_t
        init(num: UInt32? = nil, scl: gpio_num_t, sda: gpio_num_t) throws(IDF.Error) {
            portNumber = i2c_port_num_t(Self.i2cPool.take(num))
            var config = i2c_master_bus_config_t(
                i2c_port: portNumber,
                sda_io_num: sda,
                scl_io_num: scl,
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
