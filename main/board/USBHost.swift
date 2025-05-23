fileprivate let Log = Logger(tag: "USBHost")

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
                Log.info("All devices freed")
            }
        }
    }

    var deviceAddrList: [UInt8] {
        var addrList = [UInt8](repeating: 0, count: 16)
        var numDevices: Int32 = 0
        usb_host_device_addr_list_fill(Int32(addrList.count), &addrList, &numDevices)
        if numDevices != addrList.count {
            addrList.removeLast(addrList.count - Int(numDevices))
        }
        return addrList
    }

    // MARK: UVC Driver
    class UVC {
        private var detectedDevice: (addr: UInt8, streamIndex: UInt8, frameInfoNum: Int)?

        func install(
            taskStackSize: Int,
            taskPriority: UInt32,
            xCoreID: Int32 = tskNO_AFFINITY,
            createBackgroundTask: Bool = true
        ) throws(IDF.Error) {
            var config = uvc_host_driver_config_t(
                driver_task_stack_size: taskStackSize,
                driver_task_priority: taskPriority,
                xCoreID: xCoreID,
                create_background_task: createBackgroundTask,
                event_cb: { (event, user_ctx) in
                    let uvc = Unmanaged<UVC>.fromOpaque(user_ctx!).takeUnretainedValue()
                    uvc.detectedDevice = (
                        addr: event!.pointee.device_connected.dev_addr,
                        streamIndex: event!.pointee.device_connected.uvc_stream_index,
                        frameInfoNum: Int(event!.pointee.device_connected.frame_info_num)
                    )
                },
                user_ctx: Unmanaged.passRetained(self).toOpaque()
            )
            try IDF.Error.check(uvc_host_install(&config))
            Log.info("UAC class driver installed")
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
                    if $0!.pointee.type == UVC_HOST_DEVICE_DISCONNECTED {
                        uvc_host_stream_close(uvc.stream)
                        let _ = Unmanaged<UVC>.fromOpaque($1!).takeRetainedValue()
                    }
                },
                frame_cb: {
                    let uvc = Unmanaged<UVC>.fromOpaque($1!).takeUnretainedValue()
                    if let frameCallback = uvc.frameCallback {
                        return frameCallback($0!)
                    }
                    Log.warn("Frame callback not registered")
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
                    frame_size: 2048 * 1024,
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
                Log.error("Transfer error")
            case UVC_HOST_DEVICE_DISCONNECTED:
                Log.warn("Device disconnected")
                disconnectCallback?()
            case UVC_HOST_FRAME_BUFFER_OVERFLOW:
                Log.warn("Frame buffer overflow")
            case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
                Log.warn("Frame buffer underflow")
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

        var frameInfoList: [uvc_host_frame_info_t]? {
            get throws(IDF.Error) {
                guard let device = detectedDevice else {
                    return nil
                }
                var list: [uvc_host_frame_info_t] = .init(repeating: uvc_host_frame_info_t(), count: device.frameInfoNum)
                var listSize = device.frameInfoNum
                let ptr = list.withUnsafeMutableBufferPointer { $0.baseAddress! }
                try IDF.Error.check(uvc_host_get_frame_list(device.addr, device.streamIndex, OpaquePointer(ptr), &listSize))
                return list
            }
        }
    }

    // MARK: UAC Driver
    class UAC {
        func install(
            taskStackSize: Int,
            taskPriority: UInt32,
            xCoreID: Int32 = tskNO_AFFINITY,
            createBackgroundTask: Bool = true,
        ) throws(IDF.Error) {
            var config = uac_host_driver_config_t(
                create_background_task: createBackgroundTask,
                task_priority: Int(taskPriority),
                stack_size: taskStackSize,
                core_id: xCoreID,
                callback: { (addr, ifaceNum, event, arg) in
                    let uac = Unmanaged<UAC>.fromOpaque(arg!).takeUnretainedValue()
                    uac.callback(addr: addr, ifaceNum: ifaceNum, event: event)
                },
                callback_arg: Unmanaged.passRetained(self).toOpaque()
            )
            try IDF.Error.check(uac_host_install(&config))
            Log.info("UAC class driver installed")
        }

        enum Direction {
            case rx
            case tx
        }

        var detected: [(addr: UInt8, ifaceNum: UInt8, direction: Direction)] = []

        private func callback(addr: UInt8, ifaceNum: UInt8, event: uac_host_driver_event_t) {
            detected.append((addr: addr, ifaceNum: ifaceNum, direction: event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED ? .rx : .tx))
        }

        class Device {
            var handle: uac_host_device_handle_t!
            private var rxDoneCallback: ((Device) -> Void)?
            init(
                addr: UInt8,
                ifaceNum: UInt8,
                bufferSize: UInt32,
                bufferThreshold: UInt32
            ) throws(IDF.Error) {
                var config = uac_host_device_config_t(
                    addr: addr,
                    iface_num: ifaceNum,
                    buffer_size: bufferSize,
                    buffer_threshold: bufferThreshold,
                    callback: { (handle, event, arg) in
                        let device = Unmanaged<Device>.fromOpaque(arg!).takeUnretainedValue()
                        switch event {
                        case UAC_HOST_DEVICE_EVENT_RX_DONE:
                            device.rxDoneCallback?(device)
                        case UAC_HOST_DEVICE_EVENT_TX_DONE:
                            Log.info("TX done")
                        case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                            Log.error("Transfer error")
                        case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                            Log.warn("UAC Device disconnected")
                            uac_host_device_close(handle)
                            let _ = Unmanaged<Device>.fromOpaque(arg!).takeRetainedValue()
                        default:
                            Log.warn("Unknown event: \(event.rawValue)")
                        }
                    },
                    callback_arg: Unmanaged.passRetained(self).toOpaque()
                )
                try IDF.Error.check(uac_host_device_open(&config, &handle))
            }
            deinit {
                Log.info("UAC device deinit")
            }

            func printDeviceParam() throws(IDF.Error) {
                try IDF.Error.check(uac_host_printf_device_param(handle))
            }

            var deviceInfo: uac_host_dev_info_t {
                get throws(IDF.Error) {
                    var info = uac_host_dev_info_t()
                    try IDF.Error.check(uac_host_get_device_info(handle, &info))
                    return info
                }
            }

            func start(
                sampleRate: Int,
                channels: Int,
                bitWidth: Int,
            ) throws(IDF.Error) {
                var config = uac_host_stream_config_t(
                    channels: UInt8(channels),
                    bit_resolution: UInt8(bitWidth),
                    sample_freq: UInt32(sampleRate),
                    flags: 0
                )
                try IDF.Error.check(uac_host_device_start(handle, &config))
            }

            func onRxDone(_ callback: @escaping (Device) -> Void) {
                self.rxDoneCallback = callback
            }

            func read(
                buffer: UnsafeMutableRawBufferPointer,
                timeout: UInt32 = portMAX_DELAY
            ) throws(IDF.Error) -> UInt32 {
                var bytesRead: UInt32 = 0
                try IDF.Error.check(uac_host_device_read(
                    handle,
                    buffer.assumingMemoryBound(to: UInt8.self).baseAddress,
                    UInt32(buffer.count),
                    &bytesRead,
                    timeout
                ))
                return bytesRead
            }
        }

        func open(
            addr: UInt8,
            ifaceNum: UInt8,
            bufferSize: UInt32,
            bufferThreshold: UInt32
        ) throws(IDF.Error) -> Device {
            return try Device(
                addr: addr,
                ifaceNum: ifaceNum,
                bufferSize: bufferSize,
                bufferThreshold: bufferThreshold
            )
        }

        func open(
            direction: Direction,
            bufferSize: UInt32,
            bufferThreshold: UInt32
        ) throws(IDF.Error) -> Device? {
            guard let device = detected.first(where: { $0.direction == direction }) else {
                return nil
            }
            return try open(
                addr: device.addr,
                ifaceNum: device.ifaceNum,
                bufferSize: bufferSize,
                bufferThreshold: bufferThreshold
            )
        }
    }

    // MSC Driver
    class MSC {
        func install(
            taskStackSize: Int,
            taskPriority: UInt32,
            xCoreID: Int32 = tskNO_AFFINITY,
            createBackgroundTask: Bool = true,
        ) throws(IDF.Error) {
            var config = msc_host_driver_config_t(
                create_backround_task: createBackgroundTask,
                task_priority: Int(taskPriority),
                stack_size: taskStackSize,
                core_id: xCoreID,
                callback: { (event, arg) in
                    let msc = Unmanaged<MSC>.fromOpaque(arg!).takeUnretainedValue()
                    msc.callback(event: event!)
                },
                callback_arg: Unmanaged.passRetained(self).toOpaque()
            )
            try IDF.Error.check(msc_host_install(&config))
            Log.info("MSC class driver installed")
        }

        func callback(event: UnsafePointer<msc_host_event_t>) {
            switch event.pointee.event {
            case MSC_DEVICE_CONNECTED:
                Log.info("MSC device connected, addr: \(event.pointee.device.address)")
                self.addr = event.pointee.device.address
            case MSC_DEVICE_DISCONNECTED:
                Log.info("MSC device disconnected")
                self.addr = nil
            default:
                abort()
            }
        }

        var addr: UInt8?
        var device: msc_host_device_handle_t?
        var vfsHandle: msc_host_vfs_handle_t?

        func mount(path: String, maxFiles: Int32) throws(IDF.Error) {
            guard let addr = addr else {
                throw IDF.Error(ESP_ERR_NOT_FOUND)
            }
            var mountConfig = esp_vfs_fat_mount_config_t(
                format_if_mount_failed: false,
                max_files: maxFiles,
                allocation_unit_size: 1024,
                disk_status_check_enable: false,
                use_one_fat: false
            )

            try IDF.Error.check(msc_host_install_device(addr, &device))
            try IDF.Error.check(path.utf8CString.withUnsafeBufferPointer {
                msc_host_vfs_register(device, $0.baseAddress!, &mountConfig, &vfsHandle)
            })
        }
    }
}
