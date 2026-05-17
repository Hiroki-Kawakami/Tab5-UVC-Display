/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_eq.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define EQ_DEFAULT_MAX_STAGES 8

typedef struct {
    float z1, z2;
} biquad_state_t;

struct audio_eq_state {
    SemaphoreHandle_t mutex;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    size_t   max_stages;
    size_t   num_stages;
    audio_eq_biquad_t *biquads;        /* [max_stages] */
    biquad_state_t    *states;         /* [max_stages * channels] */
    bool enabled;
};

static inline float biquad_step(const audio_eq_biquad_t *b, biquad_state_t *s, float x) {
    float y = b->b0 * x + s->z1;
    s->z1 = b->b1 * x - b->a1 * y + s->z2;
    s->z2 = b->b2 * x - b->a2 * y;
    return y;
}

static void reset_states(audio_eq_t eq) {
    if (eq->states) {
        memset(eq->states, 0, eq->max_stages * eq->channels * sizeof(biquad_state_t));
    }
}

esp_err_t audio_eq_init(const audio_eq_config_t *config, audio_eq_t *out_eq) {
    if (!config || !out_eq) return ESP_ERR_INVALID_ARG;
    if (config->channels < 1 || config->channels > 2) return ESP_ERR_INVALID_ARG;
    if (config->bits_per_sample != 16) return ESP_ERR_INVALID_ARG;
    if (!config->sample_rate) return ESP_ERR_INVALID_ARG;

    size_t initial_n = config->initial_biquads ? config->initial_num_stages : 0;
    size_t max_stages = config->max_stages;
    if (!max_stages) {
        max_stages = initial_n > EQ_DEFAULT_MAX_STAGES ? initial_n : EQ_DEFAULT_MAX_STAGES;
    }
    if (initial_n > max_stages) return ESP_ERR_INVALID_ARG;

    audio_eq_t eq = calloc(1, sizeof(*eq));
    if (!eq) return ESP_ERR_NO_MEM;

    eq->mutex = xSemaphoreCreateMutex();
    eq->biquads = calloc(max_stages, sizeof(audio_eq_biquad_t));
    eq->states  = calloc(max_stages * config->channels, sizeof(biquad_state_t));
    if (!eq->mutex || !eq->biquads || !eq->states) {
        audio_eq_deinit(eq);
        return ESP_ERR_NO_MEM;
    }

    eq->sample_rate     = config->sample_rate;
    eq->channels        = config->channels;
    eq->bits_per_sample = config->bits_per_sample;
    eq->max_stages      = max_stages;
    eq->num_stages      = initial_n;
    eq->enabled         = config->enabled;
    if (initial_n) memcpy(eq->biquads, config->initial_biquads, initial_n * sizeof(audio_eq_biquad_t));

    *out_eq = eq;
    return ESP_OK;
}

esp_err_t audio_eq_deinit(audio_eq_t eq) {
    if (!eq) return ESP_ERR_INVALID_ARG;
    if (eq->mutex) vSemaphoreDelete(eq->mutex);
    free(eq->biquads);
    free(eq->states);
    free(eq);
    return ESP_OK;
}

esp_err_t audio_eq_set_biquads(audio_eq_t eq, const audio_eq_biquad_t *biquads, size_t num_stages) {
    if (!eq) return ESP_ERR_INVALID_ARG;
    if (num_stages > eq->max_stages) return ESP_ERR_INVALID_SIZE;
    if (num_stages && !biquads) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(eq->mutex, portMAX_DELAY);
    if (num_stages) memcpy(eq->biquads, biquads, num_stages * sizeof(audio_eq_biquad_t));
    eq->num_stages = num_stages;
    reset_states(eq);
    xSemaphoreGive(eq->mutex);
    return ESP_OK;
}

esp_err_t audio_eq_set_enabled(audio_eq_t eq, bool enabled) {
    if (!eq) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(eq->mutex, portMAX_DELAY);
    if (enabled && !eq->enabled) reset_states(eq);
    eq->enabled = enabled;
    xSemaphoreGive(eq->mutex);
    return ESP_OK;
}

bool audio_eq_is_enabled(audio_eq_t eq) {
    return eq && eq->enabled;
}

esp_err_t audio_eq_reconfig(audio_eq_t eq, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    if (!eq) return ESP_ERR_INVALID_ARG;
    if (channels < 1 || channels > 2) return ESP_ERR_INVALID_ARG;
    if (bits_per_sample != 16) return ESP_ERR_INVALID_ARG;
    if (!sample_rate) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(eq->mutex, portMAX_DELAY);
    if (channels != eq->channels) {
        biquad_state_t *new_states = calloc(eq->max_stages * channels, sizeof(biquad_state_t));
        if (!new_states) {
            xSemaphoreGive(eq->mutex);
            return ESP_ERR_NO_MEM;
        }
        free(eq->states);
        eq->states = new_states;
    }
    eq->sample_rate     = sample_rate;
    eq->channels        = channels;
    eq->bits_per_sample = bits_per_sample;
    reset_states(eq);
    xSemaphoreGive(eq->mutex);
    return ESP_OK;
}

esp_err_t audio_eq_process(audio_eq_t eq, void *data, size_t bytes) {
    if (!eq || !data) return ESP_ERR_INVALID_ARG;
    if (!eq->enabled || !eq->num_stages) return ESP_OK;

    const size_t frame_bytes = sizeof(int16_t) * eq->channels;
    if (bytes % frame_bytes) return ESP_ERR_INVALID_SIZE;
    const size_t frames = bytes / frame_bytes;

    xSemaphoreTake(eq->mutex, portMAX_DELAY);

    const size_t n_stages = eq->num_stages;
    const uint8_t channels = eq->channels;
    const audio_eq_biquad_t *biquads = eq->biquads;
    biquad_state_t *states = eq->states;

    int16_t *p = (int16_t *)data;
    for (size_t f = 0; f < frames; f++) {
        for (uint8_t ch = 0; ch < channels; ch++) {
            float x = (float)p[f * channels + ch];
            for (size_t s = 0; s < n_stages; s++) {
                x = biquad_step(&biquads[s], &states[s * channels + ch], x);
            }
            if (x >  32767.0f) x =  32767.0f;
            if (x < -32768.0f) x = -32768.0f;
            p[f * channels + ch] = (int16_t)lrintf(x);
        }
    }

    xSemaphoreGive(eq->mutex);
    return ESP_OK;
}

/* ---------------- RBJ Cookbook designers ---------------- */

static inline audio_eq_biquad_t normalize(float b0, float b1, float b2, float a0, float a1, float a2) {
    float inv = 1.0f / a0;
    return (audio_eq_biquad_t){
        .b0 = b0 * inv, .b1 = b1 * inv, .b2 = b2 * inv,
        .a1 = a1 * inv, .a2 = a2 * inv,
    };
}

audio_eq_biquad_t audio_eq_design_peaking(uint32_t fs, float f0, float q, float gain_db) {
    float A      = powf(10.0f, gain_db / 40.0f);
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    return normalize(
        1.0f + alpha * A,
       -2.0f * cos_w0,
        1.0f - alpha * A,
        1.0f + alpha / A,
       -2.0f * cos_w0,
        1.0f - alpha / A);
}

audio_eq_biquad_t audio_eq_design_low_shelf(uint32_t fs, float f0, float q, float gain_db) {
    float A      = powf(10.0f, gain_db / 40.0f);
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float beta   = 2.0f * sqrtf(A) * alpha;
    return normalize(
            A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + beta),
     2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0),
            A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - beta),
                (A + 1.0f) + (A - 1.0f) * cos_w0 + beta,
        -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0),
                (A + 1.0f) + (A - 1.0f) * cos_w0 - beta);
}

audio_eq_biquad_t audio_eq_design_high_shelf(uint32_t fs, float f0, float q, float gain_db) {
    float A      = powf(10.0f, gain_db / 40.0f);
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float beta   = 2.0f * sqrtf(A) * alpha;
    return normalize(
             A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + beta),
     -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0),
             A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - beta),
                 (A + 1.0f) - (A - 1.0f) * cos_w0 + beta,
         2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0),
                 (A + 1.0f) - (A - 1.0f) * cos_w0 - beta);
}

audio_eq_biquad_t audio_eq_design_lowpass(uint32_t fs, float f0, float q) {
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float k      = (1.0f - cos_w0) * 0.5f;
    return normalize(
        k, 2.0f * k, k,
        1.0f + alpha, -2.0f * cos_w0, 1.0f - alpha);
}

audio_eq_biquad_t audio_eq_design_highpass(uint32_t fs, float f0, float q) {
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float k      = (1.0f + cos_w0) * 0.5f;
    return normalize(
        k, -2.0f * k, k,
        1.0f + alpha, -2.0f * cos_w0, 1.0f - alpha);
}
