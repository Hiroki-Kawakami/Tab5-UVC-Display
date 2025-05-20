#include "sdkconfig.h"
#include "freertos/idf_additions.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

void esp_log_write_str(esp_log_level_t level, const char *tag, const char *str) {
    esp_log_write(level, tag, str);
}
