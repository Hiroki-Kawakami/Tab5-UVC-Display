#include "uvc_display.hpp"
#include "platform_port.hpp"
#include "lvgl.hpp"
#include "preview_screen.hpp"
#include "esp_lvgl_port.h"
#include "nvs.hpp"

#define GUI_WIDTH        320
#define GUI_HEIGHT       480
#define GUI_SCALE        1.5f

static_assert(GUI_HEIGHT * GUI_SCALE == GUI_PANEL_W, "GUI_PANEL_W mismatch");
static_assert(GUI_WIDTH  * GUI_SCALE == GUI_PANEL_H, "GUI_PANEL_H mismatch");

static uint16_t *gui_fb;
static pf_port::SRMClient *gui_srm;
static volatile bool gui_visible = true;

bool gui_is_visible() {
    return gui_visible;
}

void gui_set_visible(bool visible) {
    gui_visible = visible;
}

void gui_compose(void *out_fb) {
    // Hold the LVGL mutex so the lvgl_port task can't render into gui_fb
    // mid-PPA. PPA itself is blocking so the lock is released as soon as
    // the strip is fully consumed.
    if (lvgl_port_lock(0)) {
        gui_srm->do_scale_rotate_mirror(gui_fb, out_fb);
        lvgl_port_unlock();
    }
}

static void lvgl_setup() {
    gui_fb = (uint16_t*)pf_port::psram_malloc(GUI_WIDTH * GUI_HEIGHT * 2);
    gui_srm = new pf_port::SRMClient();
    gui_srm->setInputBlock(GUI_WIDTH, GUI_HEIGHT, GUI_WIDTH, GUI_HEIGHT, 0, 0, pf_port::PixelFormat::RGB565);
    gui_srm->setOutputBlock(720, 1280, 0, 0, pf_port::PixelFormat::RGB888);
    gui_srm->setRotation(90);
    gui_srm->setScale(GUI_SCALE, GUI_SCALE);

    auto disp = lv_display_create(GUI_WIDTH, GUI_HEIGHT);
    lv_display_set_buffers(disp, gui_fb, NULL, GUI_WIDTH * GUI_HEIGHT * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    // The renderer task pulls gui_fb via gui_compose() each UVC frame, so
    // the flush_cb only needs to acknowledge the render.
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
        lv_display_flush_ready(disp);
    });

    auto indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, NULL);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data){
        // A press whose sole purpose is to toggle visibility (show/hide) is
        // "consumed" so that the same press doesn't also click a widget.
        static bool prev_pressed = false;
        static bool press_consumed = false;

        auto touch = pf_port::touch_get_point();
        bool pressed = touch.has_value();
        int px = pressed ? std::get<0>(touch.value()) : 0;
        int py = pressed ? std::get<1>(touch.value()) : 0;
        bool in_region = pressed && px < GUI_PANEL_W && py < GUI_PANEL_H;

        if (pressed && !prev_pressed) {
            if (!gui_visible) {
                gui_visible = true;
                press_consumed = true;
            } else if (!in_region) {
                gui_visible = false;
                press_consumed = true;
            } else {
                press_consumed = false;
            }
        }
        if (!pressed) press_consumed = false;
        prev_pressed = pressed;

        if (pressed && !press_consumed && gui_visible && in_region) {
            // PPA ANGLE_90 is CCW per-block (camera/LVGL top-left lands at
            // panel bottom-left, etc.). With GUI_SCALE applied:
            //   px = ly * GUI_SCALE
            //   py = (GUI_WIDTH - 1 - lx) * GUI_SCALE
            // Inverse:
            int ly = (int)(px / GUI_SCALE);
            int lx = (GUI_WIDTH - 1) - (int)(py / GUI_SCALE);
            if (lx < 0) lx = 0;
            if (lx >= GUI_WIDTH) lx = GUI_WIDTH - 1;
            if (ly < 0) ly = 0;
            if (ly >= GUI_HEIGHT) ly = GUI_HEIGHT - 1;
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = lx;
            data->point.y = ly;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

void uvc_display_app() {
    NVS::init();
    pf_port::init(3, pf_port::PixelFormat::RGB565);
    lvgl_setup();
    lv_async_call([](){
        screen_manager.push(std::make_unique<PreviewScreen>());
    });
    pf_port::display_set_brightness(50);
}
