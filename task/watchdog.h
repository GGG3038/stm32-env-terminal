/**
 * @file    watchdog.h
 * @brief   独立看门狗 IWDG（约 2s 超时）
 */
#ifndef __WATCHDOG_H
#define __WATCHDOG_H

/* 启动独立看门狗。必须在 os_start() 之前、所有耗时初始化之后调用：
   IWDG 一旦启动无法关闭，提前启动会让 LCD/ESP 初始化超时复位。 */
void Watchdog_Init(void);

/* 喂狗。放在空闲钩子里：任何任务把 CPU 占死、系统再也进不了空闲，
   看门狗就会复位；正常运行时空闲钩子约每 1ms 调用一次。 */
void Watchdog_Feed(void);

#endif
