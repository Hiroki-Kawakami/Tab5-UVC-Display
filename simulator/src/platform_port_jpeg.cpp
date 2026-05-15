#include "platform_port.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace pf_port {

namespace {

struct JpegErrMgr {
    jpeg_error_mgr pub;
    jmp_buf jmp;
    char msg[JMSG_LENGTH_MAX];
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegErrMgr *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->msg);
    longjmp(err->jmp, 1);
}

static void jpeg_emit_message(j_common_ptr, int) {
    // suppress warnings
}

static void rgb888_to_rgb565(const uint8_t *src, uint8_t *dst, size_t pixels) {
    for (size_t i = 0; i < pixels; i++) {
        uint8_t r = src[i * 3 + 0];
        uint8_t g = src[i * 3 + 1];
        uint8_t b = src[i * 3 + 2];
        uint16_t v = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
        dst[i * 2 + 0] = v & 0xFF;
        dst[i * 2 + 1] = (v >> 8) & 0xFF;
    }
}

} // namespace

JpegDecoder::JpegDecoder() = default;
JpegDecoder::~JpegDecoder() = default;

std::optional<JpegDecoder::PictureInfo> JpegDecoder::getInfo(const void *stream, size_t stream_size) {
    if (!stream || stream_size == 0) return std::nullopt;

    jpeg_decompress_struct cinfo;
    JpegErrMgr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_error_exit;
    err.pub.emit_message = jpeg_emit_message;
    if (setjmp(err.jmp)) {
        fprintf(stderr, "JPEG getInfo error: %s\n", err.msg);
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, static_cast<const unsigned char *>(stream),
                 static_cast<unsigned long>(stream_size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }
    PictureInfo info{cinfo.image_width, cinfo.image_height};
    jpeg_destroy_decompress(&cinfo);
    return info;
}

void JpegDecoder::setOutputFormat(PixelFormat pixel_format) {
    out_fmt_ = pixel_format;
}

Error JpegDecoder::decode(const void *stream, size_t stream_size,
                          void *output, size_t output_size,
                          size_t *out_size) {
    if (!stream || stream_size == 0 || !output) return Error::InvalidArgument;

    jpeg_decompress_struct cinfo;
    JpegErrMgr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_error_exit;
    err.pub.emit_message = jpeg_emit_message;
    if (setjmp(err.jmp)) {
        fprintf(stderr, "JPEG decode error: %s\n", err.msg);
        jpeg_destroy_decompress(&cinfo);
        return Error::Fail;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, static_cast<const unsigned char *>(stream),
                 static_cast<unsigned long>(stream_size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return Error::Fail;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const size_t width = cinfo.output_width;
    const size_t height = cinfo.output_height;
    const size_t row_rgb888 = width * 3;
    const size_t bpp_out = (out_fmt_ == PixelFormat::RGB565) ? 2 : 3;
    const size_t row_out = width * bpp_out;
    const size_t required = row_out * height;

    if (output_size < required) {
        fprintf(stderr, "JPEG decode: output buffer too small (%zu < %zu)\n",
                output_size, required);
        jpeg_destroy_decompress(&cinfo);
        return Error::InvalidArgument;
    }

    std::vector<uint8_t> rowbuf;
    if (out_fmt_ == PixelFormat::RGB565) {
        rowbuf.resize(row_rgb888);
    }

    uint8_t *dst = static_cast<uint8_t *>(output);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *target = (out_fmt_ == PixelFormat::RGB565)
                              ? rowbuf.data()
                              : dst + cinfo.output_scanline * row_out;
        JSAMPROW rows[1] = { target };
        jpeg_read_scanlines(&cinfo, rows, 1);
        if (out_fmt_ == PixelFormat::RGB565) {
            rgb888_to_rgb565(rowbuf.data(),
                             dst + (cinfo.output_scanline - 1) * row_out,
                             width);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if (out_size) *out_size = required;
    return Error::Ok;
}

}
