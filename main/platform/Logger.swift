struct Logger {
    let tag: StaticString

    func error(_ message: String) {
        esp_log_write_str(ESP_LOG_ERROR, tag.utf8Start, "\u{1b}[0;31mE (\(esp_log_timestamp())) \(tag): \(message)\u{1b}[0m\n")
    }

    func warn(_ message: String) {
        esp_log_write_str(ESP_LOG_WARN, tag.utf8Start, "\u{1b}[0;33mW (\(esp_log_timestamp())) \(tag): \(message)\u{1b}[0m\n")
    }

    func info(_ message: String) {
        esp_log_write_str(ESP_LOG_INFO, tag.utf8Start, "\u{1b}[0;32mI (\(esp_log_timestamp())) \(tag): \(message)\u{1b}[0m\n")
    }
}

extension Optional {
    func unwrap(errMsg: () -> String) -> Wrapped {
        if let value = self {
            return value
        }
        Logger(tag: "Optional").error(errMsg())
        abort()
    }
}
