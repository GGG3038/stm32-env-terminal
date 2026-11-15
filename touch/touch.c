#include "touch.h"
#include "usart_wifi.h"     /* 调试串口：打印原始触摸值，方便标定 */
#include <stdint.h>

/* 1 = 触摸时在调试串口打印 raw/screen 坐标（最多 4 行/秒）。
   标定已完成（2026-10-20），正式固件关闭打印；需要重新标定时改回 1。 */
#define XPT2046_DEBUG_RAW   0

static int16_t avg_buf_x[XPT2046_AVG], avg_buf_y[XPT2046_AVG];
static uint8_t avg_last = 0;

/* 调试用 */
static volatile bool last_touch_state = false;
static volatile uint16_t last_touch_x = 0;
static volatile uint16_t last_touch_y = 0;

/* ===== GPIO 软件 SPI =====
 * XPT2046 最大 SPI 时钟约 2.5MHz，这里半周期约 0.4us（≈1MHz），
 * 杜邦线稍长也稳定。mode 0：上升沿采样，下降沿移位。 */
static inline void xpt_delay(void)
{
    volatile uint8_t i;
    for (i = 0; i < 6; i++) {
        __NOP();
    }
}

static uint8_t xpt_xfer(uint8_t out)
{
    uint8_t in = 0;
    int8_t i;

    for (i = 7; i >= 0; i--) {
        if (out & (1U << i))
            XPT2046_MOSI_HIGH();
        else
            XPT2046_MOSI_LOW();

        xpt_delay();
        XPT2046_SCK_HIGH();
        xpt_delay();

        in = (uint8_t)((in << 1) | (XPT2046_MISO_READ() ? 1U : 0U));

        XPT2046_SCK_LOW();
        xpt_delay();
    }

    return in;
}

/*
 * 标准 XPT2046 3字节读取时序：
 *   发送 cmd (8 clks) → 接收高位 (8 clks) → 接收低位 (8 clks)
 *   12位ADC值在高位/低位中，通过 ((高位<<8 | 低位) >> 3) 取出
 */
static uint16_t XPT2046_ReadRaw(uint8_t cmd)
{
    uint8_t hi, lo;

    XPT2046_CS_LOW();
    (void)xpt_xfer(cmd);
    hi = xpt_xfer(0x00);
    lo = xpt_xfer(0x00);
    XPT2046_CS_HIGH();

    return (uint16_t)((((uint16_t)hi << 8) | lo) >> 3);
}

static void xpt_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* CS / SCK / MOSI：推挽输出 */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = XPT2046_CS_Pin;
    HAL_GPIO_Init(XPT2046_CS_GPIO_Port, &gpio);
    gpio.Pin = XPT2046_SCK_Pin;
    HAL_GPIO_Init(XPT2046_SCK_GPIO_Port, &gpio);
    gpio.Pin = XPT2046_MOSI_Pin;
    HAL_GPIO_Init(XPT2046_MOSI_GPIO_Port, &gpio);

    /* MISO：输入，开上拉避免悬空（PB2 同时是 BOOT1，BOOT0=0 时无影响） */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Pin  = XPT2046_MISO_Pin;
    HAL_GPIO_Init(XPT2046_MISO_GPIO_Port, &gpio);

    XPT2046_CS_HIGH();
    XPT2046_SCK_LOW();
    XPT2046_MOSI_LOW();
}

void XPT2046_Init(void)
{
    xpt_gpio_init();
}

bool XPT2046_GetTouchXY(uint16_t *x, uint16_t *y)
{
    /* 不依赖PENIRQ，直接读Z压力判断触摸 */
    uint16_t z1 = XPT2046_ReadRaw(0xB0);  // Z1 压力
    uint16_t z2 = XPT2046_ReadRaw(0xC0);  // Z2 压力

    /* 未触摸时 Z1 ~0, Z2 ~4095 */
    if (z1 < 80 || (z2 - z1) < 100) {
        return false;
    }

    /* 读取 X / Y 坐标 */
    uint16_t tx = XPT2046_ReadRaw(0xD0);
    uint16_t ty = XPT2046_ReadRaw(0x90);

    /* 基本范围检查 */
    if (tx < 50 || tx > 4090 || ty < 50 || ty > 4090) {
        return false;
    }

    *x = tx;
    *y = ty;
    return true;
}

/* 原始 ADC 值 → 0..1000 的归一化比例 */
static uint32_t norm(uint16_t raw, uint16_t lo, uint16_t hi)
{
    uint32_t v;

    if (raw <= lo) return 0;
    v = ((uint32_t)(raw - lo) * 1000U) / (hi - lo);
    return (v > 1000U) ? 1000U : v;
}

/* 原始触摸坐标 → 320x240 屏幕坐标（横屏）
   轴向交换和翻转由 touch.h 里的 3 个宏控制，上机时按实际效果调 */
void XPT2046_Correct(uint16_t *x, uint16_t *y)
{
    uint32_t fx = norm(*x, XPT2046_X_MIN, XPT2046_X_MAX);   /* 触摸自身 X 轴 */
    uint32_t fy = norm(*y, XPT2046_Y_MIN, XPT2046_Y_MAX);   /* 触摸自身 Y 轴 */
    uint32_t sx, sy;

#if XPT2046_SWAP_XY
    sx = fy;    /* 屏幕 X ← 触摸 Y */
    sy = fx;    /* 屏幕 Y ← 触摸 X */
#else
    sx = fx;
    sy = fy;
#endif

#if XPT2046_INVERT_X
    sx = 1000U - sx;
#endif
#if XPT2046_INVERT_Y
    sy = 1000U - sy;
#endif

    *x = (uint16_t)((sx * (XPT2046_HOR_RES - 1)) / 1000U);
    *y = (uint16_t)((sy * (XPT2046_VER_RES - 1)) / 1000U);
}

void XPT2046_Average(uint16_t *x, uint16_t *y)
{
    for (int i = XPT2046_AVG - 1; i > 0; i--) {
        avg_buf_x[i] = avg_buf_x[i - 1];
        avg_buf_y[i] = avg_buf_y[i - 1];
    }
    avg_buf_x[0] = *x;
    avg_buf_y[0] = *y;
    if (avg_last < XPT2046_AVG) avg_last++;

    int32_t x_sum = 0, y_sum = 0;
    for (int i = 0; i < avg_last; i++) {
        x_sum += avg_buf_x[i];
        y_sum += avg_buf_y[i];
    }
    *x = (int16_t)(x_sum / avg_last);
    *y = (int16_t)(y_sum / avg_last);
}

void XPT2046_Read(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    static int16_t last_x = 0;
    static int16_t last_y = 0;

    (void)drv;

    uint16_t x, y;
    if (XPT2046_GetTouchXY(&x, &y)) {
        uint16_t raw_x = x, raw_y = y;

        XPT2046_Correct(&x, &y);
        XPT2046_Average(&x, &y);

#if XPT2046_DEBUG_RAW
        {
            static uint32_t last_dbg = 0;
            if (HAL_GetTick() - last_dbg >= 250U) {
                last_dbg = HAL_GetTick();
                UsartPrintf(USART_DEBUG, "touch raw=%u,%u screen=%u,%u",
                            raw_x, raw_y, x, y);
            }
        }
#endif

        last_x = (int16_t)x;
        last_y = (int16_t)y;

        last_touch_state = true;
        last_touch_x = (uint16_t)x;
        last_touch_y = (uint16_t)y;

        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        last_touch_state = false;

        /* 松手就清空滑动平均：否则下一次按下的前几个采样会和上一次的
           旧坐标混在一起，快速连续点相邻按钮时会点空（实测丢过 R2） */
        avg_last = 0;

        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
    }
}

/* 调试导出 */
bool XPT2046_IsTouching(void)  { return last_touch_state; }
uint16_t XPT2046_GetLastX(void) { return last_touch_x; }
uint16_t XPT2046_GetLastY(void) { return last_touch_y; }
