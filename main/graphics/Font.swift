fileprivate let Log = Logger(tag: "Font")

class Font {

    private let partition: IDF.Partition?
    private let fontData: UnsafeRawPointer
    var fontInfo = stbtt_fontinfo()

    init?(from partition: IDF.Partition) {
        self.partition = partition
        guard let fontData = partition.mmap else {
            Log.error("Failed to map font partition")
            return nil
        }
        self.fontData = fontData.baseAddress!
        if stbtt_InitFont(&fontInfo, fontData.baseAddress!, stbtt_GetFontOffsetForIndex(fontData.baseAddress!, 0)) == 0 {
            Log.error("Failed to initialize font")
            return nil
        }
    }

    var fontSize: Int = 24 {
        didSet {
            if oldValue != fontSize {
                sizeInfoCache = nil
            }
        }
    }
    var proportional: Bool = true {
        didSet {
            if oldValue != proportional {
                sizeInfoCache = nil
            }
        }
    }

    private struct SizeInfo {
        var scale: Float
        var baseline: Int
        var charWidth: Int?

        init(font: UnsafePointer<stbtt_fontinfo>, fontSize: Float, proportional: Bool) {
            scale = stbtt_ScaleForPixelHeight(font, fontSize)
            var ascent: Int32 = 0, decent: Int32 = 0
            stbtt_GetFontVMetrics(font, &ascent, &decent, nil)
            self.baseline = Int(Float(ascent) * scale)
            if !proportional {
                let codePoint: Int32 = 0x57 // 'W'
                var charWidth: Int32 = 0
                stbtt_GetCodepointHMetrics(font, codePoint, &charWidth, nil)
                self.charWidth = Int(Float(charWidth) * scale)
            } else {
                charWidth = nil
            }
        }
    }
    private var sizeInfoCache: SizeInfo? = nil
    private var sizeInfo: SizeInfo {
        if let info = sizeInfoCache {
            return info
        }
        sizeInfoCache = SizeInfo(font: &fontInfo, fontSize: Float(fontSize), proportional: proportional)
        return sizeInfoCache!
    }
    func width(of codePoint: Unicode.Scalar, fontSize: Int? = nil) -> Int {
        if let fontSize = fontSize {
            self.fontSize = fontSize
        }
        if let charWidth = sizeInfo.charWidth {
            return charWidth
        } else {
            var charWidth: Int32 = 0
            stbtt_GetCodepointHMetrics(&fontInfo, Int32(codePoint.value), &charWidth, nil)
            return Int(Float(charWidth) * sizeInfo.scale)
        }
    }

    func width(of text: String, fontSize: Int? = nil) -> Int {
        if let fontSize = fontSize {
            self.fontSize = fontSize
        }
        if let charWidth = sizeInfo.charWidth {
            return text.count * charWidth
        } else {
            var width = 0
            for char in text.unicodeScalars {
                width += self.width(of: char)
            }
            return width
        }
    }

    func drawBitmap(_ string: String, maxWidth: Int? = nil, drawPixel: (Point, UInt8) -> Void) {
        var offsetX: Int = 0
        for char in string.unicodeScalars {
            var bitmapWidth: Int32 = 0, bitmapHeight: Int32 = 0
            var xoff: Int32 = 0, yoff: Int32 = 0
            if let bitmap = stbtt_GetCodepointBitmap(
                &fontInfo, 0, sizeInfo.scale, Int32(char.value),
                &bitmapWidth, &bitmapHeight, &xoff, &yoff
            ) {
                if let maxWidth = maxWidth, offsetX + Int(bitmapWidth) > maxWidth {
                    break
                }
                for y in 0..<bitmapHeight {
                    for x in 0..<bitmapWidth {
                        let point = Point(x: Int(x + xoff) + offsetX, y: sizeInfo.baseline + Int(yoff + y))
                        let pixel = bitmap[Int(y * bitmapWidth + x)]
                        drawPixel(point, pixel)
                    }
                }
            }
            offsetX += self.width(of: char)
        }
    }
}
