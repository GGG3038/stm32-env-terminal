/**
 * @file    esp8266.c
 * @brief   ESP8266 AT 指令驱动（USART2 / PA2、PA3）
 *
 * 相对原版的改动：
 *   1. 共享变量加 volatile —— esp8266_buf / esp8266_cnt / rx_data 由中断写、任务读
 *   2. HAL_Delay → os_task_delay —— 原来在 High 优先级任务里忙等，把低优先级的
 *      电源管理和显示任务饿死；os_task_delay 会正常让出 CPU
 *   3. 无限重连 → 有限重试 —— 原来 while(ESP8266_SendCmd(...)) HAL_Delay(500);
 *      WiFi 不在就永远卡在这条循环里，后面的上报代码再也跑不到
 *   4. 新增 ESP8266_Wake()：低功耗唤醒后 ESP 还在 light sleep，第一条 AT 会丢
 */
#include "stm32f1xx_hal.h"
#include "Usart.h"
#include "esp8266.h"
#include "usart_wifi.h"
#include "uart_handler.h"
#include "app_state.h"
#include "os.h"
#include "app_secrets.h"
#include <string.h>
#include <stdio.h>

/* WiFi 账号密码 / OneNet 的 MQTT 接入点（真实值在 app_secrets.h，不入库） */
#define ESP8266_WIFI_INFO		"AT+CWJAP=\"" ESP8266_WIFI_SSID "\",\"" ESP8266_WIFI_PASS "\"\r\n"
/* 新版 OneNet 的产品专属接入点（控制台"设备接入地址"里显示的值） */
#define ESP8266_ONENET_INFO		"AT+CIPSTART=\"TCP\",\"" ONENET_PROID ".mqtts.acc.cmcconenet.cn\",1883\r\n"

/* 每条 AT 指令的重试次数与间隔 */
#define ESP_AT_TRIES			3
#define ESP_AT_GAP_MS			500

/* 中断写、任务读 —— 必须 volatile */
volatile unsigned char  esp8266_buf[512];
volatile unsigned short esp8266_cnt = 0, esp8266_cntPre = 0;
volatile uint8_t        rx_data;

/* 给 strstr/cJSON 这类库函数用的"非 volatile 视图" */
static char *esp_buf(void)
{
	return (char *)(void *)esp8266_buf;
}

//==========================================================
//	函数名称：	ESP8266_Clear
//	函数功能：	清空缓存
//==========================================================
void ESP8266_Clear(void)
{
	memset((void *)esp8266_buf, 0, sizeof(esp8266_buf));
	esp8266_cnt = 0;
	esp8266_cntPre = 0;
}

//==========================================================
//	函数名称：	ESP8266_WaitRecive
//	函数功能：	等待接收完成（两轮计数相同 = 这帧收完了）
//	返回值：	REV_OK-接收完成		REV_WAIT-还在收
//==========================================================
_Bool ESP8266_WaitRecive(void)
{
	if (esp8266_cnt == 0)					/* 一字节都没收到 */
		return REV_WAIT;

	if (esp8266_cnt == esp8266_cntPre) {	/* 和上次相同：收完了 */
		esp8266_cnt = 0;
		return REV_OK;
	}

	esp8266_cntPre = esp8266_cnt;
	return REV_WAIT;
}

//==========================================================
//	函数名称：	ESP8266_SendCmd
//	函数功能：	发送指令并等待应答关键字
//	返回值：	0-成功	1-失败（超时，最长约 2s）
//==========================================================
_Bool ESP8266_SendCmd(char *cmd, char *res)
{
	unsigned short timeOut = 200;

	Usart_SendString(&huart2, (unsigned char *)cmd, strlen((const char *)cmd));

	while (timeOut--) {
		if (ESP8266_WaitRecive() == REV_OK) {
			if (strstr(esp_buf(), res) != NULL) {
				ESP8266_Clear();
				return 0;
			}
		}
		os_task_delay(10);
	}

	return 1;
}

//==========================================================
//	函数名称：	ESP8266_SendData
//	函数功能：	透传发送数据（先 AT+CIPSEND 再发内容）
//==========================================================
void ESP8266_SendData(unsigned char *data, unsigned short len)
{
	char cmdBuf[32];

	ESP8266_Clear();
	sprintf(cmdBuf, "AT+CIPSEND=%d\r\n", len);
	if (!ESP8266_SendCmd(cmdBuf, ">")) {	/* 收到 '>' 才能发 */
		Usart_SendString(&huart2, data, len);
	}
}

//==========================================================
//	函数名称：	ESP8266_GetIPD
//	函数功能：	取平台返回的数据（+IPD,x:yyy 里的 yyy）
//	入口参数：	timeOut：等待次数（每次 10ms）
//==========================================================
unsigned char *ESP8266_GetIPD(unsigned short timeOut)
{
	char *ptrIPD;
	char *buf = esp_buf();

	do {
		if (ESP8266_WaitRecive() == REV_OK) {
			ptrIPD = strstr(buf, "IPD,");
			if (ptrIPD == NULL) {
				UsartPrintf(USART_DEBUG, "\"IPD\" not found");
			} else {
				ptrIPD = strchr(ptrIPD, ':');
				if (ptrIPD != NULL) return (unsigned char *)(ptrIPD + 1);
				return NULL;
			}
		}

		os_task_delay(10);
	} while (--timeOut);

	return NULL;
}

//==========================================================
//	函数名称：	ESP8266_Wake
//	函数功能：	light sleep 唤醒后重新同步（第一条 AT 常丢）
//	返回值：	1-已响应	0-无响应（需要重连）
//==========================================================
uint8_t ESP8266_Wake(void)
{
	uint8_t i;

	for (i = 0; i < 5; i++) {
		if (ESP8266_SendCmd("AT\r\n", "OK") == 0) return 1;
		os_task_delay(200);
	}
	return 0;
}

//==========================================================
//	函数名称：	ESP8266_Sleep
//	函数功能：	进 modem sleep（保持 WiFi 关联和 TCP 连接，只是少收包）
//	说明：	低功耗模式下车前调用；唤醒用 ESP8266_Wake()
//==========================================================
void ESP8266_Sleep(void)
{
	(void)ESP8266_SendCmd("AT+SLEEP=2\r\n", "OK");
}

//==========================================================
//	函数名称：	esp_at_step
//	函数功能：	单条 AT 指令 + 有限重试
//==========================================================
static uint8_t esp_at_step(const char *tag, char *cmd, char *res)
{
	uint8_t i;

	for (i = 0; i < ESP_AT_TRIES; i++) {
		if (ESP8266_SendCmd(cmd, res) == 0) {
			UsartPrintf(USART_DEBUG, "%s OK", tag);
			return 1;
		}
		UsartPrintf(USART_DEBUG, "%s retry %d/%d", tag, i + 1, ESP_AT_TRIES);
		os_task_delay(ESP_AT_GAP_MS);
	}

	UsartPrintf(USART_DEBUG, "%s FAIL", tag);
	return 0;
}

//==========================================================
//	函数名称：	ESP8266_Init
//	函数功能：	初始化 ESP8266 并连上 WiFi + OneNet TCP
//	返回值：	1-成功	0-失败（调用方过一会儿再试，不会卡死在这）
//==========================================================
uint8_t ESP8266_Init(void)
{
	ESP8266_Clear();

	/* 启动 USART2 中断接收（每次 1 字节，存 esp8266_buf） */
	(void)HAL_UART_Receive_IT(&huart2, (uint8_t *)&rx_data, 1);

	if (!esp_at_step("0.ATE0", "ATE0\r\n", "OK")) return 0;				/* 关回显 */
	if (!esp_at_step("1.AT", "AT\r\n", "OK")) return 0;					/* 在线确认 */
	if (!esp_at_step("2.CWMODE", "AT+CWMODE=1\r\n", "OK")) return 0;	/* STA 模式 */
	if (!esp_at_step("3.CWDHCP", "AT+CWDHCP=1,1\r\n", "OK")) return 0;	/* DHCP */
	if (!esp_at_step("4.CWJAP", ESP8266_WIFI_INFO, "GOT IP")) {			/* 连 WiFi */
		App_SetCloudLink(0, 0);
		return 0;
	}

	App_SetCloudLink(1, 0);												/* WiFi 通了 */

	if (!esp_at_step("5.CIPSTART", ESP8266_ONENET_INFO, "CONNECT")) {	/* 连 OneNet TCP */
		return 0;
	}

	UsartPrintf(USART_DEBUG, "6. ESP8266 ready");
	return 1;
}
