# Tab5-UVC-Display — Claude notes

ESP32-P4 (M5Stack Tab5) project that pulls 1280×720 MJPEG from a USB UVC camera,
hardware-decodes it, and displays it rotated 90° on a 720×1280 MIPI-DSI panel.

## Build / flash / monitor

The dev environment lives in a Nix flake; **always use `nix develop`** instead
of sourcing `export.sh` directly:

```sh
nix develop --command idf.py -C esp32p4 build
nix develop --command idf.py -C esp32p4 flash monitor   # needs a TTY
```

When `idf.py monitor` can't attach (no TTY in non-interactive shell), drive
the serial directly with PySerial:

```sh
nix develop --command python3 -u -c "
import subprocess, serial, time, sys
subprocess.run(['python', '-m', 'esptool', '--chip', 'esp32p4', '-p',
    '/dev/cu.usbmodem114201', '-b', '460800', '--before=default_reset',
    '--after=hard_reset', 'write_flash', '--flash_mode', 'dio',
    '--flash_freq', '80m', '--flash_size', '16MB',
    '0x10000', 'esp32p4/build/tab5-uvc-display.bin'])
ser = serial.Serial('/dev/cu.usbmodem114201', 115200, timeout=0.2)
end = time.time() + 15
while time.time() < end:
    data = ser.read(4096)
    if data: sys.stdout.write(data.decode('utf-8', errors='replace')); sys.stdout.flush()
"
```

Note: the second `/dev/cu.usbmodem*` enumerator is the JTAG/console port we
flash on; the first one is busy if the user already has a monitor open.
**ESP32-P4 only prints logs once after reset** — capture during the boot
sequence, don't expect output later.

ESP-IDF v5.4.3 lives at `/nix/store/1jf3iqwyp77i8y54cgn6qxbrwl3wx5mz-esp-idf-v5.4.3/`.

## Layout

- `app/` — `PreviewScreen` (UVC frame → pipeline → display), `uvc_display.cpp`.
  Shared by ESP-IDF build and SDL `simulator/`.
- `components/{lvgl++,screen_manager}/` — shared UI helpers.
- `idf-components/main/` — IDF entry (`main.cpp`), `platform_port_*` adapters
  for JPEG/PPA, USB host wrappers. **No explicit `REQUIRES`** — the absence
  is intentional so that the implicit "all-components-available" mode stays
  on; adding `REQUIRES`/`PRIV_REQUIRES` here breaks transitive header lookup
  (e.g. `bsp_tab5.h`, `esp_timer.h`).
- `idf-components/m5tab5-bsp/` — vendored BSP for the panel + touch.
- `idf-components/jpeg_ppa_pipeline/` — the strip-pipelined JPEG + PPA path.
  Bypasses IDF's `jpeg_decoder_process()` by reaching into ESP-IDF private
  headers; do **not** override `esp_driver_jpeg` (the user explicitly rejected
  that approach). The component's CMakeLists adds private include paths via
  `target_include_directories(... PRIVATE $ENV{IDF_PATH}/components/esp_driver_jpeg{,/private})`.

`esp32p4/CMakeLists.txt` wires both `components/` and `idf-components/` via
`EXTRA_COMPONENT_DIRS`. The `main` component's CMake also globs `app/` and
`components/*/` into the main source list.

## jpeg_ppa_pipeline — design summary

Why it exists: JPEG-codec alone can do 60fps@1280×720; with the stock
`jpeg_decoder_process` → PSRAM → PPA SRM → PSRAM-FB chain, PSRAM bandwidth
caps throughput at ~20fps. Decoding into a ring of **internal SRAM** strip
buffers and feeding each strip through PPA SRM in parallel removes the
PSRAM-read leg and brings the system back to camera-saturating 30fps.

Pipeline shape:

```
JPEG codec ──TX from PSRAM JPEG stream──┐
                                        ▼
                              2D-DMA RX with N linked descriptors
                                        │
                                        ▼
                       SRAM ring of K strip buffers (1 MCU row * pic_w)
                                        │
                                        ▼  on_desc_done(ISR) → queue
                                  PPA worker task
                                        │  ppa_do_scale_rotate_mirror (blocking)
                                        ▼
                            PSRAM frame buffer (rotated)
```

Key implementation decisions that were learned the hard way:

1. **Owner-bit backpressure does NOT work for JPEG-RX.** Setting `owner=CPU`
   on a descriptor with `in_check_owner_chn=1` raises `RX_DSCR_ERROR` and
   ends the transaction; it does not pause. (See `dma2d_struct.h`,
   `in_dscr_err_chn_int_raw` doc: "including owner error".)
2. **End-of-chain (`next=NULL`) raises `IN_DSCR_EMPTY` and ends the
   transaction** — also not a pause mechanism.
3. The only viable backpressure is **dynamic chain extension via
   `dma2d_append()`**: pre-link only `ring_count` descriptors with the last
   one's `.next=NULL`, and splice in descriptor `i + ring_count` only after
   PPA finishes strip `i`. `release_strip()` does the splice + cache-msync
   + `dma2d_append`. The DMA never sees a CPU-owned descriptor and never
   reaches the chain end as long as `PPA-strip-time < (ring_count - 1) ×
   JPEG-strip-time`.
4. **`suc_eof=0` on every descriptor**. The JPEG↔DMA bridge fires `SUC_EOF`
   off the JPEG hardware's frame-done signal, not off the descriptor's eof
   bit. Setting `suc_eof=1` somewhere in the chain causes a spurious EOF and
   triggers the `dma2d_ll_rx_is_fsm_idle` assert in the dma2d ISR.
5. The `on_recv_eof` callback must **post `JPEG_DMA2D_RX_EOF` into the
   engine's `evt_queue`** to unblock our wait loop (mirroring IDF's
   `jpeg_rx_eof`); just releasing the semaphore wasn't enough and led to
   `decode timeout` errors.
6. `on_desc_done` does **not** populate `event_data->rx_eof_desc_addr` (only
   `on_recv_eof` does). Track strip index with a counter, not by inspecting
   the descriptor address from the event.

## Tuning (preview_screen.cpp)

```cpp
constexpr int STRIP_H = 16;       // must be a multiple of JPEG mcu_y
constexpr int RING_COUNT = 5;     // SRAM ring depth
```

Constraints to satisfy:

- `STRIP_H % mcu_y == 0`. For YUV420 JPEG: `mcu_y = 16`; for **YUV422 JPEG**
  (this camera): `mcu_y = 8`. STRIP_H=16 covers 2 MCU rows on YUV422.
- `pic_h % STRIP_H == 0`.
- `RING_COUNT * STRIP_H * pic_w * bytes_per_pixel < ~300 KiB` of available
  `MALLOC_CAP_INTERNAL`. After IDF + USB + LVGL + FreeRTOS overhead, the
  largest free SRAM region is the 384 KiB pool at `0x4FF40000` (boot log
  `heap_init: At 4FF40000 len 00060000 (384 KiB): RAM`).
- `RING_COUNT - 1 ≥ PPA-strip-time / JPEG-strip-time`. With STRIP_H=16,
  ring=5 has worked in practice for both RGB565 and RGB888 intermediates at
  30fps.
- **Smaller STRIP_H is not free.** Halving STRIP_H doubles the per-frame
  PPA `ppa_do_scale_rotate_mirror` call count; the per-call validation +
  cache-msync overhead drops total throughput (observed 30fps → 24fps
  going from STRIP_H=16 to STRIP_H=8 with RGB888).

## Output-FB strip placement

`Pipeline::Impl::set_up_ppa_op` maps strip `i` to its location in the
rotated output FB. PPA's `ANGLE_90` is **CW** (input top row → output right
column, then strip-0 lands at output `x=0`):

| rotation | block_offset_x       | block_offset_y       |
|---|---|---|
| 0   | 0                          | i * scaled_strip           |
| 90  | i * scaled_strip           | 0                          |
| 180 | 0                          | (N-1-i) * scaled_strip     |
| 270 | (N-1-i) * scaled_strip     | 0                          |

If output looks mirrored, swap the 90/270 formulas.

## Camera (UVC) notes

- Camera advertises `1280x720@30fps`. Asking for 60fps fails with
  `Could not find frame format 1280x720@60.0FPS`.
- The JPEG stream from this camera is **YUV422** (`mcu_y=8`), NOT YUV420.
  YUV420 SRM input (which could halve intermediate strip size) is therefore
  not directly usable — JPEG hardware can't transcode 422→420. Use RGB565
  or RGB888 intermediate instead.

## Things to check before changing the pipeline

- `Pipeline::Config::yuv_rgb_conv_std` chooses BT601 vs BT709 for the
  YUV→RGB CSC done inside the JPEG decode 2D-DMA path. Default BT601.
- `Pipeline::Config::input_color_mode` is the **intermediate strip
  format**, not the JPEG input format. It controls both the JPEG codec
  output format (`s_jpeg_out_for_ppa`) and the PPA SRM input color mode.
  RGB565 saves SRAM at the cost of color depth; RGB888 preserves precision.
- `s_bit_depth(cm) / 8` correctly sizes the strip buffer for YUV420 (12bpp);
  do not switch back to `bit_depth / 8` truncated arithmetic for buffer
  sizing.

## Risk / unfinished

- Backpressure margin is implicit: there's no runtime detection if PPA
  falls behind by more than `RING_COUNT-1` strips. Symptom would be the
  DMA chain reaching `next=NULL` and the transaction terminating
  prematurely. So far observed only when STRIP_H/RING_COUNT was set too
  aggressively.
- The simulator (`simulator/`) shares `app/` sources but has no JPEG/PPA
  path; pipeline changes there are no-ops, no need to keep them in sync.
