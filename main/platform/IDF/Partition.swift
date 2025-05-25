extension IDF {
    class Partition {
        private let partition: UnsafePointer<esp_partition_t>

        init?(type: UInt8, subtype: UInt8) {
            let type = esp_partition_type_t(UInt32(type))
            let subtype = esp_partition_subtype_t(UInt32(subtype))
            guard let partition = esp_partition_find_first(type, subtype, nil) else {
                return nil
            }
            self.partition = partition
        }

        deinit {
            if let mmapState = mmapState {
                esp_partition_munmap(mmapState.handle)
            }
        }

        private var mmapState: (ptr: UnsafeRawBufferPointer, handle: esp_partition_mmap_handle_t)? = nil
        var mmap: UnsafeRawBufferPointer? {
            if let mmapState = mmapState {
                return mmapState.ptr
            }
            var ptr: UnsafeRawPointer? = nil
            var handle: esp_partition_mmap_handle_t = 0
            let err = esp_partition_mmap(
                partition, 0, Int(partition.pointee.size),
                ESP_PARTITION_MMAP_DATA, &ptr, &handle
            )
            if err != ESP_OK || ptr == nil {
                return nil
            }
            mmapState = (ptr: UnsafeRawBufferPointer(start: ptr!, count: Int(partition.pointee.size)), handle: handle)
            return mmapState!.ptr
        }
    }
}
