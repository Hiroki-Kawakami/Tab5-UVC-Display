#pragma once
#include <tuple>
#include <optional>
#include <cstdint>
#include <cstddef>
#include "driver/ppa.h"
#include "driver/jpeg_decode.h"

namespace pf_port {

enum class Error {
    Ok,
    Fail,
    InvalidArgument,
};

enum class PixelFormat {
    RGB565,
    RGB888,
};

void init(int fb_num, PixelFormat pixel_format);
PixelFormat display_pixel_format();
inline size_t bytes_per_pixel(PixelFormat pf) {
    switch (pf) {
        case PixelFormat::RGB565: return 2;
        case PixelFormat::RGB888: return 3;
    }
    return 0;
}
void display_set_brightness(int value);
void *display_get_frame_buffer(int fb_index);
void display_flush(int fb_index);
std::optional<std::tuple<int, int>> touch_get_point();

void *psram_malloc(size_t size);
void *psram_malloc_dma(size_t size);

class SRMClient {
public:
    SRMClient();
    ~SRMClient();
    void setInputBlock(
        uint32_t pic_w,
        uint32_t pic_h,
        uint32_t block_w,
        uint32_t block_h,
        uint32_t block_offset_x,
        uint32_t block_offset_y,
        PixelFormat pixel_format
    );
    void setOutputBlock(
        uint32_t pic_w,
        uint32_t pic_h,
        uint32_t block_offset_x,
        uint32_t block_offset_y,
        PixelFormat pixel_format
    );
    void setRotation(int rotation);
    void setScale(float scale_x, float scale_y);
    Error do_scale_rotate_mirror(const void *input, void *output);

private:
    ppa_client_handle_t ppa_client_{nullptr};
    ppa_srm_oper_config_t oper_config_{};
};

class JpegDecoder {
public:
    struct PictureInfo {
        uint32_t width;
        uint32_t height;
    };

    JpegDecoder();
    ~JpegDecoder();

    JpegDecoder(const JpegDecoder &) = delete;
    JpegDecoder &operator=(const JpegDecoder &) = delete;

    static std::optional<PictureInfo> getInfo(const void *stream, size_t stream_size);

    void setOutputFormat(PixelFormat pixel_format);
    Error decode(const void *stream, size_t stream_size,
                 void *output, size_t output_size,
                 size_t *out_size);

private:
    jpeg_decoder_handle_t decoder_{nullptr};
    jpeg_decode_cfg_t cfg_{};
};

}
