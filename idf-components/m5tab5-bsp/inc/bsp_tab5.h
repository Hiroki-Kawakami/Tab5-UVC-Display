/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_common.h"
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
} bsp_tab5_config_t;

esp_err_t bsp_tab5_init(const bsp_tab5_config_t *config);
void bsp_tab5_display_set_brightness(int brightness);
void *bsp_tab5_display_get_frame_buffer(int fb_index);
void bsp_tab5_display_flush(int fb_index);
int bsp_tab5_touch_read(esp_lcd_touch_point_data_t *points, uint8_t max_points);
void bsp_tab5_touch_wait_interrupt(void);

#ifdef __cplusplus
}
#endif
