/**
 * @file lv_port_disp.c
 * LVGL v8 显示驱动 - ILI9341 横屏 320x240 (MV=1模式)
 */

/* Enable this file */
#if 1

/*********************
 *      INCLUDES
 *********************/
#include "lv_port_disp.h"
#include "lvgl.h"
#include "lcd.h"

/*********************
 *      DEFINES
 *********************/
#define MY_DISP_HOR_RES    320
#define MY_DISP_VER_RES    240
#define DISP_BUF_LINES     5     /* 5行缓冲省3.2KB */
#define DISP_BUF_SIZE      (MY_DISP_HOR_RES * DISP_BUF_LINES)

/*********************
 *  STATIC PROTOTYPES
 *********************/
static void disp_init(void);
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

/*********************
 *  STATIC VARIABLES
 *********************/
static lv_color_t buf[DISP_BUF_SIZE];

/*********************
 *   GLOBAL FUNCTIONS
 *********************/
void lv_port_disp_init(void)
{
    disp_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, DISP_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = MY_DISP_HOR_RES;
    disp_drv.ver_res = MY_DISP_VER_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void disp_init(void)
{
    ILI9341_Init();
}

/* LVGL刷新回调 - 直接使用v9原版逻辑，MV=1下ILI9341自动处理行列映射 */
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t w  = area->x2 - x1 + 1;
    int32_t h  = area->y2 - y1 + 1;
    uint32_t size = (uint32_t)(w * h);
    uint16_t * c16 = (uint16_t *)color_p;

    /* CASET: 列地址 */
    ILI9341_Write_Cmd(CMD_SetCoordinateX);
    ILI9341_Write_Data(x1 >> 8);
    ILI9341_Write_Data(x1 & 0xFF);
    ILI9341_Write_Data((x1 + w - 1) >> 8);
    ILI9341_Write_Data((x1 + w - 1) & 0xFF);

    /* PASET: 行地址 */
    ILI9341_Write_Cmd(CMD_SetCoordinateY);
    ILI9341_Write_Data(y1 >> 8);
    ILI9341_Write_Data(y1 & 0xFF);
    ILI9341_Write_Data((y1 + h - 1) >> 8);
    ILI9341_Write_Data((y1 + h - 1) & 0xFF);

    /* RAMWR: 启动内存写入 */
    ILI9341_Write_Cmd(CMD_SetPixel);

    /* 平坦单循环写入像素 (MV=1自动处理行列映射) */
    for (uint32_t i = 0; i < size; i++) {
        ILI9341_Write_Data(c16[i]);
    }

    lv_disp_flush_ready(disp_drv);
}

#else
typedef int keep_pedantic_happy;
#endif
