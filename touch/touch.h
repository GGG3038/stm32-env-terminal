#ifndef __TOUCH_H
#define __TOUCH_H

#include "stm32f1xx_hal.h"
#include <stdbool.h>
#include "lvgl.h"  // LVGL的头文件



/* 屏幕分辨率：和 lv_port_disp.c 的 320x240 横屏保持一致
   （原代码这里是 240x320 竖屏，映射出来的坐标对不上横屏界面） */
#define XPT2046_HOR_RES     320
#define XPT2046_VER_RES     240

/* 触摸标定参数：上机后如果触摸位置偏移，就改下面 3 个开关 + 4 个量程，
   配合界面上的“触摸坐标调试页”两分钟就能调好 */
#define XPT2046_SWAP_XY     1   /* 横屏必须交换触摸 X/Y 轴 */
#define XPT2046_INVERT_X    0   /* 屏幕 X 方向翻转：本板实测不需要 */
#define XPT2046_INVERT_Y    0   /* 屏幕 Y 方向翻转 */

/* 2026-10-20 按实机四角触摸值标定：
 *   左上 raw(575,338)  右上 raw(450,3580)
 *   左下 raw(3446,356) 右下 raw(3260,3526)
 * 交换后：屏幕 X ← raw_y，屏幕 Y ← raw_x */
#define XPT2046_X_MIN       450
#define XPT2046_Y_MIN       338
#define XPT2046_X_MAX       3446
#define XPT2046_Y_MAX       3580
#define XPT2046_AVG         4

/* ===== XPT2046 引脚（按原理图接线）=====
 *   T_CS   = PF11
 *   T_MISO = PB2
 *   T_SCK  = PB1
 *   T_MOSI = PF9
 *
 * 这四个都不是 STM32F1 的 SPI 复用脚，所以用 GPIO 软件模拟 SPI（bit-bang），
 * 不再使用硬件 SPI2。原 CubeMX 里的 SPI2(PB13~PB15) 保持不动，不影响本项目。
 *
 * T_PEN(PF10) 只保留宏定义：现在的读取用 XPT2046 的 Z1/Z2 压力判断，
 * 不依赖 PEN 中断，即使板上没接也不影响触摸。 */
#define XPT2046_CS_GPIO_Port    GPIOF
#define XPT2046_CS_Pin          GPIO_PIN_11
#define XPT2046_SCK_GPIO_Port   GPIOB
#define XPT2046_SCK_Pin         GPIO_PIN_1
#define XPT2046_MOSI_GPIO_Port  GPIOF
#define XPT2046_MOSI_Pin        GPIO_PIN_9
#define XPT2046_MISO_GPIO_Port  GPIOB
#define XPT2046_MISO_Pin        GPIO_PIN_2

#define XPT2046_CS_LOW()    HAL_GPIO_WritePin(XPT2046_CS_GPIO_Port, XPT2046_CS_Pin, GPIO_PIN_RESET)
#define XPT2046_CS_HIGH()   HAL_GPIO_WritePin(XPT2046_CS_GPIO_Port, XPT2046_CS_Pin, GPIO_PIN_SET)
#define XPT2046_SCK_LOW()   HAL_GPIO_WritePin(XPT2046_SCK_GPIO_Port, XPT2046_SCK_Pin, GPIO_PIN_RESET)
#define XPT2046_SCK_HIGH()  HAL_GPIO_WritePin(XPT2046_SCK_GPIO_Port, XPT2046_SCK_Pin, GPIO_PIN_SET)
#define XPT2046_MOSI_LOW()  HAL_GPIO_WritePin(XPT2046_MOSI_GPIO_Port, XPT2046_MOSI_Pin, GPIO_PIN_RESET)
#define XPT2046_MOSI_HIGH() HAL_GPIO_WritePin(XPT2046_MOSI_GPIO_Port, XPT2046_MOSI_Pin, GPIO_PIN_SET)
#define XPT2046_MISO_READ() (HAL_GPIO_ReadPin(XPT2046_MISO_GPIO_Port, XPT2046_MISO_Pin) == GPIO_PIN_SET)

#define XPT2046_PEN_GPIO_Port   GPIOF
#define XPT2046_PEN_Pin         GPIO_PIN_10
#define XPT2046_PRESSED()   (HAL_GPIO_ReadPin(XPT2046_PEN_GPIO_Port, XPT2046_PEN_Pin) == GPIO_PIN_RESET)

void XPT2046_Init(void);
bool XPT2046_GetTouchXY(uint16_t *x, uint16_t *y);
void XPT2046_Correct(uint16_t *x, uint16_t *y);  
void XPT2046_Average(uint16_t *x, uint16_t *y);
/* 第二个参数是 lv_indev_drv_t（驱动本身），不是 lv_indev_t（设备实例）。
   写成 lv_indev_t 编译只报 #513-D 警告，但和 LVGL 的 read_cb 原型并不一致 */
void XPT2046_Read(lv_indev_drv_t * drv, lv_indev_data_t * data);

/* 调试用：获取最后一次触摸的原始状态 */
bool XPT2046_IsTouching(void);
uint16_t XPT2046_GetLastX(void);
uint16_t XPT2046_GetLastY(void);
#endif
