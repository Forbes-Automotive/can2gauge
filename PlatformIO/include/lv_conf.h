/**
 * can2gauge lv_conf.h - LVGL 9.3 configuration (partial: everything not listed takes the lv_conf_internal.h default)
 */
#if 1 /* Set this to "1" to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
/* LVGL 9.3 deprecated deriving the format from LV_COLOR_DEPTH; set the format explicitly (== RGB565 for depth 16)
   so lv_conf_internal.h doesn't emit its deprecation #warning. */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/* memory: LVGL's own allocator, its pool (LV_MEM_SIZE) taken from PSRAM when LVGL starts. It was a static array in internal RAM
   (faster widget work), but that left too little internal RAM for WiFi, the web server and the tasks (2026-10-14: WiFi
   failed with ESP_ERR_NO_MEM). Pixels are still drawn in internal RAM or straight into the frame buffers. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
/* 0.31.0: 96 KB -> 512 KB (PSRAM has room). The pool holds every object on every page, menu and card, AND the temporary
   layers LVGL draws scaled values into (Custom's main value at 1.25x, Single's at 2x: ~60-120 KB each). At 96 KB those
   ran out: Custom's main value wasn't drawn at all (photo 2026-10-30), and a failed allocation elsewhere stops the screen
   task for good (LV_USE_ASSERT_MALLOC). Diag > Display shows how full it is (lvglMemUsedPct). */
#define LV_MEM_SIZE (512 * 1024U)
#define LV_MEM_POOL_EXPAND_SIZE 0
#define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>                                                  /* for heap_caps_malloc below */
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) /* LVGL calls this once for its pool: PSRAM */

/* HAL */
#define LV_DEF_REFR_PERIOD 16 /* ms - ~60 fps ceiling, the RGB panel refreshes on its own */
#define LV_DPI_DEF 130
#define LV_USE_OS LV_OS_NONE /* we wrap lv_timer_handler in our own mutex */

/* drawing */
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 16 /* 0.24.5: was 64. The RGB frame buffers aren't both on 64 (LVGL then refused them for direct mode
                                and drew nothing - black glass, 2026-10-26); 16 is what the S3's 128-bit SIMD loads need */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_A8 1
#define LV_DRAW_SW_SUPPORT_RGB565A8 1 /* sprite packs with transparency (include/c2gs.h) */
#define LV_DRAW_SW_SUPPORT_I1 0
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
/* the ESP32-S3 SIMD fill / RGB565 copy (include/lv_blend_esp32s3.h, src/simd/), switchable at run time */
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_CUSTOM
#define LV_DRAW_SW_ASM_CUSTOM_INCLUDE "lv_blend_esp32s3.h"

/* logging / asserts */
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/* input */
#define LV_INDEV_DEF_READ_PERIOD 20
#define LV_INDEV_DEF_SCROLL_LIMIT 10
#define LV_INDEV_DEF_SCROLL_THROW 10
#define LV_INDEV_DEF_LONG_PRESS_TIME 600 /* overridden at runtime from settings.longPressMs */
#define LV_INDEV_DEF_LONG_PRESS_REP_TIME 100
#define LV_INDEV_DEF_GESTURE_LIMIT 40
#define LV_INDEV_DEF_GESTURE_MIN_VELOCITY 3

/* features */
#define LV_USE_FLOAT 0
#define LV_USE_MATRIX 0
#define LV_USE_OBJ_ID 0
#define LV_USE_OBJ_PROPERTY 0

/* fonts */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0

/* text */
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"

/* widgets */
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 1
#define LV_USE_CHART 1
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMAGE 1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LABEL 1
#define LV_USE_LED 1
#define LV_USE_LINE 1
#define LV_USE_LIST 1
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 1
#define LV_USE_ROLLER 1
#define LV_USE_SCALE 1
#define LV_USE_SLIDER 1
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 1
#define LV_USE_SWITCH 1
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* themes / layouts */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* others */
#define LV_USE_SNAPSHOT 0
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_LODEPNG 0
#define LV_USE_TJPGD 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_FFMPEG 0
#define LV_USE_OBSERVER 1
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0

#endif /* LV_CONF_H */
#endif /* End of "Content enable" */
