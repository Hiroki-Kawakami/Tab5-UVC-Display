/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_common.h"
#include "audio_eq.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    struct {
        uint8_t fb_num;
        bsp_pixel_format_t pixel_format;
    } display;
    struct {
        bool interrupt;
    } touch;
    struct {
        bool usb5v_en;
    } usb;
    struct {
        bsp_wifi_mode_t mode;
    } wifi;
    struct {
        bool enable;
    } bluetooth;
    struct {
        bool disable;            /*!< Skip audio codec init (default: enabled) */
        uint32_t sample_rate;    /*!< 0 -> 48000 */
        uint8_t bits_per_sample; /*!< 0 -> 16 */
        uint8_t channels;        /*!< 0 -> 2 */
        struct {
            bool enable;                          /*!< Enable EQ at boot */
            size_t num_stages;                    /*!< Number of biquads in `biquads` */
            const audio_eq_biquad_t *biquads;     /*!< Initial coefficients (copied) */
            size_t max_stages;                    /*!< Capacity; 0 -> max(num_stages, 8) */
        } eq;
    } audio;
} bsp_tab5_config_t;

esp_err_t bsp_tab5_init(const bsp_tab5_config_t *config);
void bsp_tab5_display_set_brightness(int brightness);
void *bsp_tab5_display_get_frame_buffer(int fb_index);
void bsp_tab5_display_flush(int fb_index);
int bsp_tab5_touch_read(esp_lcd_touch_point_data_t *points, uint8_t max_points);
void bsp_tab5_touch_wait_interrupt(void);

// MARK: Audio (speaker output via ES8388)
esp_err_t bsp_tab5_audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t bsp_tab5_audio_close(void);
esp_err_t bsp_tab5_audio_reconfig(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
/* When EQ is enabled, `data` is filtered in-place — caller must own the buffer. */
esp_err_t bsp_tab5_audio_write(void *data, size_t len);
esp_err_t bsp_tab5_audio_set_volume(int volume);   /*!< 0..100, 0 mutes */
esp_err_t bsp_tab5_audio_set_mute(bool mute);
int       bsp_tab5_audio_get_volume(void);

/* Speaker EQ — applied in-place inside bsp_tab5_audio_write. */
esp_err_t bsp_tab5_audio_eq_set_enabled(bool enabled);
bool      bsp_tab5_audio_eq_is_enabled(void);
esp_err_t bsp_tab5_audio_eq_set_biquads(const audio_eq_biquad_t *biquads, size_t num_stages);
audio_eq_t bsp_tab5_audio_eq_handle(void);  /*!< NULL if EQ was not initialised */

#ifdef __cplusplus
}
#endif
