#include "preview_screen.hpp"
#include "platform_port.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cassert>

static const char *TAG = "preview";

namespace {

constexpr int STREAM_W = 1280;
constexpr int STREAM_H = 720;
constexpr int DISP_W = 720;
constexpr int DISP_H = 1280;
constexpr int BPP = 2;
constexpr int DECODE_BUF_COUNT = 3;
constexpr int DISPLAY_FB_COUNT = 2;

usb_host::UVC uvc;
pf_port::JpegDecoder *jpeg_decoder;
pf_port::SRMClient *ppa;
uint8_t *decode_buffers[DECODE_BUF_COUNT];
QueueHandle_t frame_queue;
QueueHandle_t renderer_queue;

void decoder_task(void *) {
    int idx = 0;
    while (true) {
        const uvc_host_frame_t *frame;
        if (xQueueReceive(frame_queue, &frame, portMAX_DELAY) != pdTRUE) continue;

        size_t out_size = 0;
        auto err = jpeg_decoder->decode(
            frame->data, frame->data_len,
            decode_buffers[idx], STREAM_W * STREAM_H * BPP, &out_size
        );
        uvc.returnFrame(frame);

        if (err == pf_port::Error::Ok) {
            uint8_t *buf = decode_buffers[idx];
            xQueueSend(renderer_queue, &buf, 0);
            idx = (idx + 1) % DECODE_BUF_COUNT;
        } else {
            ESP_LOGW(TAG, "jpeg decode failed");
        }
    }
}

void renderer_task(void *) {
    ppa->setInputBlock(STREAM_W, STREAM_H, STREAM_W, STREAM_H, 0, 0, pf_port::PixelFormat::RGB565);
    ppa->setOutputBlock(DISP_W, DISP_H, 0, 0, pf_port::PixelFormat::RGB565);
    ppa->setRotation(90);
    ppa->setScale(1.0f, 1.0f);

    int fb_index = 0;
    int frame_count = 0;
    int64_t fps_start = esp_timer_get_time();
    while (true) {
        uint8_t *decode_buf;
        if (xQueueReceive(renderer_queue, &decode_buf, portMAX_DELAY) != pdTRUE) continue;
        int next = (fb_index + 1) % DISPLAY_FB_COUNT;
        ppa->do_scale_rotate_mirror(decode_buf, pf_port::display_get_frame_buffer(next));
        pf_port::display_flush(next);
        fb_index = next;

        frame_count++;
        int64_t now = esp_timer_get_time();
        if (now - fps_start >= 1000000) {
            ESP_LOGI(TAG, "%dfps", frame_count);
            frame_count = 0;
            fps_start = now;
        }
    }
}

} // namespace

void PreviewScreen::build() {
}

void PreviewScreen::onEnter() {
    usb_host::install();
    uvc.install();
    uvc.setCallback(this);

    jpeg_decoder = new pf_port::JpegDecoder();
    jpeg_decoder->setOutputFormat(pf_port::PixelFormat::RGB565);

    ppa = new pf_port::SRMClient();

    jpeg_decode_memory_alloc_cfg_t mem_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t allocated = 0;
    for (int i = 0; i < DECODE_BUF_COUNT; i++) {
        decode_buffers[i] = (uint8_t*)jpeg_alloc_decoder_mem(STREAM_W * STREAM_H * BPP, &mem_cfg, &allocated);
        assert(decode_buffers[i]);
    }

    frame_queue = xQueueCreate(1, sizeof(uvc_host_frame_t*));
    renderer_queue = xQueueCreate(DECODE_BUF_COUNT, sizeof(uint8_t*));

    xTaskCreatePinnedToCore(decoder_task, "decoder", 4096, nullptr, 15, nullptr, 0);
    xTaskCreatePinnedToCore(renderer_task, "renderer", 4096, nullptr, 16, nullptr, 0);

    xTaskCreatePinnedToCore([](void*){
        while (uvc.open(STREAM_W, STREAM_H, 30) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        uvc.start();
        vTaskDelete(NULL);
    }, "uvc_open", 4096, nullptr, 5, nullptr, 0);
}

void PreviewScreen::onEvent(const uvc_host_stream_event_data_t *event) {
}

bool PreviewScreen::onFrame(const uvc_host_frame_t *frame) {
    if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
        return true;
    }
    return false;
}
