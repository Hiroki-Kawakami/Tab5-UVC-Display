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
            inputBuffer: UnsafeMutableBufferPointer<UInt16>,
            inputSize: Size,
            outputBuffer: UnsafeMutableBufferPointer<UInt16>,
            outputSize: Size,
        ) throws(IDF.Error) {
            let rotate =
                (inputSize.width > inputSize.height && outputSize.width < outputSize.height) ||
                (inputSize.width < inputSize.height && outputSize.width > outputSize.height)
            let scale = min(
                Float(rotate ? outputSize.height : outputSize.width) / Float(inputSize.width),
                Float(rotate ? outputSize.width : outputSize.height) / Float(inputSize.height)
            )
            let outputFitSize = Size(
                width: rotate ? Int(Float(inputSize.height) * scale) : Int(Float(inputSize.width) * scale),
                height: rotate ? Int(Float(inputSize.width) * scale) : Int(Float(inputSize.height) * scale)
            )

            var config = ppa_srm_oper_config_t()
            config.in.buffer = UnsafeRawPointer(inputBuffer.baseAddress)
            config.in.pic_w = UInt32(inputSize.width)
            config.in.pic_h = UInt32(inputSize.height)
            config.in.block_w = UInt32(inputSize.width)
            config.in.block_h = UInt32(inputSize.height)
            config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565
            config.out.buffer = UnsafeMutableRawPointer(outputBuffer.baseAddress)
            config.out.buffer_size = UInt32(outputBuffer.count * MemoryLayout<UInt16>.size)
            config.out.pic_w = UInt32(outputSize.width)
            config.out.pic_h = UInt32(outputSize.height)
            config.out.block_offset_x = UInt32(outputSize.width - outputFitSize.width) / 2
            config.out.block_offset_y = UInt32(outputSize.height - outputFitSize.height) / 2
            config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565
            config.rotation_angle = rotate ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0
            config.scale_x = scale
            config.scale_y = scale
            try IDF.Error.check(ppa_do_scale_rotate_mirror(client, &config))
        }
    }
}
