fileprivate let LOG = Logger(tag: "USBHost")

class USBHost {

    func install() throws(IDF.Error) {
        var config = usb_host_config_t()
        config.intr_flags = ESP_INTR_FLAG_LEVEL1
        try IDF.Error.check(usb_host_install(&config))
        Task(name: "USBHost", priority: 10) { _ in self.task() }
    }

    private func task() {
        while true {
            var eventFlags: UInt32 = 0
            usb_host_lib_handle_events(portMAX_DELAY, &eventFlags)
            if (eventFlags & UInt32(USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) != 0 {
                usb_host_device_free_all()
            }
            if (eventFlags & UInt32(USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) != 0 {
                LOG.info("All devices freed")
            }
        }
    }

    // UVC Driver
    class UVC {
        func install(
            taskStackSize: Int,
            taskPriority: UInt32,
            xCoreID: Int32 = tskNO_AFFINITY,
            createBackgroundTask: Bool = true,
        ) throws(IDF.Error) {
            var config = uvc_host_driver_config_t(
                driver_task_stack_size: taskStackSize,
                driver_task_priority: taskPriority,
                xCoreID: xCoreID,
                create_background_task: createBackgroundTask,
                event_cb: nil,
                user_ctx: nil
            )
            try IDF.Error.check(uvc_host_install(&config))
        }

        enum StreamFormat {
            case mjpeg
            case yuy2
            case h264
            case h265

            init?(_ value: uvc_host_stream_format) {
                switch value {
                case UVC_VS_FORMAT_MJPEG: self = .mjpeg
                case UVC_VS_FORMAT_YUY2 : self = .yuy2
                case UVC_VS_FORMAT_H264 : self = .h264
                case UVC_VS_FORMAT_H265 : self = .h265
                default: return nil
                }
            }

            var value: uvc_host_stream_format {
                switch self {
                case .mjpeg: return UVC_VS_FORMAT_MJPEG
                case .yuy2 : return UVC_VS_FORMAT_YUY2
                case .h264 : return UVC_VS_FORMAT_H264
                case .h265 : return UVC_VS_FORMAT_H265
                }
            }
        }

        private var stream: uvc_host_stream_hdl_t?
        private var frameCallback: ((UnsafePointer<uvc_host_frame_t>) -> Bool)?
        private var disconnectCallback: (() -> Void)?

        func open(
            resolution: (width: UInt32, height: UInt32),
            frameRate: Float,
            pixelFormat: StreamFormat,
            numberOfFrameBuffers: Int32 = 2,
            timeout: UInt32 = 1000
        ) throws(IDF.Error) {
            var config = uvc_host_stream_config_t(
                event_cb: {
                    let uvc = Unmanaged<UVC>.fromOpaque($1!).takeUnretainedValue()
                    uvc.streamCallback(event: $0!)
                },
                frame_cb: {
                    let uvc = Unmanaged<UVC>.fromOpaque($1!).takeUnretainedValue()
                    if let frameCallback = uvc.frameCallback {
                        return frameCallback($0!)
                    }
                    LOG.warn("Frame callback not registered")
                    return false
                },
                user_ctx: Unmanaged.passRetained(self).toOpaque(),
                usb: uvc_host_stream_config_t.__Unnamed_struct_usb(
                    dev_addr: 0,
                    vid: 0,
                    pid: 0,
                    uvc_stream_index: 0
                ),
                vs_format: uvc_host_stream_format_t(
                    h_res: resolution.width,
                    v_res: resolution.height,
                    fps: frameRate,
                    format: pixelFormat.value
                ),
                advanced: uvc_host_stream_config_t.__Unnamed_struct_advanced(
                    number_of_frame_buffers: numberOfFrameBuffers,
                    frame_size: 256 * 1024,
                    frame_heap_caps: UInt32(MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED),
                    number_of_urbs: 3,
                    urb_size: 4 * 1024
                )
            )
            try IDF.Error.check(uvc_host_stream_open(&config, Int32(Task.ticks(5000)), &stream))
        }

        private func streamCallback(event: UnsafePointer<uvc_host_stream_event_data_t>) {
            switch event.pointee.type {
            case UVC_HOST_TRANSFER_ERROR:
                LOG.error("Transfer error")
            case UVC_HOST_DEVICE_DISCONNECTED:
                LOG.warn("Device disconnected")
                disconnectCallback?()
            case UVC_HOST_FRAME_BUFFER_OVERFLOW:
                LOG.warn("Frame buffer overflow")
            case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
                LOG.warn("Frame buffer underflow")
            default:
                fatalError("Unknown event type: \(event.pointee.type)")
                break
            }
        }

        func start() throws(IDF.Error) {
            try IDF.Error.check(uvc_host_stream_start(stream))
        }

        func onFrame(_ callback: @escaping (UnsafePointer<uvc_host_frame_t>) -> Bool) {
            self.frameCallback = callback
        }
        func returnFrame(_ frame: UnsafePointer<uvc_host_frame_t>) {
            uvc_host_frame_return(stream, UnsafeMutablePointer(mutating: frame))
        }

        func onDisconnect(_ callback: @escaping () -> Void) {
            self.disconnectCallback = callback
        }
    }
}
