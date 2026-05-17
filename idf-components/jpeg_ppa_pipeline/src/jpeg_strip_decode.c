/*
 * Strip-pipelined JPEG decode: replays the bulk of the IDF jpeg_decoder_process()
 * flow but writes the decoded image into a ring of N internal-RAM strip buffers
 * via N linked 2D-DMA RX descriptors. Each finished strip fires on_strip_done()
 * from ISR context so a downstream consumer (PPA SRM in our case) can start
 * processing it immediately, in parallel with the decode of subsequent strips.
 *
 * Bandwidth motivation: when the JPEG codec writes into PSRAM and PPA later
 * reads the same data back from PSRAM, the round-trip dominates the PSRAM
 * write/read bandwidth budget on ESP32-P4 and limits 1280x720 throughput to
 * ~20fps. Keeping the intermediate raster in internal SRAM removes that
 * round-trip; the only remaining PSRAM-write traffic is the final rotated
 * framebuffer.
 */

#include <stdlib.h>
#include <string.h>
#include "jpeg_strip_decode.h"
#include "jpeg_private.h"
#include "private/jpeg_parse_marker.h"
#include "private/jpeg_param.h"
#include "esp_private/dma2d.h"
#include "hal/jpeg_ll.h"
#include "hal/jpeg_hal.h"
#include "hal/jpeg_defs.h"
#include "hal/cache_ll.h"
#include "hal/cache_hal.h"
#include "hal/dma2d_ll.h"
#include "hal/color_hal.h"
#include "soc/dma2d_channel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

static const char *TAG = "jpeg_strip";

struct jpeg_strip_decoder_s {
    jpeg_decoder_handle_t engine;       // IDF JPEG decoder engine (owns DMA pool, mutex, ISR)

    jpeg_strip_decode_cfg_t cfg;        // copy of user config
    uint32_t strip_count;               // pic_h / strip_h

    dma2d_descriptor_t *rxlinks;        // strip_count contiguous descriptors
    dma2d_descriptor_t *txlink;         // 1 descriptor for the input bitstream
    size_t desc_byte_size;              // size of one descriptor (cache aligned)

    SemaphoreHandle_t frame_done_sem;   // released by on_recv_eof from last RX desc
    portMUX_TYPE spin;                  // protects chain bookkeeping

    dma2d_channel_handle_t rx_chan;     // captured in on_job_picked, used by release_strip()
    volatile uint32_t isr_next_strip;   // next strip index expected by on_desc_done (in-order)
    volatile uint32_t chain_tail;       // index of the last descriptor currently linked into the chain (its .next is NULL)

    uint32_t dma_hb;                    // horizontal block size (pixels) for the RX descriptor
    uint32_t dma_vb;                    // vertical block size  (= mcu_y)
};

// =============================================================================
// JPEG header parsing helpers (replicas of static helpers in jpeg_decode.c)
// =============================================================================

static esp_err_t s_jpeg_default_huff_table(jpeg_dec_header_info_t *header_info)
{
    memcpy(header_info->huffbits[0][0], luminance_dc_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffbits[0][1], chrominance_dc_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffbits[1][0], luminance_ac_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffbits[1][1], chrominance_ac_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffcode[0][0], luminance_dc_values, JPEG_HUFFMAN_DC_VALUE_TABLE_LEN);
    memcpy(header_info->huffcode[0][1], chrominance_dc_values, JPEG_HUFFMAN_DC_VALUE_TABLE_LEN);
    memcpy(header_info->huffcode[1][0], luminance_ac_values, JPEG_HUFFMAN_AC_VALUE_TABLE_LEN);
    memcpy(header_info->huffcode[1][1], chrominance_ac_values, JPEG_HUFFMAN_AC_VALUE_TABLE_LEN);
    return ESP_OK;
}

static esp_err_t s_parse_jpeg(jpeg_decoder_handle_t engine, const uint8_t *in_buf, uint32_t inbuf_len)
{
    jpeg_dec_header_info_t *header_info = engine->header_info;
    jpeg_hal_context_t *hal = &engine->codec_base->hal;

    memset(header_info, 0, sizeof(*header_info));
    header_info->buffer_offset = (uint8_t *)in_buf;
    header_info->buffer_left = inbuf_len;
    engine->total_size = inbuf_len;
    header_info->header_size = 0;

    jpeg_ll_soft_rst(hal->dev);
    jpeg_ll_set_codec_mode(hal->dev, JPEG_CODEC_DECODER);
    jpeg_ll_set_picture_height(hal->dev, 0);
    jpeg_ll_set_picture_width(hal->dev, 0);

    const uint8_t *buf_end = in_buf + inbuf_len;
    while (header_info->buffer_left) {
        // Detect underflow / runaway: the IDF marker handlers advance
        // buffer_offset by a length field read from the stream, then
        // decrement buffer_left. A corrupt MJPEG (e.g. dropped USB isoc
        // packet) can make that length huge, wrapping buffer_left around
        // 0 and walking buffer_offset off the end of PSRAM. The invariants
        // below hold for a clean parse; if either fails we've left the
        // buffer and must bail before reading unmapped memory.
        if (header_info->buffer_left > inbuf_len ||
            header_info->buffer_offset < in_buf ||
            header_info->buffer_offset >= buf_end) {
            ESP_LOGW(TAG, "header parse out of bounds (corrupt JPEG)");
            return ESP_ERR_INVALID_STATE;
        }
        uint8_t lastchar = jpeg_get_bytes(header_info, 1);
        uint8_t thischar = jpeg_get_bytes(header_info, 1);
        uint16_t marker = (lastchar << 8 | thischar);
        switch (marker) {
        case JPEG_M_SOI:
            break;
        case JPEG_M_APP0: case JPEG_M_APP1: case JPEG_M_APP2: case JPEG_M_APP3:
        case JPEG_M_APP4: case JPEG_M_APP5: case JPEG_M_APP6: case JPEG_M_APP7:
        case JPEG_M_APP8: case JPEG_M_APP9: case JPEG_M_APP10: case JPEG_M_APP11:
        case JPEG_M_APP12: case JPEG_M_APP13: case JPEG_M_APP14: case JPEG_M_APP15:
            jpeg_parse_appn_marker(header_info);
            break;
        case JPEG_M_COM:
            jpeg_parse_com_marker(header_info);
            break;
        case JPEG_M_DQT:
            jpeg_parse_dqt_marker(header_info);
            break;
        case JPEG_M_SOF0:
            if (jpeg_parse_sof_marker(header_info) != ESP_OK) return ESP_ERR_INVALID_STATE;
            break;
        case JPEG_M_SOF1: case JPEG_M_SOF2: case JPEG_M_SOF3: case JPEG_M_SOF5:
        case JPEG_M_SOF6: case JPEG_M_SOF7: case JPEG_M_SOF9: case JPEG_M_SOF10:
        case JPEG_M_SOF11: case JPEG_M_SOF13: case JPEG_M_SOF14: case JPEG_M_SOF15:
            ESP_LOGE(TAG, "Only baseline-DCT JPEG is supported");
            return ESP_ERR_NOT_SUPPORTED;
        case JPEG_M_DRI:
            jpeg_parse_dri_marker(header_info);
            break;
        case JPEG_M_DHT:
            jpeg_parse_dht_marker(header_info);
            break;
        case JPEG_M_SOS:
            jpeg_parse_sos_marker(header_info);
            break;
        case JPEG_M_INV:
            jpeg_parse_inv_marker(header_info);
            break;
        }
        if (marker == JPEG_M_SOS) break;
    }

    header_info->buffer_left = engine->total_size - header_info->header_size;

    if (!header_info->dht_marker) {
        s_jpeg_default_huff_table(header_info);
    }
    return ESP_OK;
}

static esp_err_t s_apply_header_to_hw(jpeg_decoder_handle_t engine)
{
    jpeg_dec_header_info_t *header_info = engine->header_info;
    jpeg_hal_context_t *hal = &engine->codec_base->hal;

    for (int i = 0; i < header_info->qt_tbl_num; i++) {
        dqt_func[i](hal->dev, header_info->qt_tbl[i]);
    }
    jpeg_ll_set_picture_height(hal->dev, header_info->process_v);
    jpeg_ll_set_picture_width(hal->dev, header_info->process_h);
    jpeg_ll_set_decode_component_num(hal->dev, header_info->nf);
    for (int i = 0; i < header_info->nf; i++) {
        sof_func[i](hal->dev, header_info->ci[i], header_info->hi[i], header_info->vi[i], header_info->qtid[i]);
    }
    if (header_info->nf == 3) {
        switch (header_info->hivi[0]) {
        case 0x11: engine->sample_method = JPEG_DOWN_SAMPLING_YUV444; break;
        case 0x21: engine->sample_method = JPEG_DOWN_SAMPLING_YUV422; break;
        case 0x22: engine->sample_method = JPEG_DOWN_SAMPLING_YUV420; break;
        default: return ESP_ERR_INVALID_STATE;
        }
    } else if (header_info->nf == 1) {
        engine->sample_method = JPEG_DOWN_SAMPLING_GRAY;
    }

    engine->no_color_conversion = ((uint32_t)engine->sample_method == (uint32_t)engine->output_format);

    dht_func[0][0](hal, header_info->huffbits[0][0], header_info->huffcode[0][0], header_info->tmp_huff);
    dht_func[0][1](hal, header_info->huffbits[0][1], header_info->huffcode[0][1], header_info->tmp_huff);
    dht_func[1][0](hal, header_info->huffbits[1][0], header_info->huffcode[1][0], header_info->tmp_huff);
    dht_func[1][1](hal, header_info->huffbits[1][1], header_info->huffcode[1][1], header_info->tmp_huff);

    jpeg_ll_set_restart_interval(hal->dev, header_info->ri);
    return ESP_OK;
}

// =============================================================================
// Descriptor configuration
// =============================================================================

static inline jpeg_dec_format_hb_t s_best_hb_idx(jpeg_dec_output_format_t out_fmt, bool no_csc)
{
    if (no_csc) return JPEG_DEC_DIRECT_OUTPUT_HB;
    switch (out_fmt) {
    case JPEG_DECODE_OUT_FORMAT_RGB888: return JPEG_DEC_RGB888_HB;
    case JPEG_DECODE_OUT_FORMAT_RGB565: return JPEG_DEC_RGB565_HB;
    case JPEG_DECODE_OUT_FORMAT_GRAY:   return JPEG_DEC_GRAY_HB;
    case JPEG_DECODE_OUT_FORMAT_YUV444: return JPEG_DEC_YUV444_HB;
    default: return JPEG_DEC_BEST_HB_MAX;
    }
}

static inline uint8_t s_sample_idx(jpeg_down_sampling_type_t s)
{
    switch (s) {
    case JPEG_DOWN_SAMPLING_YUV444: return 0;
    case JPEG_DOWN_SAMPLING_YUV422: return 1;
    case JPEG_DOWN_SAMPLING_YUV420: return 2;
    case JPEG_DOWN_SAMPLING_GRAY:   return 3;
    default: return 0;
    }
}

static void s_init_descriptors(jpeg_strip_decoder_handle_t h)
{
    jpeg_dec_header_info_t *hi = h->engine->header_info;
    color_space_pixel_format_t picture_format = { .color_type_id = h->engine->output_format };
    h->engine->bit_per_pixel = color_hal_pixel_format_get_bit_depth(picture_format);

    jpeg_dec_format_hb_t hb_idx = s_best_hb_idx(h->engine->output_format, h->engine->no_color_conversion);
    h->dma_hb = dec_hb_tbl[s_sample_idx(h->engine->sample_method)][hb_idx];
    h->dma_vb = hi->mcuy;

    // TX descriptor (single, points at the JPEG bitstream)
    h->txlink->dma2d_en      = JPEG_DMA2D_2D_DISABLE;
    h->txlink->mode          = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
    h->txlink->vb_size       = hi->buffer_left & JPEG_DMA2D_MAX_SIZE;
    h->txlink->hb_length     = hi->buffer_left & JPEG_DMA2D_MAX_SIZE;
    h->txlink->pbyte         = 1;
    h->txlink->suc_eof       = JPEG_DMA2D_EOF_NOT_LAST;
    h->txlink->owner         = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
    h->txlink->va_size       = (hi->buffer_left >> JPEG_DMA2D_1D_HIGH_14BIT);
    h->txlink->ha_length     = (hi->buffer_left >> JPEG_DMA2D_1D_HIGH_14BIT);
    h->txlink->buffer        = hi->buffer_offset;
    h->txlink->next          = NULL;
    h->txlink->err_eof       = 0;
    h->txlink->x = h->txlink->y = 0;

    // RX descriptors (one per strip; descriptor i points to ring buffer
    // i % ring_count). We initialize all of them with the static fields, but
    // only LINK the first `ring_count` into the chain — that limits how far
    // ahead DMA can run from PPA. The remaining descriptors are spliced in
    // via dma2d_append() as PPA finishes the corresponding earlier strips
    // (see jpeg_strip_decoder_release_strip).
    //
    // suc_eof=0 on every descriptor: the JPEG→DMA bridge drives SUC_EOF off
    // the JPEG hardware's frame-done signal, matching IDF's own decoder.
    h->isr_next_strip = 0;
    for (uint32_t i = 0; i < h->strip_count; i++) {
        dma2d_descriptor_t *d = (dma2d_descriptor_t *)((uint8_t *)h->rxlinks + i * h->desc_byte_size);
        uint32_t ring_slot = i % h->cfg.ring_count;

        d->dma2d_en   = JPEG_DMA2D_2D_ENABLE;
        d->mode       = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_MULTIPLE;
        d->vb_size    = h->dma_vb;
        d->hb_length  = h->dma_hb;
        d->pbyte      = dma2d_desc_pixel_format_to_pbyte_value(picture_format);
        d->suc_eof    = JPEG_DMA2D_EOF_NOT_LAST;
        d->owner      = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
        d->va_size    = h->cfg.strip_h;
        d->ha_length  = hi->process_h;
        d->buffer     = h->cfg.strip_buffers[ring_slot];
        d->err_eof    = 0;
        d->x = d->y   = 0;

        // Link only the first ring_count descriptors initially.
        if (i + 1 < h->cfg.ring_count && i + 1 < h->strip_count) {
            d->next = (dma2d_descriptor_t *)((uint8_t *)h->rxlinks + (i + 1) * h->desc_byte_size);
        } else {
            d->next = NULL;
        }
    }
    // Last descriptor currently in the chain is the (ring_count - 1)-th, or
    // (strip_count - 1)-th if the image is smaller than the ring.
    h->chain_tail = (h->cfg.ring_count < h->strip_count) ? (h->cfg.ring_count - 1)
                                                         : (h->strip_count - 1);

    esp_cache_msync(h->rxlinks, h->desc_byte_size * h->strip_count, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cache_msync(h->txlink,  h->desc_byte_size,                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

// =============================================================================
// 2D-DMA channel & event plumbing
// =============================================================================

// Descriptors are processed strictly in chain order, so we count strips with a
// simple in-order counter instead of trying to identify the descriptor from
// the (non-populated for RX_DONE) event data.
static IRAM_ATTR bool s_on_desc_done(dma2d_channel_handle_t chan, dma2d_event_data_t *evt, void *user_data)
{
    (void)chan; (void)evt;
    jpeg_strip_decoder_handle_t h = (jpeg_strip_decoder_handle_t)user_data;
    uint32_t idx = h->isr_next_strip++;
    if (idx >= h->strip_count) return false;
    uint32_t ring_slot = idx % h->cfg.ring_count;
    if (h->cfg.on_strip_done) {
        return h->cfg.on_strip_done(idx, h->cfg.strip_buffers[ring_slot], h->cfg.user_ctx);
    }
    return false;
}

static IRAM_ATTR bool s_on_recv_eof(dma2d_channel_handle_t chan, dma2d_event_data_t *evt, void *user_data)
{
    (void)chan; (void)evt;
    jpeg_strip_decoder_handle_t h = (jpeg_strip_decoder_handle_t)user_data;
    BaseType_t hp = pdFALSE;
    // Mirror the IDF JPEG decoder: post RX_EOF to the engine's event queue so
    // the waiting jpeg_decoder_process_strip() loop can unblock. Also release
    // frame_done_sem in case a future caller wants direct wait semantics.
    jpeg_dma2d_dec_evt_t e = { .dma_evt = JPEG_DMA2D_RX_EOF, .jpgd_status = 0 };
    xQueueSendFromISR(h->engine->evt_queue, &e, &hp);
    xSemaphoreGiveFromISR(h->frame_done_sem, &hp);
    return hp == pdTRUE;
}

static void s_dma_apply_jpeg_transfer_ability(jpeg_strip_decoder_handle_t h,
                                              dma2d_channel_handle_t tx, dma2d_channel_handle_t rx)
{
    dma2d_transfer_ability_t tx_ab = {
        .data_burst_length = DMA2D_DATA_BURST_LENGTH_128,
        .desc_burst_en = true,
        .mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE,
    };
    dma2d_transfer_ability_t rx_ab = tx_ab;
    switch (h->engine->sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444: rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_8_8;   break;
    case JPEG_DOWN_SAMPLING_YUV422: rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_8_16;  break;
    case JPEG_DOWN_SAMPLING_YUV420: rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_16_16; break;
    case JPEG_DOWN_SAMPLING_GRAY:   rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_8_8;   break;
    default: break;
    }
    dma2d_set_transfer_ability(tx, &tx_ab);
    dma2d_set_transfer_ability(rx, &rx_ab);
}

// Full-range BT.601 YCbCr->RGB matrix (JPEG/JFIF convention, Y/Cb/Cr all in
// [0,255], output in [0,255]):
//     R = Y                  + 1.402   *(Cr - 128)
//     G = Y - 0.344136*(Cb - 128) - 0.714136*(Cr - 128)
//     B = Y + 1.772  *(Cb - 128)
//
// The 2D-DMA CSC unit evaluates  256 * Q = A*Y + B*Cb + C*Cr + D  with field
// widths A[9:0]/B[10:0]/C[9:0]/D[17:0] (all signed two's complement, see
// in_color_param_h/m/l_ch0 in ESP32-P4 TRM section 8.4). The IDF default
// (DMA2D_COLOR_SPACE_CONV_PARAM_YUV2RGB_BT601) bakes in the limited-range
// (Y_studio in [16,235]) 1.164*(Y-16) form, which under-saturates JPEG output
// because MJPEG is full-range. We keep using DMA2D_CSC_RX_YUV*_TO_RGB*_601 to
// drive input/output muxing + scramble setup, then overwrite the matrix here.
//
// Coefficients * 256 (rounded): 1.0->256, 1.402->359, 0.344136->88,
// 0.714136->183, 1.772->454. Offsets chosen so a neutral input
// (Y=0, Cb=128, Cr=128) maps to RGB=0 exactly: D_R = -359*128, D_G = (88+183)*128,
// D_B = -454*128.
static const int s_yuv2rgb_bt601_full_table[3][4] = {
    { 256,    0,   359,  -45952 },  // R: param_h
    { 256,  -88,  -183,   34688 },  // G: param_m
    { 256,  454,     0,  -58112 },  // B: param_l
};

static void s_dma_load_full_range_bt601_matrix(void)
{
    dma2d_dev_t *dev = DMA2D_LL_GET_HW(0);
    // Only RX channel 0 implements CSC (DMA2D_LL_RX_CHANNEL_SUPPORT_CSC_MASK = BIT0).
    volatile dma2d_color_param_group_chn_reg_t *grp = &dev->in_channel0.in_color_param_group;
    grp->param_h.a = s_yuv2rgb_bt601_full_table[0][0];
    grp->param_h.b = s_yuv2rgb_bt601_full_table[0][1];
    grp->param_h.c = s_yuv2rgb_bt601_full_table[0][2];
    grp->param_h.d = s_yuv2rgb_bt601_full_table[0][3];
    grp->param_m.a = s_yuv2rgb_bt601_full_table[1][0];
    grp->param_m.b = s_yuv2rgb_bt601_full_table[1][1];
    grp->param_m.c = s_yuv2rgb_bt601_full_table[1][2];
    grp->param_m.d = s_yuv2rgb_bt601_full_table[1][3];
    grp->param_l.a = s_yuv2rgb_bt601_full_table[2][0];
    grp->param_l.b = s_yuv2rgb_bt601_full_table[2][1];
    grp->param_l.c = s_yuv2rgb_bt601_full_table[2][2];
    grp->param_l.d = s_yuv2rgb_bt601_full_table[2][3];
}

static void s_dma_apply_csc(jpeg_strip_decoder_handle_t h, dma2d_channel_handle_t rx)
{
    dma2d_scramble_order_t post = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
    dma2d_csc_rx_option_t opt = DMA2D_CSC_RX_NONE;
    bool yuv_to_rgb_bt601 = false;

    if (h->engine->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB) {
        if (h->engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB565) post = DMA2D_SCRAMBLE_ORDER_BYTE2_0_1;
        else if (h->engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB888) post = DMA2D_SCRAMBLE_ORDER_BYTE0_1_2;
    }
    if (h->engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB565) {
        opt = (h->engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601)
              ? DMA2D_CSC_RX_YUV420_TO_RGB565_601 : DMA2D_CSC_RX_YUV420_TO_RGB565_709;
        yuv_to_rgb_bt601 = (h->engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601);
    } else if (h->engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB888) {
        opt = (h->engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601)
              ? DMA2D_CSC_RX_YUV420_TO_RGB888_601 : DMA2D_CSC_RX_YUV420_TO_RGB888_709;
        yuv_to_rgb_bt601 = (h->engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601);
    } else if (h->engine->output_format == JPEG_DECODE_OUT_FORMAT_YUV444) {
        if (h->engine->sample_method == JPEG_DOWN_SAMPLING_YUV422)      opt = DMA2D_CSC_RX_YUV422_TO_YUV444;
        else if (h->engine->sample_method == JPEG_DOWN_SAMPLING_YUV420) opt = DMA2D_CSC_RX_YUV420_TO_YUV444;
    }
    dma2d_csc_config_t cfg = { .post_scramble = post, .rx_csc_option = opt };
    dma2d_configure_color_space_conversion(rx, &cfg);
    // IDF just wrote the limited-range BT.601 matrix; clobber it with the
    // JFIF full-range form that matches MJPEG content.
    if (yuv_to_rgb_bt601) {
        s_dma_load_full_range_bt601_matrix();
    }
}

static bool s_on_job_picked(uint32_t num_chans, const dma2d_trans_channel_info_t *chans, void *uc)
{
    jpeg_strip_decoder_handle_t h = (jpeg_strip_decoder_handle_t)uc;
    jpeg_hal_context_t *hal = &h->engine->codec_base->hal;
    assert(num_chans == 2);
    dma2d_channel_handle_t tx = NULL, rx = NULL;
    for (uint32_t i = 0; i < num_chans; i++) {
        if (chans[i].dir == DMA2D_CHANNEL_DIRECTION_TX) tx = chans[i].chan;
        else rx = chans[i].chan;
    }
    h->rx_chan = rx;
    h->engine->dma2d_tx_channel = tx;
    h->engine->dma2d_rx_channel = rx;

    dma2d_trigger_t trig = { .periph = DMA2D_TRIG_PERIPH_JPEG_DECODER,
                             .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_JPEG_TX };
    dma2d_connect(tx, &trig);
    trig.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_JPEG_RX;
    dma2d_connect(rx, &trig);

    s_dma_apply_jpeg_transfer_ability(h, tx, rx);
    s_dma_apply_csc(h, rx);

    // No owner_check: rely on the pipeline being naturally balanced (PPA
    // reading from SRAM should always be faster per-strip than JPEG producing
    // a strip), so DMA never overwrites a still-in-use SRAM buffer. If this
    // assumption breaks we'd see tearing, not corruption.

    dma2d_rx_event_callbacks_t cbs = {
        .on_recv_eof = s_on_recv_eof,
        .on_desc_done = s_on_desc_done,
        .on_desc_empty = NULL,
    };
    dma2d_register_rx_event_callbacks(rx, &cbs, h);

    dma2d_set_desc_addr(tx, (intptr_t)h->txlink);
    dma2d_set_desc_addr(rx, (intptr_t)h->rxlinks);
    dma2d_start(tx);
    dma2d_start(rx);
    jpeg_ll_process_start(hal->dev);
    return false;
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t jpeg_strip_decoder_new(const jpeg_strip_decode_cfg_t *cfg, jpeg_strip_decoder_handle_t *out_handle)
{
    if (!cfg || !out_handle || !cfg->strip_buffers || cfg->ring_count == 0) return ESP_ERR_INVALID_ARG;
    if (cfg->pic_h == 0 || cfg->strip_h == 0 || (cfg->pic_h % cfg->strip_h) != 0) return ESP_ERR_INVALID_ARG;

    jpeg_strip_decoder_handle_t h = heap_caps_calloc(1, sizeof(*h), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h) return ESP_ERR_NO_MEM;
    h->cfg = *cfg;
    h->strip_count = cfg->pic_h / cfg->strip_h;
    h->spin = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 200 };
    esp_err_t err = jpeg_new_decoder_engine(&ecfg, &h->engine);
    if (err != ESP_OK) goto fail;
    h->engine->output_format = cfg->base_cfg.output_format;
    h->engine->rgb_order = cfg->base_cfg.rgb_order;
    h->engine->conv_std = cfg->base_cfg.conv_std;

    uint32_t cache_line = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);
    h->desc_byte_size = JPEG_ALIGN_UP(sizeof(dma2d_descriptor_t), cache_line);
    h->rxlinks = heap_caps_aligned_calloc(cache_line, h->strip_count, h->desc_byte_size,
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    h->txlink  = heap_caps_aligned_calloc(cache_line, 1,             h->desc_byte_size,
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h->rxlinks || !h->txlink) { err = ESP_ERR_NO_MEM; goto fail; }

    h->frame_done_sem = xSemaphoreCreateBinary();
    if (!h->frame_done_sem) { err = ESP_ERR_NO_MEM; goto fail; }

    *out_handle = h;
    return ESP_OK;
fail:
    jpeg_strip_decoder_del(h);
    return err;
}

esp_err_t jpeg_strip_decoder_del(jpeg_strip_decoder_handle_t h)
{
    if (!h) return ESP_OK;
    if (h->rxlinks) free(h->rxlinks);
    if (h->txlink)  free(h->txlink);
    if (h->frame_done_sem) vSemaphoreDelete(h->frame_done_sem);
    if (h->engine) jpeg_del_decoder_engine(h->engine);
    free(h);
    return ESP_OK;
}

esp_err_t jpeg_strip_decoder_process(jpeg_strip_decoder_handle_t h, const uint8_t *bit_stream, uint32_t stream_size)
{
    if (!h || !bit_stream || !stream_size) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_OK;
    if (h->engine->codec_base->pm_lock) esp_pm_lock_acquire(h->engine->codec_base->pm_lock);
    xSemaphoreTake(h->engine->codec_base->codec_mutex, portMAX_DELAY);
    xQueueReset(h->engine->evt_queue);
    // Drain any stale semaphore from a previous error path
    xSemaphoreTake(h->frame_done_sem, 0);

    err = s_parse_jpeg(h->engine, bit_stream, stream_size);
    if (err != ESP_OK) goto out;
    err = s_apply_header_to_hw(h->engine);
    if (err != ESP_OK) goto out;

    // Validate picture matches what we sized our descriptor chain for
    if (h->engine->header_info->process_h != h->cfg.pic_w ||
        h->engine->header_info->process_v != h->cfg.pic_h) {
        ESP_LOGE(TAG, "picture %lux%lu mismatches config %lux%lu",
                 (unsigned long)h->engine->header_info->process_h,
                 (unsigned long)h->engine->header_info->process_v,
                 (unsigned long)h->cfg.pic_w, (unsigned long)h->cfg.pic_h);
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    s_init_descriptors(h);

    // Sync input from cache to PSRAM so DMA reads the right bytes
    esp_cache_msync((void *)h->engine->header_info->buffer_offset,
                    h->engine->header_info->buffer_left,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    // The strip buffers live in internal RAM so no cache invalidate is needed.

    dma2d_trans_config_t trans = {
        .tx_channel_num = 1,
        .rx_channel_num = 1,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_RX_REORDER,
        .user_config = h,
        .on_job_picked = s_on_job_picked,
    };
    err = dma2d_enqueue(h->engine->dma2d_group_handle, &trans, h->engine->trans_desc);
    if (err != ESP_OK) goto out;

    // Wait for the JPEG event queue: either the JPEG hardware reports an
    // error, or RX EOF lands (in which case s_on_recv_eof has released
    // frame_done_sem).
    while (1) {
        jpeg_dma2d_dec_evt_t evt;
        BaseType_t r = xQueueReceive(h->engine->evt_queue, &evt, h->engine->timeout_tick);
        if (r != pdTRUE) {
            ESP_LOGE(TAG, "decode timeout");
            err = ESP_ERR_TIMEOUT;
            bool ny;
            dma2d_force_end(h->engine->trans_desc, &ny);
            goto out;
        }
        if (evt.jpgd_status != 0) {
            ESP_LOGE(TAG, "decode jpgd_status=0x%lx", (unsigned long)evt.jpgd_status);
            err = ESP_ERR_INVALID_STATE;
            bool ny;
            dma2d_force_end(h->engine->trans_desc, &ny);
            goto out;
        }
        if (evt.dma_evt & JPEG_DMA2D_RX_EOF) break;
    }
    // Confirm strip-callback for the final strip has fired and frame_done_sem
    // is signaled; in the on_recv_eof path it's signalled before the JPEG
    // event is observed via the codec ISR, so this take should be immediate.
    xSemaphoreTake(h->frame_done_sem, h->engine->timeout_tick);

out:
    xSemaphoreGive(h->engine->codec_base->codec_mutex);
    if (h->engine->codec_base->pm_lock) esp_pm_lock_release(h->engine->codec_base->pm_lock);
    return err;
}

esp_err_t jpeg_strip_decoder_release_strip(jpeg_strip_decoder_handle_t h, uint32_t strip_idx)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    // Strip strip_idx used buffer (strip_idx % ring_count). The next time we
    // need to write to that buffer is at descriptor (strip_idx + ring_count).
    // Make that descriptor part of the DMA chain now (it's safe — the
    // SRAM buffer is no longer being read by PPA).
    uint32_t new_tail = strip_idx + h->cfg.ring_count;
    if (new_tail >= h->strip_count) {
        // No later use of this slot in this frame, nothing to splice.
        return ESP_OK;
    }
    dma2d_descriptor_t *prev = (dma2d_descriptor_t *)((uint8_t *)h->rxlinks + (new_tail - 1) * h->desc_byte_size);
    dma2d_descriptor_t *next = (dma2d_descriptor_t *)((uint8_t *)h->rxlinks + new_tail * h->desc_byte_size);

    portENTER_CRITICAL_SAFE(&h->spin);
    // (new_tail - 1) used to be the tail (with .next=NULL). Link it to new_tail.
    prev->next = next;
    next->next = NULL;
    h->chain_tail = new_tail;
    portEXIT_CRITICAL_SAFE(&h->spin);

    // Make sure the descriptor writes are visible to the DMA before we kick it.
    esp_cache_msync(prev, h->desc_byte_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cache_msync(next, h->desc_byte_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (h->rx_chan) dma2d_append(h->rx_chan);
    return ESP_OK;
}
