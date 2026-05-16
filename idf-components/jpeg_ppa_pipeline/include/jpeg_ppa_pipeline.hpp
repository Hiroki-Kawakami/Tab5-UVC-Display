#pragma once

#include <stdint.h>
#include <cstddef>
#include "esp_err.h"
#include "driver/ppa.h"
#include "driver/jpeg_decode.h"

namespace jpeg_ppa {

/**
 * @brief Configuration for the strip-pipelined JPEG decode + PPA SRM operation.
 *
 * The pipeline decodes a single JPEG image into a ring of SRAM strip buffers
 * (each strip_h rows tall) and feeds each strip through PPA SRM to the
 * destination frame buffer as soon as it is decoded.
 *
 * Constraints:
 *  - pic_h % mcu_h == 0 (mcu_h is 16 for YUV420, 8 for YUV422, etc.)
 *  - strip_h % mcu_h == 0 and pic_h % strip_h == 0
 *  - ring_count >= 2 for any pipelining benefit
 *  - strip_h * pic_w * bytes_per_pixel * ring_count must fit in MALLOC_CAP_INTERNAL
 */
struct Config {
    uint32_t pic_w;                                 // input picture width (pixels)
    uint32_t pic_h;                                 // input picture height (pixels)
    uint32_t strip_h;                               // height of one strip (pixels)
    uint32_t ring_count;                            // # of SRAM strip buffers
    ppa_srm_color_mode_t input_color_mode;          // intermediate strip color mode (e.g. PPA_SRM_COLOR_MODE_RGB565)

    uint32_t out_pic_w;                             // output (frame buffer) width
    uint32_t out_pic_h;                             // output (frame buffer) height
    ppa_srm_color_mode_t out_color_mode;            // output color mode

    ppa_srm_rotation_angle_t rotation;              // PPA_SRM_ROTATION_ANGLE_{0,90,180,270}
    float scale_x;                                  // SRM scale factor X
    float scale_y;                                  // SRM scale factor Y

    // YUV→RGB conversion standard applied by the JPEG hardware's 2D-DMA CSC
    // when the input_color_mode is an RGB format. Defaults to BT601 (matches
    // most webcam encoder output); use BT709 for HD/Rec.709 streams.
    jpeg_yuv_rgb_conv_std_t yuv_rgb_conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
};

/// Per-call options for Pipeline::process(). Default-constructed renders the
/// full output picture.
struct RenderOpts {
    // Restrict rendering to the output's vertical band [out_y_start, out_y_end).
    // Pixels outside the band are left untouched (caller's responsibility to
    // overlay or repaint them). Only honored for rotation=ANGLE_90 with
    // scale_x=1; ignored otherwise. out_y_end<=out_y_start disables clipping.
    uint32_t out_y_start = 0;
    uint32_t out_y_end   = 0;
};

class Pipeline {
public:
    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    /// Set up SRAM strip ring, JPEG strip decoder, PPA SRM client, and worker task.
    esp_err_t init(const Config &cfg);
    esp_err_t deinit();

    /// Decode a JPEG and render it to `output_fb`. Blocks until the entire
    /// frame has been pushed through PPA SRM to the frame buffer.
    esp_err_t process(const void *jpeg_data, size_t jpeg_size, void *output_fb,
                      const RenderOpts &opts = {});

private:
    struct Impl;
    Impl *impl_;
};

}  // namespace jpeg_ppa
