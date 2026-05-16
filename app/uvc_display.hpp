#pragma once

#include <cstdint>

// Panel-space extent of the LVGL strip (placed at panel x∈[0,GUI_PANEL_W),
// y∈[0,GUI_PANEL_H)). Used by the renderer to clip the UVC pipeline so it
// doesn't render the band the GUI will overwrite.
constexpr uint32_t GUI_PANEL_W = 720;
constexpr uint32_t GUI_PANEL_H = 480;

void uvc_display_app();

// Composite the LVGL surface onto `out_fb` (720x1280 RGB888 panel buffer).
// Unconditional: the caller decides when to call (typically gated on
// gui_is_visible()). The LVGL mutex is held during PPA to keep gui_fb stable.
void gui_compose(void *out_fb);
bool gui_is_visible();
void gui_set_visible(bool visible);
