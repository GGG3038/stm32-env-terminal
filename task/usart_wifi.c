#include "usart_wifi.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/**
 * @brief  通过串口发送原始字节数据 (阻塞)
 */
void Usart_SendString(UART_HandleTypeDef *huart, unsigned char *str, unsigned short len)
{
    HAL_UART_Transmit(huart, (uint8_t *)str, len, 1000);
}

/**
 * @brief  调试格式化打印 (自动换行, 最大256字节)
 */
void UsartPrintf(UART_HandleTypeDef *huart, const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    HAL_UART_Transmit(huart, (uint8_t *)buf, (uint16_t)strlen(buf), 1000);
    HAL_UART_Transmit(huart, (uint8_t *)"\r\n", 2, 100);
}
