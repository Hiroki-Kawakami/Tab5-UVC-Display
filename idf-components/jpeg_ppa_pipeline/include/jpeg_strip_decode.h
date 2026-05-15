#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/jpeg_decode.h"
#include "esp_private/dma2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-strip event callback. Fired from 2D-DMA ISR context when a strip
 *        worth of decoded pixels has finished being written to its SRAM buffer.
 *
 * @param strip_idx  Index of the just-completed strip (0..num_strips-1).
 * @param buffer     Pointer to the SRAM buffer that now holds the decoded strip.
 * @param user_ctx   User-supplied context.
 * @return Whether the callback woke a higher priority task (forwarded as task yield request).
 */
typedef bool (*jpeg_strip_on_done_t)(uint32_t strip_idx, void *buffer, void *user_ctx);

/**
 * @brief Configuration of strip-based JPEG decode.
 */
typedef struct {
    jpeg_decode_cfg_t base_cfg;   /*!< Standard JPEG decode config (output_format, rgb_order, conv_std) */

    uint32_t pic_w;               /*!< Expected picture width in pixels */
    uint32_t pic_h;               /*!< Expected picture height in pixels (must be multiple of MCU height) */
    uint32_t strip_h;             /*!< Height (rows) of one strip, must be multiple of MCU height */
    uint32_t ring_count;          /*!< Number of distinct SRAM strip buffers in the ring (<= strip_count) */

    void **strip_buffers;         /*!< Array of ring_count pointers to SRAM strip buffers. Each buffer is strip_h * pic_w * bytes_per_pixel bytes, DMA-able internal RAM. */

    jpeg_strip_on_done_t on_strip_done;   /*!< Fired per strip (in ISR) */
    void *user_ctx;
} jpeg_strip_decode_cfg_t;

/**
 * @brief Strip decoder handle (opaque).
 */
typedef struct jpeg_strip_decoder_s *jpeg_strip_decoder_handle_t;

/**
 * @brief Acquire the JPEG hardware and prepare strip-mode resources.
 *
 * Internally uses jpeg_new_decoder_engine() to grab the JPEG engine, DMA pool,
 * codec mutex, and ISR, then allocates the descriptor chain used for chained
 * RX into SRAM.
 *
 * @note pic_w / pic_h / strip_h / ring_count are part of the config so the
 *       descriptor chain and per-strip parameters can be computed up front.
 *
 * @return ESP_OK on success.
 */
esp_err_t jpeg_strip_decoder_new(const jpeg_strip_decode_cfg_t *cfg, jpeg_strip_decoder_handle_t *out_handle);

/**
 * @brief Release a strip decoder.
 */
esp_err_t jpeg_strip_decoder_del(jpeg_strip_decoder_handle_t handle);

/**
 * @brief Process one JPEG image with chained-strip RX.
 *
 * Blocks until the JPEG hardware signals end-of-frame. While running, the
 * on_strip_done callback fires from ISR context for each strip as it lands
 * in SRAM.
 *
 * After a strip's downstream consumer is finished with the buffer, call
 * jpeg_strip_decoder_release_strip() to let the chained DMA reuse that
 * descriptor slot for a later strip in the same image (or stay idle).
 *
 * @param bit_stream JPEG-encoded input (PSRAM ok).
 * @param stream_size Size of the JPEG input.
 * @return ESP_OK on success.
 */
esp_err_t jpeg_strip_decoder_process(jpeg_strip_decoder_handle_t handle,
                                     const uint8_t *bit_stream,
                                     uint32_t stream_size);

/**
 * @brief Tell the strip decoder that consumer has finished with strip_idx, so
 *        its buffer can be reused for later strips in the same image.
 *
 * Safe to call from any task / non-ISR context. The descriptor's owner bit is
 * re-armed and the DMA channel is woken via dma2d_append().
 */
esp_err_t jpeg_strip_decoder_release_strip(jpeg_strip_decoder_handle_t handle, uint32_t strip_idx);

#ifdef __cplusplus
}
#endif
