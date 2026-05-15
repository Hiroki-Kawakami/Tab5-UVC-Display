#include <cstdint>
#include <atomic>
#include <vector>
#include <tuple>
#include <optional>
#include <assert.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "lvgl.hpp"
#include "uvc_display.hpp"
#include "platform_port.hpp"

#define LCD_WIDTH    (720)
#define LCD_HEIGHT   (1280)
#define FB_COUNT_MAX (3)

static std::vector<void*> fb_pool;
static std::atomic<int> fb_index = 0;
static SDL_Window *sdl_window;
static SDL_Renderer *sdl_renderer;
static SDL_Texture *sdl_texture;
static int window_rotation = 0;
static int drawable_w, drawable_h;
static int pixel_format_size;
static lv_timer_t *event_handler_timer;
static std::optional<std::tuple<int, int>> mouse;
static bool app_running;

static void sdl_mouse_event_handler(SDL_Event *event) {
    auto convert = [](const SDL_MouseMotionEvent &motion){
        int win_w, win_h, x = 0, y = 0;
        SDL_GetWindowSize(sdl_window, &win_w, &win_h);
        if (window_rotation == 0) {
            x = ((double)motion.x / (double)win_w) * 720;
            y = ((double)motion.y / (double)win_h) * 1280;
        } else if (window_rotation == 90) {
            x = ((double)motion.y / (double)win_h) * 720;
            y = (1 - (double)motion.x / (double)win_w) * 1280;
        } else if (window_rotation == 180) {
            x = (1 - (double)motion.x / (double)win_w) * 720;
            y = (1 - (double)motion.y / (double)win_h) * 1280;
        } else if (window_rotation == 270) {
            x = (1 - (double)motion.y / (double)win_h) * 720;
            y = ((double)motion.x / (double)win_w) * 1280;
        } else {
            assert(0);
        }
        return std::make_tuple(x, y);
    };
    switch(event->type) {
    case SDL_WINDOWEVENT:
        if(event->window.event == SDL_WINDOWEVENT_LEAVE) {
            mouse = std::nullopt;
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if(event->button.button == SDL_BUTTON_LEFT)
            mouse = std::nullopt;
        break;
    case SDL_WINDOWEVENT_LEAVE:
        mouse = std::nullopt;
        break;
    case SDL_MOUSEBUTTONDOWN:
        if(event->button.button == SDL_BUTTON_LEFT) {
            mouse = convert(event->motion);
        }
        break;
    case SDL_MOUSEMOTION:
        if (mouse.has_value()) {
            mouse = convert(event->motion);
        }
        break;
    }
}

static void sdl_keyboard_event_handler(SDL_Event *event) {
    auto rotate_window = [](int angle){
        window_rotation = (window_rotation + angle + 360) % 360;
        int win_w = (window_rotation == 90 || window_rotation == 270) ? LCD_HEIGHT : LCD_WIDTH;
        int win_h = (window_rotation == 90 || window_rotation == 270) ? LCD_WIDTH  : LCD_HEIGHT;
        SDL_SetWindowSize(sdl_window, win_w / 2, win_h / 2);
        SDL_GetRendererOutputSize(sdl_renderer, &drawable_w, &drawable_h);
    };

    if (event->type != SDL_KEYDOWN) return;
    switch (event->key.keysym.sym) {
    case SDLK_ESCAPE: app_running = false; break;
    case SDLK_r: rotate_window( 90); break;
    case SDLK_l: rotate_window(-90); break;
    }
}

static void sdl_event_handler(lv_timer_t *t) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            app_running = false;
            break;
        }
        sdl_mouse_event_handler(&event);
        sdl_keyboard_event_handler(&event);
    }
}

namespace pf_port {

void init(int fb_num, PixelFormat pixel_format) {
    SDL_PixelFormatEnum sdl_pixel_format;
    if (pixel_format == PixelFormat::RGB888) {
        sdl_pixel_format = SDL_PIXELFORMAT_RGB888;
        pixel_format_size = 3;
    } else {
        sdl_pixel_format = SDL_PIXELFORMAT_RGB565;
        pixel_format_size = 2;
    }

    for (int i = 0; i < fb_num; i++) {
        auto fb = malloc(LCD_WIDTH * LCD_HEIGHT * pixel_format_size);
        fb_pool.push_back(fb);
    }

    lv_init();
    SDL_Init(SDL_INIT_VIDEO);

    event_handler_timer = lv_timer_create(sdl_event_handler, 5, NULL);
    lv_tick_set_cb(SDL_GetTicks);
    lv_delay_set_cb(SDL_Delay);

    sdl_window = SDL_CreateWindow(
        "Tab5 Media Player",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LCD_WIDTH / 2, LCD_HEIGHT / 2,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );

    sdl_renderer = SDL_CreateRenderer(
        sdl_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    SDL_GetRendererOutputSize(sdl_renderer, &drawable_w, &drawable_h);
    SDL_Log("Drawable size: %d x %d", drawable_w, drawable_h);

    sdl_texture = SDL_CreateTexture(
        sdl_renderer,
        sdl_pixel_format,
        SDL_TEXTUREACCESS_STREAMING,
        LCD_WIDTH, LCD_HEIGHT
    );
}

void display_set_brightness(int value) {
    printf("[INFO] set brightness: %d\n", value);
}

void *display_get_frame_buffer(int fb_index) {
    if (fb_index < fb_pool.size()) return fb_pool[fb_index];
    return nullptr;
}

void display_flush(int fb_index) {
    if (fb_index >= fb_pool.size()) {
        printf("[ERR] display_flush: fb_index out of range, %d > %ld\n", fb_index, fb_pool.size());
    }
    if (fb_index != ::fb_index.load()) {
        ::fb_index.store(fb_index);
    }
}

std::optional<std::tuple<int, int>> touch_get_point() {
    return mouse;
}

void *psram_malloc(size_t size) {
    return malloc(size);
}
void *psram_malloc_dma(size_t size) {
    return malloc(size);
}

}

int main(void) {
    uvc_display_app();
    if (!sdl_window) return 1;

    app_running = true;
    while (app_running) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY){
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }

        auto fb = fb_pool[fb_index.load()];
        SDL_UpdateTexture(sdl_texture, NULL, fb, pixel_format_size * LCD_WIDTH);
        SDL_RenderClear(sdl_renderer);

        SDL_Rect dst;
        SDL_Point center;
        if (window_rotation == 90 || window_rotation == 270) {
            dst = { (drawable_w - drawable_h) / 2, (drawable_h - drawable_w) / 2, drawable_h, drawable_w };
            center = { drawable_h / 2, drawable_w / 2 };
        } else {
            dst = { 0, 0, drawable_w, drawable_h };
            center = { drawable_w / 2, drawable_h / 2 };
        }
        SDL_RenderCopyEx(sdl_renderer, sdl_texture, NULL, &dst,
                         (double)window_rotation, &center, SDL_FLIP_NONE);

        SDL_RenderPresent(sdl_renderer);
        usleep(sleep_time_ms * 1000);
    }

    SDL_DestroyTexture(sdl_texture);
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    return 0;
}
