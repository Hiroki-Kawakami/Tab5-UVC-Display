extension IDF {
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
}
