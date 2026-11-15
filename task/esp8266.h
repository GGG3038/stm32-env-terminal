#ifndef _ESP8266_H_
#define _ESP8266_H_

#include <stdint.h>

#define REV_OK		0	/* 接收完成 */
#define REV_WAIT	1	/* 接收未完成 */

/* 接收缓冲（中断写、任务读）。不要直接操作，用下面几个接口 */
extern volatile unsigned char  esp8266_buf[512];
extern volatile unsigned short esp8266_cnt;

/* 初始化并连接 WiFi + OneNet TCP：1-成功 0-失败（有限重试，不阻塞死） */
uint8_t ESP8266_Init(void);

/* light sleep 唤醒后重新同步：1-已响应 0-需要重连 */
uint8_t ESP8266_Wake(void);

/* 进 modem sleep（AT+SLEEP=2），低功耗模式睡前调用 */
void ESP8266_Sleep(void);

void ESP8266_Clear(void);

_Bool ESP8266_SendCmd(char *cmd, char *res);

void ESP8266_SendData(unsigned char *data, unsigned short len);

unsigned char *ESP8266_GetIPD(unsigned short timeOut);

_Bool ESP8266_WaitRecive(void);

#endif
