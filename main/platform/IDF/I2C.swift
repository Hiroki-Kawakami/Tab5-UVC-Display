extension IDF {
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
}
