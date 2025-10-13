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
            if !name.starts(with: ".") {
                entries.append(name)
            }
            entry = readdir(dir)
        }
        return entries
    }

    func isDirectory(atPath path: String) -> Bool {
        var statbuf = stat()
        guard path.utf8CString.withUnsafeBufferPointer({ stat($0.baseAddress, &statbuf) }) == 0 else {
            return false
        }
        return (statbuf.st_mode & UInt32(S_IFMT)) == S_IFDIR
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
