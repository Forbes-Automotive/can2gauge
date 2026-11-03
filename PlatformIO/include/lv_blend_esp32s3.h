#pragma once
// ---------------------------------------------------------------------------
// LVGL's software blending, the two hot RGB565 paths on the ESP32-S3's SIMD (PIE) instructions (0.11.0 frame-rate work).
// lv_conf.h names this file as LV_DRAW_SW_ASM_CUSTOM_INCLUDE, so LVGL's lv_draw_sw_blend_to_rgb565.c calls:
//   LV_DRAW_SW_COLOR_BLEND_TO_RGB565          an opaque fill with no mask (backgrounds, panels, bars, solid shapes)
//   LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565  an opaque RGB565 image copy with no mask (sprite frames, logos)
// The assembly is Espressif's (esp-bsp components/esp_lvgl_port/src/lvgl9/simd, Apache-2.0), unmodified in src/simd/.
// The wrappers (src/lv_blend_esp32s3.c) return LV_RESULT_INVALID while switched off (settings.simdBlend), and LVGL
// then does it in C as usual - so the two can be compared with the Diag benchmark without reflashing.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

struct _lv_draw_sw_blend_fill_dsc_t;
struct _lv_draw_sw_blend_image_dsc_t;
int c2g_color_blend_to_rgb565(struct _lv_draw_sw_blend_fill_dsc_t *dsc);
int c2g_rgb565_blend_normal_to_rgb565(struct _lv_draw_sw_blend_image_dsc_t *dsc);
void c2gSimdEnable(int on); // 0 = LVGL's C code
int c2gSimdEnabled(void);

#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) c2g_color_blend_to_rgb565(dsc)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc) c2g_rgb565_blend_normal_to_rgb565(dsc)

#ifdef __cplusplus
}
#endif
