/**
 * @file    modbus_rtu.h
 * @brief   Modbus RTU 从机协议栈（USART3 / PB10、PB11）
 *
 * 功能码：0x01 0x02 0x03 0x04 0x05 0x06 0x0F 0x10
 * 支持广播地址 0 写入（不应答）；异常码 01 02 03 04
 * 收帧：IDLE 中断 + t3.5 静默校验
 */
#ifndef __MODBUS_RTU_H
#define __MODBUS_RTU_H

#include <stdint.h>

/* 固件版本（输入寄存器 0x0005）
   0x0100 初版；0x0101 关闭触摸调试打印、加入 IWDG 看门狗 */
#define MB_FW_VERSION       0x0101

/* 寄存器表规模 */
#define MB_INPUT_REG_NUM    8       /* 输入寄存器 0x0000~0x0007 */
#define MB_HOLD_REG_NUM     14      /* 保持寄存器 0x0000~0x000D */
#define MB_COIL_NUM         6       /* 线圈       0x0000~0x0005 */
#define MB_DISCRETE_NUM     6       /* 离散输入   0x0000~0x0005 */

/* 是否使用 RS485 收发方向控制引脚：
 *   0 = 自动收发模块 / TTL 直连（默认，无需接线改动）
 *   1 = MAX485/SP3485 等需要 DE 的模块，用 PC4 做方向控制 */
#define MODBUS_USE_DE_PIN   0
#if MODBUS_USE_DE_PIN
    #define MODBUS_DE_PORT  GPIOC
    #define MODBUS_DE_PIN   GPIO_PIN_4
#endif

/* Modbus RTU CRC16（多项式 0xA001），本工程其它模块也复用 */
uint16_t Crc16_Modbus(const uint8_t *data, uint16_t len);

/* 初始化 USART3（波特率取配置项）并启动接收，创建任务前调用 */
void Modbus_Init(void);

/* 任务主体，由 freertos.c 作为任务函数创建 */
void Modbus_Task(void const *argument);

/* 轮询一次（取帧 + 校验 t3.5 + 处理 + 应答），Modbus_Task 里循环调用 */
void Modbus_Poll(void);

/* 供 uart_handler.c 在 USART3 RxEvent 中断里调用：
   数据已由 HAL 收到模块内部的 s_rx_buf，len 为本次收到的字节数 */
void Modbus_RxEventCallback(uint16_t len);

/* IDLE 事件后 HAL 会停止接收，由 uart_handler.c 调用本函数重新挂 ReceiveToIdle
   （不丢组帧缓冲，区别于出错时的 Modbus_RxReset） */
void Modbus_RxRearm(void);

/* UART 出错（ORE/FE/NE/PE）后重新挂接收，由 uart_handler.c 调用 */
void Modbus_RxReset(void);

/* 从机地址（有效配置）*/
uint8_t Modbus_GetSlaveAddr(void);

#endif /* __MODBUS_RTU_H */
