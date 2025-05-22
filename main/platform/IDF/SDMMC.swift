extension IDF {
    class SDMMC {
        typealias HostConfig = sdmmc_host_t
        typealias SlotConfig = sdmmc_slot_config_t

        let card: UnsafeMutablePointer<sdmmc_card_t>

        static func mount(
            path: String,
            host: HostConfig,
            slot: SlotConfig,
            formatIfMountFailed: Bool = false,
            maxFiles: Int32,
            allocationUnitSize: Int = 16 * 1024,
            diskStatusCheckEnable: Bool = false,
            useOneFat: Bool = false
        ) throws(IDF.Error) -> SDMMC {
            var card: UnsafeMutablePointer<sdmmc_card_t>?
            var host = host
            var slot = slot
            var config = esp_vfs_fat_sdmmc_mount_config_t(
                format_if_mount_failed: formatIfMountFailed,
                max_files: maxFiles,
                allocation_unit_size: allocationUnitSize,
                disk_status_check_enable: diskStatusCheckEnable,
                use_one_fat: useOneFat
            )
            let err = path.utf8CString.withUnsafeBufferPointer {
                esp_vfs_fat_sdmmc_mount($0.baseAddress!, &host, &slot, &config, &card)
            }
            try IDF.Error.check(err)
            return SDMMC(card: card!)
        }
        private init(card: UnsafeMutablePointer<sdmmc_card_t>) {
            self.card = card
        }

        func printInfo() {
            sdmmc_card_print_info(_STDOUT(), card)
        }
    }
}

extension IDF.SDMMC.HostConfig {
    static var `default`: IDF.SDMMC.HostConfig {
        var config = sdmmc_host_t()
        _SDMMC_HOST_DEFAULT(&config)
        return config
    }
}
extension IDF.SDMMC.SlotConfig {
    static func `default`(
        busWidth: UInt8? = nil,
        clk: IDF.GPIO.Pin? = nil,
        cmd: IDF.GPIO.Pin? = nil,
        data0: IDF.GPIO.Pin? = nil,
        data1: IDF.GPIO.Pin? = nil,
        data2: IDF.GPIO.Pin? = nil,
        data3: IDF.GPIO.Pin? = nil,
        data4: IDF.GPIO.Pin? = nil,
        data5: IDF.GPIO.Pin? = nil,
        data6: IDF.GPIO.Pin? = nil,
        data7: IDF.GPIO.Pin? = nil,
        cardDetect: IDF.GPIO.Pin? = nil,
        writeProtect: IDF.GPIO.Pin? = nil
    ) -> IDF.SDMMC.SlotConfig {
        var config = sdmmc_slot_config_t()
        _SDMMC_SLOT_CONFIG_DEFAULT(&config)
        config.width = busWidth ?? config.width
        config.clk = clk?.value ?? config.clk
        config.cmd = cmd?.value ?? config.cmd
        config.d0 = data0?.value ?? config.d0
        config.d1 = data1?.value ?? config.d1
        config.d2 = data2?.value ?? config.d2
        config.d3 = data3?.value ?? config.d3
        config.d4 = data4?.value ?? config.d4
        config.d5 = data5?.value ?? config.d5
        config.d6 = data6?.value ?? config.d6
        config.d7 = data7?.value ?? config.d7
        config.cd = cardDetect?.value ?? config.cd
        config.wp = writeProtect?.value ?? config.wp
        return config
    }
}
