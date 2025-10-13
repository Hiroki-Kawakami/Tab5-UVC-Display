#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "ili9881_init_data.h"
#include "esp_lcd_touch_gt911.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "usb/uac_host.h"

esp_err_t esp_lcd_dpi_panel_get_frame_buffers(esp_lcd_panel_handle_t panel, void **fb0, void **fb1) {
    return esp_lcd_dpi_panel_get_frame_buffer(panel, 2, fb0, fb1);
}

/*
 * Default Config Bridge
 */
// GT911
void _ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(esp_lcd_panel_io_i2c_config_t *ptr) {
    *ptr = (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
}
