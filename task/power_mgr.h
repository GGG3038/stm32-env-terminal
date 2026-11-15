/**
 * @file    power_mgr.h
 * @brief   电源管理：双模式（正常/低功耗）、SLEEP 低功耗、报警联动、配置落盘
 *
 * 双模式：
 *   正常模式  —— 背光常亮、Modbus 随时在线
 *   低功耗模式—— 关屏关背光、ESP 休眠、MCU 空闲时 SLEEP；
 *                 USART3 保持运行，Modbus 在任何时刻都能应答（SLEEP 方案）
 *
 * 上车顺序（main.c USER CODE 2）：
 *   App_Init() → CfgFlash_Load() → Relay_Init() → PowerMgr_Init() → os_start()
 */
#ifndef __POWER_MGR_H
#define __POWER_MGR_H

#include <stdint.h>

/* 停车对象：低功耗时要求这些任务先停下（关屏 / ESP 进休眠） */
#define PARK_DISP   0
#define PARK_CLOUD  1

/* 背光 GPIO 初始化（纯寄存器操作，调度器启动前调用） */
void PowerMgr_Init(void);

/* 任务主体 */
void PowerMgr_Task(void const *argument);

/* 背光统一入口：Modbus / 触摸屏 / 显示任务都用这个，不要在别处直接写 PB0 */
void PowerMgr_SetBacklight(uint8_t on);

/* 请求切换工作模式（Modbus / 触摸屏 / 按键调用），由 PowerMgr 任务执行 */
void PowerMgr_RequestMode(uint8_t mode);

/* 请求把配置立刻写入 Flash（保持寄存器 0x000C 写 1 时调用） */
void PowerMgr_RequestSaveNow(void);

/* ---- 停车握手：给显示/云端任务用 ---- */
uint8_t PowerMgr_ParkReq(void);                     /* 轮询：1 = 请停车 */
void    PowerMgr_ParkAck(uint8_t who, uint8_t parked);  /* 回报自己是否已停车 */

#endif /* __POWER_MGR_H */
