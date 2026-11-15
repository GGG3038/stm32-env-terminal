/**
  ******************************************************************************
  * @file    os_config.h
  * @brief   轻量级 RTOS 配置项
  *
  * 修改本文件即可裁剪内核。所有配置在编译期生效。
  ******************************************************************************
  */
#ifndef __OS_CONFIG_H
#define __OS_CONFIG_H

/* 任务数量上限（包含空闲任务）。TCB 和就绪链表全部静态分配 */
#define OS_CFG_MAX_TASKS              8U

/* 优先级数量，取值 1~32。优先级 0 保留给空闲任务 */
#define OS_CFG_PRIO_COUNT             32U

/* 系统节拍频率 (Hz)。SysTick 使用该频率，默认 1ms 一个 tick */
#define OS_CFG_TICK_HZ                1000U

/* 空闲任务栈大小，单位：word (4 字节) */
#define OS_CFG_IDLE_STACK_WORDS       128U

/* 用户任务栈最小大小，单位：word。必须大于首次启动栈帧 (16 word) */
#define OS_CFG_MIN_STACK_WORDS        32U

/* 同优先级时间片，单位：tick。设为 0 表示关闭时间片轮转 */
#define OS_CFG_TIME_SLICE_TICKS       1U

/* 是否使能参数检查与断言。发布时可关闭以减小代码 */
#define OS_CFG_USE_ASSERT             1U

/* 栈填充魔数，用于统计栈高水位 */
#define OS_CFG_STACK_FILL_WORD        0xA5A5A5A5U

#endif /* __OS_CONFIG_H */
