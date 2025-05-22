class IDF {

    struct Error: Swift.Error, CustomStringConvertible {
        let rawValue: esp_err_t
        init(_ rawValue: esp_err_t) {
            self.rawValue = rawValue
        }

        static func check(_ result: esp_err_t) throws(IDF.Error) {
            if result != ESP_OK {
                throw IDF.Error(result)
            }
        }

        var description: String {
            String(cString: esp_err_to_name(rawValue))
        }
    }

    struct ResourcePool {
        let max: UInt32
        var used: UInt32 = 0
        init(max: UInt32) {
            self.max = max
        }

        mutating func take(_ value: UInt32? = nil) -> UInt32 {
            if let value = value {
                if value >= max {
                    fatalError("Resource out of range")
                }
                if used & (1 << value) != 0 {
                    fatalError("Resource already taken")
                }
                used |= (1 << value)
                return value
            } else {
                for i in 0..<max {
                    if used & (1 << i) == 0 {
                        used |= (1 << i)
                        return i
                    }
                }
                fatalError("No more resources available")
            }
        }
    }
}

extension Comparable {
    func clamp(minValue: Self, maxValue: Self) -> Self {
        min(max(minValue, self), maxValue)
    }
}
