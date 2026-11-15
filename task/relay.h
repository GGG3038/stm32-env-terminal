/**
 * @file    relay.h
 * @brief   4 路继电器 + PC13 LED 驱动（低电平触发）
 *
 * 所有控制源（Modbus / 云端 / 触摸屏 / 自动联动）统一走 Relay_* 接口，
 * 继电器掩码同步保存在 app_state 里供各模块读取。
 */
#ifndef __RELAY_H
#define __RELAY_H

#include <stdint.h>

#define RELAY_NUM           4

/* 继电器模块触发极性：1=低电平触发（常见光耦继电器板） 0=高电平触发 */
#define RELAY_ACTIVE_LOW    1

/* PC13 LED 极性：1=低电平点亮 */
#define LED_ACTIVE_LOW      1

/* 初始化 PC0~PC3 + PC13，全部输出关闭 */
void Relay_Init(void);

/* 按掩码驱动全部继电器（b0~b3 对应继电器 1~4） */
void Relay_Apply(uint8_t mask);

/* 单路控制/翻转 */
void Relay_Set(uint8_t idx, uint8_t on);
void Relay_Toggle(uint8_t idx);

/* 输出掩码（与 app_state.relay_mask 一致） */
uint8_t Relay_Get(void);

/* 实际电平回读（用于 Modbus 离散输入） */
uint8_t Relay_GetActual(void);

/* PC13 LED（报警/指示） */
void Led_Set(uint8_t on);

#endif /* __RELAY_H */
