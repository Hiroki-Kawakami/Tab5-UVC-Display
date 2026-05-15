#include "preview_screen.hpp"

usb_host::UVC uvc;

void PreviewScreen::build() {
    auto button = lv_button_create(root_);
    lv_obj_center(button);
    lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [](lv_event_t *e){
        uvc.open(1280, 720, 30);
    });
    auto label = lv_label_create(button);
    lv_label_set_text(label, "Open");
    lv_obj_center(label);
}

void PreviewScreen::onEnter() {
    usb_host::install();
    uvc.install();
}

void PreviewScreen::onEvent(const uvc_host_stream_event_data_t *event) {

}

bool PreviewScreen::onFrame(const uvc_host_frame_t *frame) {
    return true;
}
