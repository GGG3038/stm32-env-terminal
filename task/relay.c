/**
 * @file    relay.c
 * @brief   4 路继电器 + PC13 LED 驱动
 *
 * 引脚：PC0~PC3 = 继电器 1~4，PC13 = LED
 * 极性由 RELAY_ACTIVE_LOW / LED_ACTIVE_LOW 决定，改一行即可适配硬件。
 */
#include "relay.h"
#include "app_state.h"
#include "stm32f1xx_hal.h"

#define RELAY_GPIO_PORT     GPIOC
#define RELAY_GPIO_PINS     (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3)
#define LED_GPIO_PIN        GPIO_PIN_13

static uint8_t s_mask = 0;      /* 当前输出掩码（唯一写 GPIO 的地方） */

void Relay_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin   = RELAY_GPIO_PINS | LED_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RELAY_GPIO_PORT, &gpio);

    /* 全部输出关闭 */
    Relay_Apply(0);
    Led_Set(0);
}

void Relay_Apply(uint8_t mask)
{
    uint32_t set_bits = 0, rst_bits = 0;
    uint8_t i;

    mask &= 0x0F;

    for (i = 0; i < RELAY_NUM; i++) {
        uint32_t pin = (1UL << i);
        if (mask & (1UL << i)) {
#if RELAY_ACTIVE_LOW
            rst_bits |= pin;
#else
            set_bits |= pin;
#endif
        } else {
#if RELAY_ACTIVE_LOW
            set_bits |= pin;
#else
            rst_bits |= pin;
#endif
        }
    }

    /* BSRR 原子写：低 16 位置位，高 16 位复位 */
    RELAY_GPIO_PORT->BSRR = set_bits | (rst_bits << 16);

    s_mask = mask;
    App_SetRelay(mask);
}

void Relay_Set(uint8_t idx, uint8_t on)
{
    uint8_t mask = s_mask;
    if (idx >= RELAY_NUM) return;
    if (on) mask |= (1U << idx);
    else    mask &= (uint8_t)~(1U << idx);
    Relay_Apply(mask);
}

void Relay_Toggle(uint8_t idx)
{
    if (idx >= RELAY_NUM) return;
    Relay_Set(idx, !(s_mask & (1U << idx)));
}

uint8_t Relay_Get(void)
{
    return s_mask;
}

uint8_t Relay_GetActual(void)
{
    uint32_t idr = RELAY_GPIO_PORT->IDR;
    uint8_t mask = 0;
    uint8_t i;

    for (i = 0; i < RELAY_NUM; i++) {
        uint8_t level = (idr >> i) & 1U;
#if RELAY_ACTIVE_LOW
        level = !level;
#endif
        if (level) mask |= (1U << i);
    }
    return mask;
}

void Led_Set(uint8_t on)
{
#if LED_ACTIVE_LOW
    HAL_GPIO_WritePin(RELAY_GPIO_PORT, LED_GPIO_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(RELAY_GPIO_PORT, LED_GPIO_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
    App_SetLed(on);
}
