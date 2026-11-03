// LVGL's two hot RGB565 blend paths on the ESP32-S3 SIMD assembly (include/lv_blend_esp32s3.h, src/simd/).
#include <lvgl.h>
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include <lv_blend_esp32s3.h>

// the argument block the assembly reads (esp_lvgl_port_lv_blend.h asm_dsc_t; the offsets are in the .S comments)
typedef struct
{
  uint32_t opa;
  void *dst_buf;
  uint32_t dst_w;
  uint32_t dst_h;
  uint32_t dst_stride;
  const void *src_buf;
  uint32_t src_stride;
  const lv_opa_t *mask_buf;
  uint32_t mask_stride;
} asm_dsc_t;

extern int lv_color_blend_to_rgb565_esp(asm_dsc_t *asm_dsc);
extern int lv_rgb565_blend_normal_to_rgb565_esp(asm_dsc_t *asm_dsc);

static volatile int simdOn = 0;

void c2gSimdEnable(int on) { simdOn = on; }
int c2gSimdEnabled(void) { return simdOn; }

int c2g_color_blend_to_rgb565(struct _lv_draw_sw_blend_fill_dsc_t *dsc)
{
  if (!simdOn)
    return LV_RESULT_INVALID; // LVGL's C fill
  asm_dsc_t a = {0};
  a.dst_buf = dsc->dest_buf;
  a.dst_w = dsc->dest_w;
  a.dst_h = dsc->dest_h;
  a.dst_stride = dsc->dest_stride;
  a.src_buf = &dsc->color;
  return lv_color_blend_to_rgb565_esp(&a);
}

int c2g_rgb565_blend_normal_to_rgb565(struct _lv_draw_sw_blend_image_dsc_t *dsc)
{
  if (!simdOn)
    return LV_RESULT_INVALID; // LVGL's C copy
  asm_dsc_t a = {0};
  a.dst_buf = dsc->dest_buf;
  a.dst_w = dsc->dest_w;
  a.dst_h = dsc->dest_h;
  a.dst_stride = dsc->dest_stride;
  a.src_buf = dsc->src_buf;
  a.src_stride = dsc->src_stride;
  return lv_rgb565_blend_normal_to_rgb565_esp(&a);
}
