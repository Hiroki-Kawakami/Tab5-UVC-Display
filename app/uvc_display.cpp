#include "uvc_display.hpp"
#include "platform_port.hpp"
#include "lvgl.hpp"
#include "preview_screen.hpp"

#define GUI_WIDTH  360
#define GUI_HEIGHT 640

static uint16_t *gui_fb;
static pf_port::SRMClient *gui_srm;

static void lvgl_setup() {
    gui_fb = (uint16_t*)pf_port::psram_malloc(GUI_WIDTH * GUI_HEIGHT * 2);
    gui_srm = new pf_port::SRMClient();
    gui_srm->setInputBlock(GUI_WIDTH, GUI_HEIGHT, GUI_WIDTH, GUI_HEIGHT, 0, 0, pf_port::PixelFormat::RGB565);
    gui_srm->setOutputBlock(720, 1280, 0, 0, pf_port::PixelFormat::RGB888);
    gui_srm->setScale(2, 2);

    auto disp = lv_display_create(GUI_WIDTH, GUI_HEIGHT);
    lv_display_set_buffers(disp, gui_fb, NULL, GUI_WIDTH * GUI_HEIGHT * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
        gui_srm->do_scale_rotate_mirror(gui_fb, pf_port::display_get_frame_buffer(0));
        pf_port::display_flush(0);
        lv_display_flush_ready(disp);
    });

    auto indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, NULL);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data){
        auto touch = pf_port::touch_get_point();
        if (touch.has_value()) {
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = std::get<0>(touch.value()) / 2;
            data->point.y = std::get<1>(touch.value()) / 2;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

void uvc_display_app() {
    pf_port::init(2, pf_port::PixelFormat::RGB565);
    lvgl_setup();
    lv_async_call([](){
        screen_manager.push(std::make_unique<PreviewScreen>());
    });
    pf_port::display_set_brightness(80);
}
