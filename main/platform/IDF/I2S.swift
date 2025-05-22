extension IDF {
    class I2S {
        typealias Role = i2s_role_t

        enum Format {
            class STD {
                typealias ClockConfig = i2s_std_clk_config_t
                typealias SlotConfig = i2s_std_slot_config_t
                typealias GPIOConfig = i2s_std_gpio_config_t
            }
            case std(clock: STD.ClockConfig, slot: STD.SlotConfig, gpio: STD.GPIOConfig)

            var i2sConfig: i2s_std_config_t? {
                guard case let .std(clock, slot, gpio) = self else { return nil }
                return i2s_std_config_t(
                    clk_cfg: clock,
                    slot_cfg: slot,
                    gpio_cfg: gpio
                )
            }

            class TDM {
                typealias ClockConfig = i2s_tdm_clk_config_t
                typealias SlotConfig = i2s_tdm_slot_config_t
                typealias GPIOConfig = i2s_tdm_gpio_config_t
            }
            case tdm(clock: TDM.ClockConfig, slot: TDM.SlotConfig, gpio: TDM.GPIOConfig)

            var tdmConfig: i2s_tdm_config_t? {
                guard case let .tdm(clock, slot, gpio) = self else { return nil }
                return i2s_tdm_config_t(
                    clk_cfg: clock,
                    slot_cfg: slot,
                    gpio_cfg: gpio
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
            channelConfig.role = role
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
}

extension IDF.I2S.Role {
    static let master = I2S_ROLE_MASTER
    static let slave = I2S_ROLE_SLAVE
}
extension IDF.I2S.Format.STD.ClockConfig {
    static func `default`(sampleRate: UInt32) -> IDF.I2S.Format.STD.ClockConfig {
        var config = i2s_std_clk_config_t()
        _I2S_STD_CLK_DEFAULT_CONFIG(&config, sampleRate)
        return config
    }
    init(
        sampleRate: UInt32,
        clkSrc: i2s_clock_src_t,
        extClkFreq: UInt32,
        mclkMultiple: i2s_mclk_multiple_t,
    ) {
        self.init()
        self.sample_rate_hz = sampleRate
        self.clk_src = clkSrc
        self.ext_clk_freq_hz = extClkFreq
        self.mclk_multiple = mclkMultiple
    }
}
extension IDF.I2S.Format.STD.SlotConfig {
    static func philipsDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
    ) -> IDF.I2S.Format.STD.SlotConfig {
        var config = i2s_std_slot_config_t()
        _I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode)
        return config
    }
    static func pcmDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
    ) -> IDF.I2S.Format.STD.SlotConfig {
        var config = i2s_std_slot_config_t()
        _I2S_STD_PCM_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode)
        return config
    }
    static func msbDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
    ) -> IDF.I2S.Format.STD.SlotConfig {
        var config = i2s_std_slot_config_t()
        _I2S_STD_MSB_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode)
        return config
    }
    init(
        dataBitWidth: i2s_data_bit_width_t,
        slotBitWidth: i2s_slot_bit_width_t,
        slotMode: i2s_slot_mode_t,
        slotMask: i2s_std_slot_mask_t,
        wsWidth: UInt32,
        wsPol: Bool,
        bitShift: Bool,
        leftAlign: Bool,
        bigEndian: Bool,
        bitOrderLsb: Bool
    ) {
        self.init()
        self.data_bit_width = dataBitWidth
        self.slot_bit_width = slotBitWidth
        self.slot_mode = slotMode
        self.slot_mask = slotMask
        self.ws_width = wsWidth
        self.ws_pol = wsPol
        self.bit_shift = bitShift
        self.left_align = leftAlign
        self.big_endian = bigEndian
        self.bit_order_lsb = bitOrderLsb
    }
}
extension IDF.I2S.Format.STD.GPIOConfig {
    init(
        mclk: IDF.GPIO.Pin?,
        bclk: IDF.GPIO.Pin,
        ws: IDF.GPIO.Pin,
        dout: IDF.GPIO.Pin?,
        din: IDF.GPIO.Pin?
    ) {
        self.init()
        self.mclk = mclk?.value ?? GPIO_NUM_NC
        self.bclk = bclk.value
        self.ws = ws.value
        self.dout = dout?.value ?? GPIO_NUM_NC
        self.din = din?.value ?? GPIO_NUM_NC
    }
}
extension IDF.I2S.Format.TDM.ClockConfig {
    static func `default`(sampleRate: UInt32) -> IDF.I2S.Format.TDM.ClockConfig {
        var config = i2s_tdm_clk_config_t()
        _I2S_TDM_CLK_DEFAULT_CONFIG(&config, sampleRate)
        return config
    }
    init(
        sampleRate: UInt32,
        clkSrc: i2s_clock_src_t,
        extClkFreq: UInt32,
        mclkMultiple: i2s_mclk_multiple_t,
        bclkDiv: UInt32
    ) {
        self.init()
        self.sample_rate_hz = sampleRate
        self.clk_src = clkSrc
        self.ext_clk_freq_hz = extClkFreq
        self.mclk_multiple = mclkMultiple
        self.bclk_div = bclkDiv
    }
}
extension IDF.I2S.Format.TDM.SlotConfig {
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

        var mask: i2s_tdm_slot_mask_t {
            return i2s_tdm_slot_mask_t(rawValue: rawValue)
        }
    }

    static func philipsDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
        slotMask: SlotMask
    ) -> IDF.I2S.Format.TDM.SlotConfig {
        var config = i2s_tdm_slot_config_t()
        _I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode, slotMask.mask)
        return config
    }
    static func msbDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
        slotMask: SlotMask
    ) -> IDF.I2S.Format.TDM.SlotConfig {
        var config = i2s_tdm_slot_config_t()
        _I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode, slotMask.mask)
        return config
    }
    static func pcmShortDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
        slotMask: SlotMask
    ) -> IDF.I2S.Format.TDM.SlotConfig {
        var config = i2s_tdm_slot_config_t()
        _I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode, slotMask.mask)
        return config
    }
    static func pcmLongDefault(
        dataBitWidth: i2s_data_bit_width_t,
        slotMode: i2s_slot_mode_t,
        slotMask: SlotMask
    ) -> IDF.I2S.Format.TDM.SlotConfig {
        var config = i2s_tdm_slot_config_t()
        _I2S_TDM_PCM_LONG_SLOT_DEFAULT_CONFIG(&config, dataBitWidth, slotMode, slotMask.mask)
        return config
    }

    init(
        dataBitWidth: i2s_data_bit_width_t,
        slotBitWidth: i2s_slot_bit_width_t,
        slotMode: i2s_slot_mode_t,
        slotMask: SlotMask,
        wsWidth: UInt32,
        wsPol: Bool,
        bitShift: Bool,
        leftAlign: Bool,
        bigEndian: Bool,
        bitOrderLsb: Bool,
        skipMask: Bool,
        totalSlot: UInt32
    ) {
        self.init()
        self.data_bit_width = dataBitWidth
        self.slot_bit_width = slotBitWidth
        self.slot_mode = slotMode
        self.slot_mask = i2s_tdm_slot_mask_t(rawValue: slotMask.rawValue)
        self.ws_width = wsWidth
        self.ws_pol = wsPol
        self.bit_shift = bitShift
        self.left_align = leftAlign
        self.big_endian = bigEndian
        self.bit_order_lsb = bitOrderLsb
        self.skip_mask = skipMask
        self.total_slot = totalSlot
    }
}
extension IDF.I2S.Format.TDM.GPIOConfig {
    init(
        mclk: IDF.GPIO.Pin?,
        bclk: IDF.GPIO.Pin,
        ws: IDF.GPIO.Pin,
        dout: IDF.GPIO.Pin?,
        din: IDF.GPIO.Pin?
    ) {
        self.init()
        self.mclk = mclk?.value ?? GPIO_NUM_NC
        self.bclk = bclk.value
        self.ws = ws.value
        self.dout = dout?.value ?? GPIO_NUM_NC
        self.din = din?.value ?? GPIO_NUM_NC
    }
}
