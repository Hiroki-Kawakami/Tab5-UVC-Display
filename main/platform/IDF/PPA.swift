extension IDF {
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

        func rotate90WithMargin(
            inputBuffer: UnsafeMutableBufferPointer<UInt16>,
            outputBuffer: UnsafeMutableBufferPointer<UInt16>,
            size: (width: UInt32, height: UInt32),
            margin: UInt32
        ) throws(IDF.Error) {
            var config = ppa_srm_oper_config_t()
            config.in.buffer = UnsafeRawPointer(inputBuffer.baseAddress)
            config.in.pic_w = size.width
            config.in.pic_h = size.height
            config.in.block_w = size.width - margin
            config.in.block_h = size.height
            config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565
            config.out.buffer = UnsafeMutableRawPointer(outputBuffer.baseAddress)
            config.out.buffer_size = UInt32(outputBuffer.count * MemoryLayout<UInt16>.size)
            config.out.pic_w = size.height
            config.out.pic_h = size.width
            config.out.block_offset_y = margin
            config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565
            config.rotation_angle = PPA_SRM_ROTATION_ANGLE_90
            config.scale_x = 1
            config.scale_y = 1
            config.mode = PPA_TRANS_MODE_BLOCKING
            try IDF.Error.check(ppa_do_scale_rotate_mirror(client, &config))
        }
    }
}
