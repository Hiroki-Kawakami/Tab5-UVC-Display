#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "ili9881_init_data.h"

esp_err_t esp_lcd_dpi_panel_get_first_frame_buffer(esp_lcd_panel_handle_t panel, void **fb0) {
    return esp_lcd_dpi_panel_get_frame_buffer(panel, 1, fb0);
}
