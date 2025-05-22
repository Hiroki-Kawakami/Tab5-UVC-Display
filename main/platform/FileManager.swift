class FileManager {
    static let `default` = FileManager()

    func contentsOfDirectory(atPath path: String) -> [String]? {
        guard let dir = path.utf8CString.withUnsafeBufferPointer({ opendir($0.baseAddress) }) else {
            return nil
        }
        defer { closedir(dir) }

        var entries: [String] = []
        var entry = readdir(dir)
        while entry != nil {
            let name = entry!.pointee.dName
            // if name != "." && name != ".." {
            //     entries.append(name)
            // }
            if name.starts(with: "IMAGE") {
                entries.append(name)
            }
            entry = readdir(dir)
        }
        return entries
    }
}

fileprivate extension dirent {
    var dName: String {
        var cstr = d_name
        return withUnsafePointer(to: &cstr) {
            $0.withMemoryRebound(to: UInt8.self, capacity: MemoryLayout.size(ofValue: d_name)) {
                String(cString: $0)
            }
        }
    }
}
