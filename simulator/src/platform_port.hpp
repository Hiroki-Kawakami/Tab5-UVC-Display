#pragma once
#include <tuple>
#include <optional>
#include <cstdint>

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
void display_set_brightness(int value);
void *display_get_frame_buffer(int fb_index);
void display_flush(int fb_index);
std::optional<std::tuple<int, int>> touch_get_point();

void *psram_malloc(size_t size);
void *psram_malloc_dma(size_t size);

class SRMClient {
public:
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
    struct InConfig {
        uint32_t pic_w = 0, pic_h = 0;
        uint32_t block_w = 0, block_h = 0;
        uint32_t block_offset_x = 0, block_offset_y = 0;
        PixelFormat fmt = PixelFormat::RGB565;
    } in_;
    struct OutConfig {
        uint32_t pic_w = 0, pic_h = 0;
        uint32_t block_offset_x = 0, block_offset_y = 0;
        PixelFormat fmt = PixelFormat::RGB565;
    } out_;
    int rotation_ = 0;
    float scale_x_ = 1.0f, scale_y_ = 1.0f;
};

}
