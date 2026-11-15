/**
 * @file    cfg_flash.h
 * @brief   配置掉电保存（STM32F103ZE 最后一页 Flash 0x0807F800，2KB/页）
 *
 * 注意：F1 从 Flash 执行代码时擦/写会停顿 CPU（页擦 ~20-40ms），
 *       期间 SysTick 丢 tick、USART 可能丢字节，因此只在系统空闲时调用，
 *       调用方需先置 g_cfg_saving 标志让云端任务避开 AT 事务。
 */
#ifndef __CFG_FLASH_H
#define __CFG_FLASH_H

#include <stdint.h>

/* 载入：校验 magic/长度/CRC，有效则导入 app_state；无效则保持默认 */
void    CfgFlash_Load(void);

/* 保存：擦页 + 写入 + 回读校验，返回 1 成功 0 失败 */
uint8_t CfgFlash_Save(void);

/* 校验当前 Flash 内配置是否有效 */
uint8_t CfgFlash_Valid(void);

#endif /* __CFG_FLASH_H */
