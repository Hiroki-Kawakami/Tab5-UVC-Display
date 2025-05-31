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

        func fitScreen(
            input: (buffer: UnsafeRawBufferPointer, size: Size, colorMode: ppa_srm_color_mode_t),
            output: (buffer: UnsafeMutableRawBufferPointer, size: Size, colorMode: ppa_srm_color_mode_t),
        ) throws(IDF.Error) {
            let rotate =
                (input.size.width > input.size.height && output.size.width < output.size.height) ||
                (input.size.width < input.size.height && output.size.width > output.size.height)
            let scale = min(
                Float(rotate ? output.size.height : output.size.width) / Float(input.size.width),
                Float(rotate ? output.size.width : output.size.height) / Float(input.size.height)
            )
            let outputFitSize = Size(
                width: rotate ? Int(Float(input.size.height) * scale) : Int(Float(input.size.width) * scale),
                height: rotate ? Int(Float(input.size.width) * scale) : Int(Float(input.size.height) * scale)
            )

            var config = ppa_srm_oper_config_t()
            config.in.buffer = input.buffer.baseAddress
            config.in.pic_w = UInt32(input.size.width)
            config.in.pic_h = UInt32(input.size.height)
            config.in.block_w = UInt32(input.size.width)
            config.in.block_h = UInt32(input.size.height)
            config.in.srm_cm = input.colorMode
            config.out.buffer = output.buffer.baseAddress
            config.out.buffer_size = UInt32(output.buffer.count)
            config.out.pic_w = UInt32(output.size.width)
            config.out.pic_h = UInt32(output.size.height)
            config.out.block_offset_x = UInt32(output.size.width - outputFitSize.width) / 2
            config.out.block_offset_y = UInt32(output.size.height - outputFitSize.height) / 2
            config.out.srm_cm = output.colorMode
            config.rotation_angle = rotate ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0
            config.scale_x = scale
            config.scale_y = scale
            try IDF.Error.check(ppa_do_scale_rotate_mirror(client, &config))
        }
    }
}
