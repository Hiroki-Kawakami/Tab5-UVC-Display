extension IDF {
    class JPEG {

        enum DecoderOutFormat {
            case rgb888
            case rgb565
            case gray
            case yuv444
            case yuv422
            case yuv420

            var value: jpeg_dec_output_format_t {
                switch self {
                case .rgb888: return JPEG_DECODE_OUT_FORMAT_RGB888
                case .rgb565: return JPEG_DECODE_OUT_FORMAT_RGB565
                case .gray  : return JPEG_DECODE_OUT_FORMAT_GRAY
                case .yuv444: return JPEG_DECODE_OUT_FORMAT_YUV444
                case .yuv422: return JPEG_DECODE_OUT_FORMAT_YUV422
                case .yuv420: return JPEG_DECODE_OUT_FORMAT_YUV420
                }
            }
        }

        enum DecoderRGBConversion {
            case bt601
            case bt709

            var value: jpeg_yuv_rgb_conv_std_t {
                switch self {
                case .bt601: return JPEG_YUV_RGB_CONV_STD_BT601
                case .bt709: return JPEG_YUV_RGB_CONV_STD_BT709
                }
            }
        }

        enum DecoderRGBElementOrder {
            case rgb
            case bgr

            var value: jpeg_dec_rgb_element_order_t {
                switch self {
                case .rgb: return JPEG_DEC_RGB_ELEMENT_ORDER_RGB
                case .bgr: return JPEG_DEC_RGB_ELEMENT_ORDER_BGR
                }
            }
        }

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

        static func createDecoderRgb565(
            rgbElementOrder: DecoderRGBElementOrder,
            rgbConversion: DecoderRGBConversion = .bt601,
            intrPriority: Int32 = 0,
            timeout: Int32 = 100,
        ) throws(IDF.Error) -> Decoder<UInt16> {
            let decodeConfig = jpeg_decode_cfg_t(
                output_format: DecoderOutFormat.rgb565.value,
                rgb_order: rgbElementOrder.value,
                conv_std: rgbConversion.value
            )
            return try Decoder<UInt16>(intrPriority: intrPriority, timeout: timeout, decodeConfig: decodeConfig)
        }

        class Decoder<E> {
            private let engine: jpeg_decoder_handle_t
            private var decodeConfig: jpeg_decode_cfg_t

            fileprivate init(intrPriority: Int32, timeout: Int32, decodeConfig: jpeg_decode_cfg_t) throws(IDF.Error) {
                var engine: jpeg_decoder_handle_t?
                var config = jpeg_decode_engine_cfg_t(
                    intr_priority: intrPriority,
                    timeout_ms: timeout
                )
                try IDF.Error.check(jpeg_new_decoder_engine(&config, &engine))
                self.engine = engine!
                self.decodeConfig = decodeConfig
            }

            deinit {
                jpeg_del_decoder_engine(engine)
            }

            static func allocateOutputBuffer(capacity: Int) -> UnsafeMutableBufferPointer<E>? {
                let size = MemoryLayout<E>.size * capacity
                var allocatedSize = 0
                var allocConfig = jpeg_decode_memory_alloc_cfg_t(buffer_direction: JPEG_DEC_ALLOC_OUTPUT_BUFFER)
                let pointer = jpeg_alloc_decoder_mem(size, &allocConfig, &allocatedSize)
                if pointer == nil {
                    return nil
                }
                return UnsafeMutableBufferPointer<E>(
                    start: pointer?.bindMemory(to: E.self, capacity: allocatedSize / MemoryLayout<E>.size),
                    count: allocatedSize / MemoryLayout<E>.size
                )
            }

            func decode(inputBuffer: UnsafeRawBufferPointer, outputBuffer: UnsafeMutableBufferPointer<E>) throws(IDF.Error) -> UInt32 {
                var decodeSize: UInt32 = 0
                try IDF.Error.check(
                    jpeg_decoder_process(
                        engine, &decodeConfig,
                        inputBuffer.baseAddress, UInt32(inputBuffer.count),
                        outputBuffer.baseAddress, UInt32(MemoryLayout<E>.size * outputBuffer.count),
                        &decodeSize
                    )
                )
                return decodeSize
            }
        }
    }
}
