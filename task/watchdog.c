/**
 * @file    watchdog.c
 * @brief   独立看门狗 IWDG（LSI 40kHz，约 2s 超时）
 *
 * 直接写寄存器，不依赖 HAL_IWDG 模块（stm32f1xx_hal_conf.h 里没启用它）。
 *   T = (4 × 2^PR) × RLR / f_LSI
 *   PR=4（LSI/64）、RLR=1250 → 40kHz 时 2.0s；
 *   LSI 实际 30~60kHz → 超时约 1.33~2.67s。
 *
 * 系统里最长的阻塞是 Flash 页擦除（20~40ms），余量充足。
 */
#include "stm32f1xx_hal.h"
#include "watchdog.h"

#define IWDG_KEY_UNLOCK   0x5555U   /* 允许写 PR/RLR */
#define IWDG_KEY_FEED     0xAAAAU   /* 重装计数器 */
#define IWDG_KEY_START    0xCCCCU   /* 启动（启动后无法关闭） */

void Watchdog_Init(void)
{
	uint32_t guard = 0;

	IWDG->KR  = IWDG_KEY_UNLOCK;
	IWDG->PR  = 4U;					/* LSI / 64 */
	IWDG->RLR = 1250U;				/* 约 2s */

	/* 等 PR/RLR 同步到 LSI 域（PVU/RVU 清零）；加计数上限防止死等 */
	while ((IWDG->SR != 0U) && (++guard < 0x100000U)) {
	}

	IWDG->KR = IWDG_KEY_FEED;		/* 先装一次计数 */
	IWDG->KR = IWDG_KEY_START;		/* 启动 */
}

void Watchdog_Feed(void)
{
	IWDG->KR = IWDG_KEY_FEED;
}
