/**
 * @file    picture_lvgl.h
 * @brief   触摸屏控制界面（320x240 横屏）
 *
 * 两个页面：
 *   主页   —— 状态栏 + 温湿度 + 4 路继电器按钮 + 设置入口
 *   设置页 —— 阈值/周期/地址/波特率/工作模式/自动联动
 */
#ifndef __PICTURE_LVGL_H
#define __PICTURE_LVGL_H

#include "lvgl.h"

/* 建立界面对象（调度器启动前调用，不涉及 RTOS 调用） */
void Ui_Init(void);

/* 显示任务主体：LVGL 刷新 + 触摸 + 低功耗停车握手 */
void Ui_Task(void const *argument);

#endif /* __PICTURE_LVGL_H */
