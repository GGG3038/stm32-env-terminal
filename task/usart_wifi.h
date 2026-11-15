#ifndef __USART_WIFI_H__
#define __USART_WIFI_H__

#include "stm32f1xx_hal.h"
#include "usart.h"

/* 调试串口 (USART1 作为 printf 输出) */
#define USART_DEBUG  (&huart1)

/* 通过串口发送原始数据 */
void Usart_SendString(UART_HandleTypeDef *huart, unsigned char *str, unsigned short len);

/* 调试格式化打印 (自动追加 \r\n) */
void UsartPrintf(UART_HandleTypeDef *huart, const char *fmt, ...);

#endif /* __USART_WIFI_H__ */
