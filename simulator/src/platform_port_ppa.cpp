#include "platform_port.hpp"
#include <algorithm>
#include <cstdio>
#include <pthread.h>

namespace pf_port {

namespace {

static int bpp(PixelFormat fmt) {
    return fmt == PixelFormat::RGB565 ? 2 : 3;
}

static void readPixel(const uint8_t *buf, uint32_t pic_w, uint32_t x, uint32_t y, PixelFormat fmt,
                      uint8_t &r, uint8_t &g, uint8_t &b) {
    const uint8_t *p = buf + (y * pic_w + x) * bpp(fmt);
    if (fmt == PixelFormat::RGB565) {
        uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        uint8_t r5 = (v >> 11) & 0x1F;
        uint8_t g6 = (v >> 5) & 0x3F;
        uint8_t b5 = v & 0x1F;
        r = (r5 << 3) | (r5 >> 2);
        g = (g6 << 2) | (g6 >> 4);
        b = (b5 << 3) | (b5 >> 2);
    } else {
        r = p[0]; g = p[1]; b = p[2];
    }
}

static void writePixel(uint8_t *buf, uint32_t pic_w, uint32_t x, uint32_t y, PixelFormat fmt,
                       uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *p = buf + (y * pic_w + x) * bpp(fmt);
    if (fmt == PixelFormat::RGB565) {
        uint16_t v = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
    } else {
        p[0] = r; p[1] = g; p[2] = b;
    }
}

static pthread_mutex_t srm_mutex;
static pthread_once_t srm_once = PTHREAD_ONCE_INIT;

static void init_srm_mutex() {
    pthread_mutex_init(&srm_mutex, nullptr);
}

static void sampleBilinear(const uint8_t *buf, uint32_t pic_w, uint32_t pic_h,
                           uint32_t off_x, uint32_t off_y,
                           uint32_t block_w, uint32_t block_h,
                           PixelFormat fmt, float bx_f, float by_f,
                           uint8_t &r, uint8_t &g, uint8_t &b) {
    if (bx_f < 0.0f) bx_f = 0.0f;
    if (by_f < 0.0f) by_f = 0.0f;

    uint32_t x0 = (uint32_t)bx_f;
    uint32_t y0 = (uint32_t)by_f;
    uint32_t x1 = std::min(x0 + 1, block_w - 1);
    uint32_t y1 = std::min(y0 + 1, block_h - 1);
    x0 = std::min(x0, block_w - 1);
    y0 = std::min(y0, block_h - 1);

    float dx = bx_f - (float)(uint32_t)bx_f;
    float dy = by_f - (float)(uint32_t)by_f;

    uint8_t r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
    readPixel(buf, pic_w, off_x + x0, off_y + y0, fmt, r00, g00, b00);
    readPixel(buf, pic_w, off_x + x1, off_y + y0, fmt, r10, g10, b10);
    readPixel(buf, pic_w, off_x + x0, off_y + y1, fmt, r01, g01, b01);
    readPixel(buf, pic_w, off_x + x1, off_y + y1, fmt, r11, g11, b11);

    float w00 = (1.0f - dx) * (1.0f - dy);
    float w10 = dx           * (1.0f - dy);
    float w01 = (1.0f - dx) * dy;
    float w11 = dx           * dy;

    r = (uint8_t)(w00 * r00 + w10 * r10 + w01 * r01 + w11 * r11 + 0.5f);
    g = (uint8_t)(w00 * g00 + w10 * g10 + w01 * g01 + w11 * g11 + 0.5f);
    b = (uint8_t)(w00 * b00 + w10 * b10 + w01 * b01 + w11 * b11 + 0.5f);
}

} // namespace

void SRMClient::setInputBlock(
    uint32_t pic_w, uint32_t pic_h,
    uint32_t block_w, uint32_t block_h,
    uint32_t block_offset_x, uint32_t block_offset_y,
    PixelFormat pixel_format
) {
    in_.pic_w = pic_w; in_.pic_h = pic_h;
    in_.block_w = block_w; in_.block_h = block_h;
    in_.block_offset_x = block_offset_x; in_.block_offset_y = block_offset_y;
    in_.fmt = pixel_format;
}

void SRMClient::setOutputBlock(
    uint32_t pic_w, uint32_t pic_h,
    uint32_t block_offset_x, uint32_t block_offset_y,
    PixelFormat pixel_format
) {
    out_.pic_w = pic_w; out_.pic_h = pic_h;
    out_.block_offset_x = block_offset_x; out_.block_offset_y = block_offset_y;
    out_.fmt = pixel_format;
}

void SRMClient::setRotation(int rotation) {
    if (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) {
        rotation_ = rotation;
    } else {
        fprintf(stderr, "PPA: Invalid rotation angle: %d\n", rotation);
    }
}

void SRMClient::setScale(float scale_x, float scale_y) {
    scale_x_ = scale_x;
    scale_y_ = scale_y;
}

Error SRMClient::do_scale_rotate_mirror(const void *input, void *output) {
    pthread_once(&srm_once, init_srm_mutex);

    if (scale_x_ <= 0.0f || scale_y_ <= 0.0f) return Error::InvalidArgument;

    const uint8_t *in_buf = static_cast<const uint8_t *>(input);
    uint8_t *out_buf = static_cast<uint8_t *>(output);

    uint32_t scaled_w = (uint32_t)(in_.block_w * scale_x_);
    uint32_t scaled_h = (uint32_t)(in_.block_h * scale_y_);
    if (scaled_w == 0 || scaled_h == 0) return Error::InvalidArgument;

    // Output block dimensions swap when rotating 90 or 270 degrees
    uint32_t out_block_w = (rotation_ == 90 || rotation_ == 270) ? scaled_h : scaled_w;
    uint32_t out_block_h = (rotation_ == 90 || rotation_ == 270) ? scaled_w : scaled_h;

    if (out_.block_offset_x + out_block_w > out_.pic_w ||
        out_.block_offset_y + out_block_h > out_.pic_h) {
        return Error::InvalidArgument;
    }

    pthread_mutex_lock(&srm_mutex);

    for (uint32_t oy = 0; oy < out_block_h; oy++) {
        for (uint32_t ox = 0; ox < out_block_w; ox++) {
            // Inverse rotation: map output (ox,oy) back to scaled-space (sx,sy)
            // Forward 90° CW:  (sx,sy) -> (scaled_h-1-sy, sx)
            // Forward 270° CW: (sx,sy) -> (sy, scaled_w-1-sx)
            uint32_t sx, sy;
            switch (rotation_) {
            case 0:   sx = ox;                sy = oy;                break;
            case 90:  sx = oy;                sy = scaled_h - 1 - ox; break;
            case 180: sx = scaled_w - 1 - ox; sy = scaled_h - 1 - oy; break;
            case 270: sx = scaled_w - 1 - oy; sy = ox;                break;
            default:  sx = ox;                sy = oy;                break;
            }

            // Inverse scale: bilinear interpolation
            float bx_f = sx / scale_x_;
            float by_f = sy / scale_y_;

            uint8_t r, g, b;
            sampleBilinear(in_buf, in_.pic_w, in_.pic_h,
                           in_.block_offset_x, in_.block_offset_y,
                           in_.block_w, in_.block_h,
                           in_.fmt, bx_f, by_f, r, g, b);
            writePixel(out_buf, out_.pic_w,
                       out_.block_offset_x + ox, out_.block_offset_y + oy, out_.fmt,
                       r, g, b);
        }
    }

    pthread_mutex_unlock(&srm_mutex);
    return Error::Ok;
}

} // namespace pf_port
