class M5StackTab5 {

    static func begin() throws(IDF.Error) -> M5StackTab5 {
        let i2c = try IDF.I2C(num: 0, scl: GPIO_NUM_32, sda: GPIO_NUM_31)
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
        let display = try Display(
            backlightGpio: GPIO_NUM_22,
            mipiDsiPhyPowerLdo: (channel: 3, voltageMv: 2500),
            numDataLanes: 2,
            laneBitRateMbps: 730, // 720*1280 RGB24 60Hz
            width: 720,
            height: 1280
        )
        return M5StackTab5(i2c: i2c, pi4io: pi4io, display: display)
    }

    let i2c: IDF.I2C
    let pi4io: [PI4IO]
    let display: Display
    private init(i2c: IDF.I2C, pi4io: [PI4IO], display: Display) {
        self.i2c = i2c
        self.pi4io = pi4io
        self.display = display
    }

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
    }

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
            backlightGpio: gpio_num_t,
            mipiDsiPhyPowerLdo: (channel: Int32, voltageMv: Int32)?,
            numDataLanes: UInt8,
            laneBitRateMbps: UInt32,
            width: UInt32,
            height: UInt32,
        ) throws(IDF.Error) {
            // Setup Backlight
            ledcTimer = try IDF.LEDControl.makeTimer(dutyResolution: 12, freqHz: 5000)
            backlight = try IDF.LEDControl(gpio_num: backlightGpio, timer: ledcTimer)

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
}
