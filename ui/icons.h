#ifndef UI_ICONS_H
#define UI_ICONS_H

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl.h"
#endif

extern const lv_img_dsc_t ui_img_light_png;       /* 32x32 灯光 */
extern const lv_img_dsc_t ui_img_turn_light_png;   /* 32x32 转向 */
extern const lv_img_dsc_t ui_img_temp_gray_png;    /* 32x32 温度 */
extern const lv_img_dsc_t ui_img_safety_belt_png;  /* 32x32 安全带 */

#endif
