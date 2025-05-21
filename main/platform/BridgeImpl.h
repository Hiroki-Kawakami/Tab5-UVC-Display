#include "sdkconfig.h"
#include "freertos/idf_additions.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "usb/uac_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

void esp_log_write_str(esp_log_level_t level, const char *tag, const char *str) {
    esp_log_write(level, tag, "%s", str);
}

