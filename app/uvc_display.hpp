#pragma once

void uvc_display_app();

// Composite the LVGL surface onto `out_fb` (720x1280 RGB888 panel buffer)
// when the GUI is currently visible. No-op when hidden.
void gui_compose(void *out_fb);
bool gui_is_visible();
