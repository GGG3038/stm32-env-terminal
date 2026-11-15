/**
 * @file    DHT11.c
 * @brief   DHT11 温湿度采集（单总线，PB4）
 *
 * 时序：主机拉低 ≥18ms → 释放 → 从机 80us 低 + 80us 高应答 → 40bit 数据 + 校验和
 *
 * 关键点：
 *   - 20ms 复位脉冲用 os_task_delay：让出 CPU，其它任务照常跑
 *   - 只有"释放总线 → 应答 → 读完 40bit"这一段（约 5ms）屏蔽 PendSV，
 *     保证时序连续（中间被切走就整帧丢）
 *   - 用 BASEPRI 而不是 PRIMASK 关中断：USART（优先级 6）照常响应，不丢字节
 *   - 失败自动重试一次（DHT11 偶发丢帧很常见）
 */
#include "stm32f1xx_hal.h"
#include "gpio.h"
#include "tim.h"
#include "DHT11.h"
#include "os.h"

#define DHT11_PORT      GPIOB
#define DHT11_PIN       GPIO_PIN_4

/* 一次采集（含重试）总耗时约 25ms，调用方按这个节奏安排即可 */
#define DHT11_TRY_TIMES     2

/* 轻量 RTOS 没有 vTaskSuspendAll()：
   位操作期间把 BASEPRI 抬到优先级 15，屏蔽 SysTick/PendSV（其它任务不会
   被切进来），但 USART（优先级 6）等更高优先级中断照常响应，不会丢字节。
   TIM6 也是 15，HAL 时基在这约 5ms 内暂停，每秒最多丢几个 tick。 */
static uint32_t s_basepri_save;

static void dht_sched_lock(void)
{
	s_basepri_save = __get_BASEPRI();
	__set_BASEPRI(15U << 4);
	__DSB();
	__ISB();
}

static void dht_sched_unlock(void)
{
	__set_BASEPRI(s_basepri_save);
	__DSB();
	__ISB();
}

/* 注意：本函数内含 os_task_delay，必须在调度器启动后（任务里）调用 */
uint8_t dht11_init(void)
{
	GPIO_InitTypeDef gpio = {0};
	uint8_t ret;

	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* TIM1 做微秒延时：72MHz/(71+1)=1MHz，1 tick = 1us */
	htim1.Init.Prescaler = 71;
	htim1.Init.Period = 65535;
	HAL_TIM_Base_Init(&htim1);
	HAL_TIM_Base_Start(&htim1);

	dht11_mode_out();
	HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);

	dht11_rst();
	dht_sched_lock();
	ret = dht11_check();            /* 应答检测也要连续 */
	dht_sched_unlock();

	return ret;
}

void dht11_mode_out(void)
{
	GPIO_InitTypeDef gpio = {0};
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pin = DHT11_PIN;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(DHT11_PORT, &gpio);
}

void dht11_mode_in(void)
{
	GPIO_InitTypeDef gpio = {0};
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pin = DHT11_PIN;
	gpio.Pull = GPIO_PULLUP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(DHT11_PORT, &gpio);
}

void delay_us(uint32_t us)
{
	__HAL_TIM_SetCounter(&htim1, 0);
	while (__HAL_TIM_GetCounter(&htim1) < us);
}

/* 复位脉冲：拉低 20ms 后释放（os_task_delay 让出 CPU，见文件头说明） */
void dht11_rst(void)
{
	dht11_mode_out();
	HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_RESET);
	os_task_delay(20);              /* ≥18ms，DHT11 允许更长 */
	HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
	delay_us(25);                   /* 释放后 20~40us 从机开始应答 */
}

uint8_t dht11_check(void)
{
	uint8_t retry = 0;

	dht11_mode_in();

	while (HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100) {
		retry++;
		delay_us(1);
	}
	if (retry >= 100) return 1;

	retry = 0;
	while (!HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100) {
		retry++;
		delay_us(1);
	}
	if (retry >= 100) return 1;

	return 0;
}

static uint8_t dht11_readbit(void)
{
	uint8_t retry = 0;

	while (HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100) {
		retry++;
		delay_us(1);
	}
	retry = 0;
	while (!HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100) {
		retry++;
		delay_us(1);
	}

	delay_us(40);                   /* 高电平持续 ~70us 为 1，~26us 为 0 */

	return HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) ? 1 : 0;
}

static uint8_t dht11_readbyte(void)
{
	uint8_t i, dat = 0;

	for (i = 0; i < 8; i++) {
		dat <<= 1;
		dat |= dht11_readbit();
	}
	return dat;
}

/* 应答 + 40bit：必须在 dht_sched_lock() 保护下调用 */
static uint8_t dht11_read_bits(uint8_t *temp, uint8_t *hum)
{
	uint8_t buf[5];
	uint8_t i;

	if (dht11_check() != 0) return 1;

	for (i = 0; i < 5; i++) {
		buf[i] = dht11_readbyte();
	}

	if ((uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]) != buf[4]) return 1;

	*hum  = buf[0];
	*temp = buf[2];

	return 0;
}

uint8_t dht11_readdata(uint8_t *temp, uint8_t *hum)
{
	uint8_t ret = 1;
	uint8_t try;

	for (try = 0; try < DHT11_TRY_TIMES && ret != 0; try++) {
		/* 复位脉冲（20ms）：调度器照常运行 */
		dht11_mode_out();
		HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_RESET);
		os_task_delay(20);

		/* 从"释放总线"到"读完 40bit"（约 5ms）必须连续，挂起调度器；
		   中断仍然开着，串口和 tick 不受影响 */
		dht_sched_lock();
		HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
		delay_us(25);
		ret = dht11_read_bits(temp, hum);
		dht_sched_unlock();
	}

	return ret;
}
