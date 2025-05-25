#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
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
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

void esp_log_write_str(esp_log_level_t level, const char *tag, const char *str) {
    esp_log_write(level, tag, "%s", str);
}

/*
 * Default Config Bridge
 */
// I2S
void _I2S_STD_CLK_DEFAULT_CONFIG(i2s_std_clk_config_t *ptr, uint32_t rate) {
    *ptr = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(rate);
}
void _I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(i2s_std_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo) {
    *ptr = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo);
}
void _I2S_STD_PCM_SLOT_DEFAULT_CONFIG(i2s_std_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo) {
    *ptr = (i2s_std_slot_config_t)I2S_STD_PCM_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo);
}
void _I2S_STD_MSB_SLOT_DEFAULT_CONFIG(i2s_std_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo) {
    *ptr = (i2s_std_slot_config_t)I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo);
}
void _I2S_TDM_CLK_DEFAULT_CONFIG(i2s_tdm_clk_config_t *ptr, uint32_t rate) {
    *ptr = (i2s_tdm_clk_config_t)I2S_TDM_CLK_DEFAULT_CONFIG(rate);
}
void _I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(i2s_tdm_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo, i2s_tdm_slot_mask_t mask) {
    *ptr = (i2s_tdm_slot_config_t)I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo, mask);
}
void _I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(i2s_tdm_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo, i2s_tdm_slot_mask_t mask) {
    *ptr = (i2s_tdm_slot_config_t)I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo, mask);
}
void _I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(i2s_tdm_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo, i2s_tdm_slot_mask_t mask) {
    *ptr = (i2s_tdm_slot_config_t)I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo, mask);
}
void _I2S_TDM_PCM_LONG_SLOT_DEFAULT_CONFIG(i2s_tdm_slot_config_t *ptr, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t mono_or_stereo, i2s_tdm_slot_mask_t mask) {
    *ptr = (i2s_tdm_slot_config_t)I2S_TDM_PCM_LONG_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo, mask);
}
// SDMMC
void _SDMMC_HOST_DEFAULT(sdmmc_host_t *ptr) {
    *ptr = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
}
void _SDMMC_SLOT_CONFIG_DEFAULT(sdmmc_slot_config_t *ptr) {
    *ptr = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
}

// stdio
FILE *_STDOUT() { return stdout; }
