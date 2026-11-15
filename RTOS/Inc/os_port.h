/**
  ******************************************************************************
  * @file    os_port.h
  * @brief   Cortex-M3 移植层
  *
  * 任务使用 PSP，异常使用 MSP。
  *
  * 首次启动栈帧（64 字节，从低地址到高地址）：
  *   [sp+0  .. sp+7 ]  R4-R11（软件保存区，初始为 0）
  *   [sp+8  .. sp+11]  R0-R3（R0 = 任务参数）
  *   [sp+12]           R12
  *   [sp+13]           LR  = os_task_exit
  *   [sp+14]           PC  = 任务入口
  *   [sp+15]           xPSR = 0x01000000
  ******************************************************************************
  */
#ifndef __OS_PORT_H
#define __OS_PORT_H

#include <stdint.h>
#include "stm32f1xx.h"

/* 首次启动栈帧的 word 数 */
#define OS_PORT_STACK_FRAME_WORDS     16U

/* Thumb 模式 xPSR，仅置 T 位 */
#define OS_PORT_INITIAL_XPSR          0x01000000U

/* 是否处于中断上下文 */
__STATIC_INLINE uint32_t os_port_is_isr(void)
{
    return (__get_IPSR() != 0U) ? 1U : 0U;
}

/* 启动第一个任务（SVC 0），永不返回 */
void os_port_start_first_task(void);

/* PendSV 的 C 部分：选择下一个任务并更新 os_current_tcb */
void os_pendsv_switch(void);

#endif /* __OS_PORT_H */
