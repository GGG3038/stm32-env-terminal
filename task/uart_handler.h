#ifndef __UART_HANDLER_H__
#define __UART_HANDLER_H__

#include "stm32f1xx_hal.h"

/**
 * @brief  UART 中断回调集中处（uart_handler.c）
 *
 *   USART2 —— ESP8266，逐字节中断接收 → HAL_UART_RxCpltCallback
 *   USART3 —— Modbus RTU，IDLE 定帧   → HAL_UARTEx_RxEventCallback
 *   两者都实现了 HAL_UART_ErrorCallback（ORE/FE/NE/PE 后重新挂接收，
 *   否则一次溢出就会永久收不到数据）
 *
 * 这三个回调由 HAL 中断服务函数自动调用，不需要应用层声明或注册。
 */

#endif /* __UART_HANDLER_H__ */
