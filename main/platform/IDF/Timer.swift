extension IDF {
    class Timer {

        let gptimer: gptimer_handle_t?

        init(resolutionHz: UInt32 = 1 * 1000 * 1000) throws(IDF.Error) {
            var config = gptimer_config_t()
            config.clk_src = GPTIMER_CLK_SRC_DEFAULT
            config.direction = GPTIMER_COUNT_UP
            config.resolution_hz = resolutionHz

            var gptimer: gptimer_handle_t?
            try IDF.Error.check(gptimer_new_timer(&config, &gptimer))
            try IDF.Error.check(gptimer_enable(gptimer))
            try IDF.Error.check(gptimer_start(gptimer))
            self.gptimer = gptimer
        }

        deinit {
            gptimer_stop(gptimer)
            gptimer_disable(gptimer)
            gptimer_del_timer(gptimer)
        }

        var count: UInt64 {
            var count: UInt64 = 0
            gptimer_get_raw_count(gptimer, &count)
            return count
        }
        func duration(from: UInt64) -> UInt64 {
            return count - from
        }
    }
}
