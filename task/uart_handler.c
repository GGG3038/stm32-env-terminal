/**
 * @file    uart_handler.c
 * @brief   UART 中断回调集中处理
 *
 *   USART2 —— ESP8266，逐字节中断接收，存 esp8266_buf
 *   USART3 —— Modbus RTU 从机，ReceiveToIdle（IDLE 空闲线定帧）
 *
 * 为什么必须补错误回调：
 *   F1 的 HAL 一旦遇到溢出（ORE，比如某次任务长时间关中断，数据没被及时取走），
 *   会调用 UART_EndRxTransfer() 把 RxState 置回 READY 并**停止接收**，
 *   没有 HAL_UART_ErrorCallback 就再也挂不上接收 —— 现象是"串口从此哑掉"。
 *   所以每个错误分支都要：清标志 → 重新挂接收。
 */
#include "uart_handler.h"
#include "esp8266.h"
#include "usart.h"
#include "modbus_rtu.h"

/* 中断与任务共享的变量，必须是 volatile */
extern volatile unsigned char  esp8266_buf[512];
extern volatile unsigned short esp8266_cnt;
extern volatile uint8_t        rx_data;

/**
 * @brief  接收完成回调（USART2 每收到 1 字节进一次）
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        if (esp8266_cnt < (sizeof(esp8266_buf) - 1U)) {
            esp8266_buf[esp8266_cnt++] = rx_data;
        }
        (void)HAL_UART_Receive_IT(&huart2, (uint8_t *)&rx_data, 1);  /* 重新挂接收 */
    }
}

/**
 * @brief  ReceiveToIdle 事件回调
 *         USART3 收到一帧后 IDLE 中断触发，Size = 本次实际收到的字节数。
 *         注意：HAL 在 IDLE 事件后已把 RxState 置回 READY 并停止接收，
 *         必须在这里重新挂 HAL_UARTEx_ReceiveToIdle_IT，否则只能收一帧。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART3) {
        Modbus_RxEventCallback(Size);       /* 数据在 modbus 模块自己的缓冲里 */
        Modbus_RxRearm();
    }
}

/**
 * @brief  错误回调（ORE 溢出 / FE 帧错误 / NE 噪声 / PE 校验错）
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        __HAL_UART_CLEAR_PEFLAG(huart);     /* F1：读 SR/DR 序列，清 PE/FE/NE/ORE */
        __HAL_UART_CLEAR_OREFLAG(huart);
        esp8266_cnt = 0;                    /* 这一帧已经乱了，丢掉重新收 */
        (void)HAL_UART_Receive_IT(&huart2, (uint8_t *)&rx_data, 1);
    } else if (huart->Instance == USART3) {
        __HAL_UART_CLEAR_PEFLAG(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        Modbus_RxReset();                   /* 丢半帧 + 重新挂 ReceiveToIdle */
    }
}
