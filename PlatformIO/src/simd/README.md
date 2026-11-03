# ESP32-S3 SIMD blend routines

Copied unmodified from Espressif's esp-bsp, `components/esp_lvgl_port/src/lvgl9/simd/` (Apache-2.0, see each file's
SPDX header): `lv_color_blend_to_rgb565_esp32s3.S`, `lv_rgb565_blend_normal_to_rgb565_esp32s3.S` and the
`lv_macro_memcpy.S` they include. Wired into LVGL by `include/lv_blend_esp32s3.h` and `src/lv_blend_esp32s3.c`
(lv_conf.h `LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_CUSTOM`), switched by settings.simdBlend (web UI > Diag).
