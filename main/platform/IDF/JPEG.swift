extension IDF {
    class JPEG {
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

        class Decoder {
            typealias RGBElementOrder = jpeg_dec_rgb_element_order_t
            typealias RGBConversion = jpeg_yuv_rgb_conv_std_t
            enum OutputFormat {
                case rgb888(elementOrder: RGBElementOrder, conversion: RGBConversion)
                case rgb565(elementOrder: RGBElementOrder, conversion: RGBConversion)
                case gray
                case yuv444
                case yuv422
                case yuv420

                var config: jpeg_decode_cfg_t {
                    var config = jpeg_decode_cfg_t()
                    switch self {
                    case .rgb888(let elementOrder, let conversion):
                        config.output_format = JPEG_DECODE_OUT_FORMAT_RGB888
                        config.rgb_order = elementOrder
                        config.conv_std = conversion
                    case .rgb565(let elementOrder, let conversion):
                        config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565
                        config.rgb_order = elementOrder
                        config.conv_std = conversion
                    case .gray:
                        config.output_format = JPEG_DECODE_OUT_FORMAT_GRAY
                    case .yuv444:
                        config.output_format = JPEG_DECODE_OUT_FORMAT_YUV444
                    case .yuv422:
                        config.output_format = JPEG_DECODE_OUT_FORMAT_YUV422
                    case .yuv420:
                        config.output_format = JPEG_DECODE_OUT_FORMAT_YUV420
                    }
                    return config
                }
            }

            private let engine: jpeg_decoder_handle_t
            private var decodeConfig: jpeg_decode_cfg_t

            init(outputFormat: OutputFormat, intrPriority: Int32 = 0, timeout: Int32 = 100) throws(IDF.Error) {
                var engine: jpeg_decoder_handle_t?
                var config = jpeg_decode_engine_cfg_t(
                    intr_priority: intrPriority,
                    timeout_ms: timeout
                )
                try IDF.Error.check(jpeg_new_decoder_engine(&config, &engine))
                self.engine = engine!
                self.decodeConfig = outputFormat.config
            }

            deinit {
                jpeg_del_decoder_engine(engine)
            }

            static func allocateOutputBuffer(size: Int) -> UnsafeMutableRawBufferPointer? {
                var allocatedSize = 0
                var allocConfig = jpeg_decode_memory_alloc_cfg_t(buffer_direction: JPEG_DEC_ALLOC_OUTPUT_BUFFER)
                let pointer = jpeg_alloc_decoder_mem(size, &allocConfig, &allocatedSize)
                if pointer == nil {
                    return nil
                }
                return UnsafeMutableRawBufferPointer(
                    start: pointer!,
                    count: allocatedSize
                )
            }

            func decode(inputBuffer: UnsafeRawBufferPointer, outputBuffer: UnsafeMutableRawBufferPointer) throws(IDF.Error) -> UInt32 {
                var decodeSize: UInt32 = 0
                try IDF.Error.check(
                    jpeg_decoder_process(
                        engine, &decodeConfig,
                        inputBuffer.baseAddress, UInt32(inputBuffer.count),
                        outputBuffer.baseAddress, UInt32(outputBuffer.count),
                        &decodeSize
                    )
                )
                return decodeSize
            }
        }
    }
}

extension jpeg_yuv_rgb_conv_std_t {
    static let bt601 = JPEG_YUV_RGB_CONV_STD_BT601
    static let bt709 = JPEG_YUV_RGB_CONV_STD_BT709
}
extension jpeg_dec_rgb_element_order_t {
    static let rgb = JPEG_DEC_RGB_ELEMENT_ORDER_RGB
    static let bgr = JPEG_DEC_RGB_ELEMENT_ORDER_BGR
}
