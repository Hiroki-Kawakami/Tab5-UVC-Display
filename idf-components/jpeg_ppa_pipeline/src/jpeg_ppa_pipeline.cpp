/*
 * High-level wrapper around the strip-pipelined JPEG decoder and PPA SRM.
 *
 * Each call to Pipeline::process() pushes a single JPEG frame through:
 *   1) strip-decode into K SRAM ring buffers (one decoder call, in this thread)
 *   2) per-strip PPA SRM into the destination frame buffer (worker task)
 *
 * The strip decoder fires on_strip_done from ISR; we forward each strip to a
 * dedicated PPA worker task via a queue. The worker calls
 * ppa_do_scale_rotate_mirror() in BLOCKING mode (which only blocks the worker,
 * not the JPEG decode thread) and then calls jpeg_strip_decoder_release_strip
 * so the DMA can recycle the SRAM buffer for a later strip in the same image.
 *
 * Wait-for-frame: process() releases the codec mutex once the JPEG hardware
 * reports RX_EOF, but the last few PPA strips may still be in flight. We
 * block on `all_strips_done_sem` (released by the worker after the last
 * strip's PPA op) before returning.
 */

#include "jpeg_ppa_pipeline.hpp"
#include "jpeg_strip_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "hal/color_hal.h"
#include <cstring>

static const char *TAG = "jpeg_ppa";

namespace jpeg_ppa {

namespace {

constexpr int WORKER_STACK = 4096;
constexpr int WORKER_PRIO  = 17;

struct StripWork {
    uint32_t strip_idx;
};

}  // namespace

struct Pipeline::Impl {
    Config cfg{};
    uint32_t strip_count = 0;
    uint32_t bytes_per_pixel = 0;
    size_t strip_byte_size = 0;

    void *strip_bufs[8] = {};    // up to 8 ring buffers should be enough
    jpeg_strip_decoder_handle_t decoder = nullptr;
    ppa_client_handle_t ppa_client = nullptr;

    QueueHandle_t worker_queue = nullptr;
    SemaphoreHandle_t all_done = nullptr;
    TaskHandle_t worker_task = nullptr;

    // updated per process()
    void *current_output_fb = nullptr;
    RenderOpts current_opts{};
    volatile uint32_t pending_strips = 0;
    volatile bool worker_should_exit = false;

    // ISR-driven strip handoff to the worker task
    static bool on_strip_done_isr(uint32_t strip_idx, void *buffer, void *user_ctx);
    static void worker_entry(void *arg);
    void worker_loop();

    esp_err_t set_up_ppa_op(uint32_t strip_idx, ppa_srm_oper_config_t &op);
};

bool Pipeline::Impl::on_strip_done_isr(uint32_t strip_idx, void * /*buffer*/, void *user_ctx)
{
    auto *self = static_cast<Pipeline::Impl *>(user_ctx);
    StripWork w{ strip_idx };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(self->worker_queue, &w, &hp);
    return hp == pdTRUE;
}

void Pipeline::Impl::worker_entry(void *arg)
{
    auto *self = static_cast<Pipeline::Impl *>(arg);
    self->worker_loop();
    vTaskDelete(nullptr);
}

esp_err_t Pipeline::Impl::set_up_ppa_op(uint32_t strip_idx, ppa_srm_oper_config_t &op)
{
    uint32_t ring_slot = strip_idx % cfg.ring_count;
    op.in.buffer            = strip_bufs[ring_slot];
    op.in.pic_w             = cfg.pic_w;
    op.in.pic_h             = cfg.strip_h;             // each strip buffer holds strip_h rows
    op.in.block_w           = cfg.pic_w;
    op.in.block_h           = cfg.strip_h;
    op.in.block_offset_x    = 0;
    op.in.block_offset_y    = 0;
    op.in.srm_cm            = cfg.input_color_mode;

    op.out.buffer           = current_output_fb;
    op.out.buffer_size      = cfg.out_pic_w * cfg.out_pic_h * (bytes_per_pixel);
    op.out.pic_w            = cfg.out_pic_w;
    op.out.pic_h            = cfg.out_pic_h;
    op.out.srm_cm           = cfg.out_color_mode;

    // Place strip i in the output picture according to the rotation. Each
    // strip is an input slab of (pic_w × strip_h) pixels at input-y =
    // strip_idx * strip_h; after rotation it becomes a column / row in the
    // output of size (scaled_strip × scaled_pic_w_or_h). PPA hardware
    // convention: ANGLE_90 here is CW (input top-row → output right-column),
    // ANGLE_270 is CCW.
    uint32_t scaled_strip = (uint32_t)(cfg.strip_h * cfg.scale_y);
    switch (cfg.rotation) {
    case PPA_SRM_ROTATION_ANGLE_0:
        op.out.block_offset_x = 0;
        op.out.block_offset_y = strip_idx * scaled_strip;
        break;
    case PPA_SRM_ROTATION_ANGLE_90:
        op.out.block_offset_x = strip_idx * scaled_strip;
        op.out.block_offset_y = 0;
        break;
    case PPA_SRM_ROTATION_ANGLE_180:
        op.out.block_offset_x = 0;
        op.out.block_offset_y = (strip_count - 1 - strip_idx) * scaled_strip;
        break;
    case PPA_SRM_ROTATION_ANGLE_270:
        op.out.block_offset_x = (strip_count - 1 - strip_idx) * scaled_strip;
        op.out.block_offset_y = 0;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    // Apply runtime output-y clip if requested. ANGLE_90 + scale_x==1 maps
    // panel_y = pic_w - 1 - camera_x within each strip (the strip placement
    // implies PPA's ANGLE_90 behaves as CCW: camera top-left lands at panel
    // bottom-left, etc.). To skip panel_y ∈ [0, out_y_start) ∪ [out_y_end,
    // pic_w) without shifting the rendered content, trim the camera-x range
    // to the columns whose panel_y stays inside [out_y_start, out_y_end).
    //   panel_y = pic_w - 1 - cx  ⇒  cx ∈ [pic_w - out_y_end, pic_w - out_y_start)
    // Output goes to panel_y starting at out_y_start; the mapping inside the
    // clipped band is identical to the no-clip case.
    if (current_opts.out_y_end > current_opts.out_y_start &&
        cfg.rotation == PPA_SRM_ROTATION_ANGLE_90 && cfg.scale_x == 1.0f) {
        op.in.block_offset_x   = cfg.pic_w - current_opts.out_y_end;
        op.in.block_w          = current_opts.out_y_end - current_opts.out_y_start;
        op.out.block_offset_y  = current_opts.out_y_start;
    }

    op.rotation_angle = cfg.rotation;
    op.scale_x = cfg.scale_x;
    op.scale_y = cfg.scale_y;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ESP_OK;
}

void Pipeline::Impl::worker_loop()
{
    while (true) {
        StripWork w;
        if (xQueueReceive(worker_queue, &w, portMAX_DELAY) != pdTRUE) continue;
        if (worker_should_exit) return;

        ppa_srm_oper_config_t op{};
        if (set_up_ppa_op(w.strip_idx, op) != ESP_OK) continue;
        esp_err_t err = ppa_do_scale_rotate_mirror(ppa_client, &op);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ppa strip %lu err=%s", (unsigned long)w.strip_idx, esp_err_to_name(err));
        }

        // Free this descriptor's slot in the chain (for future strips re-using
        // the same SRAM buffer in this frame). This is a no-op for the last
        // ring_count strips of a frame, but is essential for the rest.
        jpeg_strip_decoder_release_strip(decoder, w.strip_idx);

        if (__atomic_sub_fetch(&pending_strips, 1, __ATOMIC_ACQ_REL) == 0) {
            xSemaphoreGive(all_done);
        }
    }
}

// -----------------------------------------------------------------------------
// Pipeline ctor / dtor / init / deinit / process
// -----------------------------------------------------------------------------

Pipeline::Pipeline() : impl_(new Impl()) {}

Pipeline::~Pipeline() {
    deinit();
    delete impl_;
}

// bit_depth/8 truncates for YUV420 (12 bpp -> 1.5 bytes/pixel), so callers
// using this for output buffer sizing should multiply by bit_depth and divide
// by 8 explicitly instead of going through this helper.
static uint32_t s_bytes_per_pixel_int(ppa_srm_color_mode_t cm) {
    color_space_pixel_format_t f{};
    f.color_type_id = cm;
    return color_hal_pixel_format_get_bit_depth(f) / 8;
}

static uint32_t s_bit_depth(ppa_srm_color_mode_t cm) {
    color_space_pixel_format_t f{};
    f.color_type_id = cm;
    return color_hal_pixel_format_get_bit_depth(f);
}

static jpeg_dec_output_format_t s_jpeg_out_for_ppa(ppa_srm_color_mode_t cm) {
    switch (cm) {
    case PPA_SRM_COLOR_MODE_RGB565: return JPEG_DECODE_OUT_FORMAT_RGB565;
    case PPA_SRM_COLOR_MODE_RGB888: return JPEG_DECODE_OUT_FORMAT_RGB888;
    case PPA_SRM_COLOR_MODE_YUV420: return JPEG_DECODE_OUT_FORMAT_YUV420;
    default:                        return JPEG_DECODE_OUT_FORMAT_RGB565;
    }
}

esp_err_t Pipeline::init(const Config &cfg)
{
    if (cfg.pic_h % cfg.strip_h != 0) {
        ESP_LOGE(TAG, "pic_h %lu not divisible by strip_h %lu",
                 (unsigned long)cfg.pic_h, (unsigned long)cfg.strip_h);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.ring_count == 0 || cfg.ring_count > 8) return ESP_ERR_INVALID_ARG;

    impl_->cfg = cfg;
    impl_->strip_count = cfg.pic_h / cfg.strip_h;
    // Use bit_depth/8 for output FB sizing (integer bpp formats only).
    impl_->bytes_per_pixel = s_bytes_per_pixel_int(cfg.out_color_mode);
    // Use bit_depth math for the intermediate strip so YUV420 (12 bpp -> 1.5 B/px) sizes correctly.
    impl_->strip_byte_size = cfg.strip_h * cfg.pic_w * s_bit_depth(cfg.input_color_mode) / 8;

    // Allocate SRAM strip buffers (internal RAM, DMA capable).
    for (uint32_t i = 0; i < cfg.ring_count; i++) {
        impl_->strip_bufs[i] = heap_caps_aligned_calloc(64, 1, impl_->strip_byte_size,
                                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!impl_->strip_bufs[i]) {
            ESP_LOGE(TAG, "strip buffer %lu alloc failed (%lu bytes)", (unsigned long)i, (unsigned long)impl_->strip_byte_size);
            deinit();
            return ESP_ERR_NO_MEM;
        }
    }

    impl_->worker_queue = xQueueCreate(impl_->strip_count, sizeof(StripWork));
    impl_->all_done = xSemaphoreCreateBinary();
    if (!impl_->worker_queue || !impl_->all_done) { deinit(); return ESP_ERR_NO_MEM; }

    // PPA SRM client: queue depth = ring_count keeps the strip decoder back-pressure simple
    ppa_client_config_t pcfg{};
    pcfg.oper_type = PPA_OPERATION_SRM;
    pcfg.max_pending_trans_num = cfg.ring_count;
    esp_err_t err = ppa_register_client(&pcfg, &impl_->ppa_client);
    if (err != ESP_OK) { deinit(); return err; }

    // Strip decoder
    jpeg_strip_decode_cfg_t dcfg{};
    dcfg.base_cfg.output_format = s_jpeg_out_for_ppa(cfg.input_color_mode);
    dcfg.base_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    dcfg.base_cfg.conv_std = cfg.yuv_rgb_conv_std;
    dcfg.pic_w = cfg.pic_w;
    dcfg.pic_h = cfg.pic_h;
    dcfg.strip_h = cfg.strip_h;
    dcfg.ring_count = cfg.ring_count;
    dcfg.strip_buffers = impl_->strip_bufs;
    dcfg.on_strip_done = Impl::on_strip_done_isr;
    dcfg.user_ctx = impl_;
    err = jpeg_strip_decoder_new(&dcfg, &impl_->decoder);
    if (err != ESP_OK) { deinit(); return err; }

    if (xTaskCreatePinnedToCore(Impl::worker_entry, "jpeg_ppa_w",
                                WORKER_STACK, impl_, WORKER_PRIO, &impl_->worker_task, 0) != pdPASS) {
        deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t Pipeline::deinit()
{
    if (!impl_) return ESP_OK;
    if (impl_->worker_task) {
        impl_->worker_should_exit = true;
        // Wake worker so it can observe the exit flag
        StripWork stop{ UINT32_MAX };
        xQueueSend(impl_->worker_queue, &stop, 0);
        // We use vTaskDelete inside the worker; can't easily join, just yield.
        vTaskDelay(pdMS_TO_TICKS(20));
        impl_->worker_task = nullptr;
    }
    if (impl_->decoder)     { jpeg_strip_decoder_del(impl_->decoder); impl_->decoder = nullptr; }
    if (impl_->ppa_client)  { ppa_unregister_client(impl_->ppa_client); impl_->ppa_client = nullptr; }
    if (impl_->worker_queue){ vQueueDelete(impl_->worker_queue); impl_->worker_queue = nullptr; }
    if (impl_->all_done)    { vSemaphoreDelete(impl_->all_done); impl_->all_done = nullptr; }
    for (uint32_t i = 0; i < 8; i++) {
        if (impl_->strip_bufs[i]) { heap_caps_free(impl_->strip_bufs[i]); impl_->strip_bufs[i] = nullptr; }
    }
    return ESP_OK;
}

esp_err_t Pipeline::process(const void *jpeg_data, size_t jpeg_size, void *output_fb,
                            const RenderOpts &opts)
{
    if (!impl_->decoder || !output_fb) return ESP_ERR_INVALID_STATE;
    impl_->current_output_fb = output_fb;
    impl_->current_opts = opts;
    impl_->pending_strips = impl_->strip_count;
    // Drain stale completion (defensive; should always be 0)
    xSemaphoreTake(impl_->all_done, 0);

    esp_err_t err = jpeg_strip_decoder_process(impl_->decoder,
                                               static_cast<const uint8_t *>(jpeg_data),
                                               static_cast<uint32_t>(jpeg_size));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "strip decode err=%s", esp_err_to_name(err));
        // Drain queued strip work and reset pending count, otherwise the next
        // process() will block forever.
        StripWork tmp;
        while (xQueueReceive(impl_->worker_queue, &tmp, 0) == pdTRUE) {}
        impl_->pending_strips = 0;
        return err;
    }

    // Wait for all PPA strips to finish before allowing the caller to use the FB.
    xSemaphoreTake(impl_->all_done, portMAX_DELAY);
    return ESP_OK;
}

}  // namespace jpeg_ppa
