/**
 * @file    power_mgr.c
 * @brief   电源管理实现（SLEEP 方案）
 *
 * 低功耗模式不再使用 STOP：
 *   STOP 会停掉 USART3 的时钟（F1 没有 UART stop-wake 功能），从机睡眠期间
 *   收不到主站查询。本方案改成 SLEEP —— CPU 时钟停、外设全部继续运行，
 *   USART3 收到字节就中断唤醒 CPU，所以 **Modbus 在任何时刻都能应答**。
 *
 *   SLEEP 的进入点在 freertos.c 的 os_idle_hook()（__WFI()）：
 *   没有任务就绪时 CPU 自动停住，任何中断（USART3 / SysTick / TIM6）都能唤醒。
 *
 * 低功耗模式只关"耗电大户"：背光、LCD 显示、ESP8266（modem sleep）。
 * 按键唤醒已去掉。正常模式下模式切换由 Modbus 40008 / 云端 / 触摸屏发起；
 * 低功耗下云端与屏幕都已停车，退出只能靠 Modbus 40008 写 0
 * （模式掉电保存，重新上电仍会恢复低功耗模式）。
 */
#include "power_mgr.h"
#include "app_state.h"
#include "relay.h"
#include "cfg_flash.h"
#include "os.h"
#include "main.h"

/* 自动联动迟滞：0.5 个单位（温度 0.5'C，湿度 0.5%） */
#define AUTO_HYST           5

/* 配置落盘：写命令停止这么久之后才擦写 Flash，避免连写多次只落一次盘 */
#define CFG_SETTLE_MS       500U

/* PB0 = LCD 背光（高电平点亮） */
#define BL_PORT             GPIOB
#define BL_PIN              GPIO_PIN_0

static volatile uint8_t s_park_req = 0;
static volatile uint8_t s_parked[2] = {0, 0};

static volatile uint8_t s_mode_req = 0xFF;  /* 0xFF = 无请求（多任务写，必须 volatile） */
static volatile uint8_t s_save_now = 0;
static uint32_t s_cfg_tick   = 0;
static uint16_t s_last_seen[CFG_IDX_NUM];   /* 上次观察到的配置（用来算静默期） */
static uint16_t s_last_saved[CFG_IDX_NUM];  /* 已写入 Flash 的配置（避免重复擦写） */

/* ===================== 背光 / 模式 ===================== */

void PowerMgr_SetBacklight(uint8_t on)
{
    HAL_GPIO_WritePin(BL_PORT, BL_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    App_SetBacklight(on);
}

void PowerMgr_RequestMode(uint8_t mode)
{
    s_mode_req = (mode == APP_MODE_LOWPOWER) ? APP_MODE_LOWPOWER : APP_MODE_NORMAL;
}

void PowerMgr_RequestSaveNow(void)
{
    s_save_now = 1;
}

uint8_t PowerMgr_ParkReq(void)
{
    return s_park_req;
}

void PowerMgr_ParkAck(uint8_t who, uint8_t parked)
{
    if (who < 2) s_parked[who] = parked ? 1 : 0;
}

/* 模式同时在 app_state（实时）和 cfg（掉电保存）里。
 *
 * 低功耗模式（SLEEP 方案）：
 *   - 置停车请求：显示任务关屏/关背光，云端任务让 ESP8266 进 modem sleep
 *   - MCU 空闲时由 idle hook 自动 __WFI()，USART3 保持运行
 *   - Modbus 全时在线，所以 awake 恒为 1（状态字 b6 保留兼容）
 */
static void apply_mode(uint8_t mode)
{
    App_SetCfg(CFG_MODE, mode);     /* 置 dirty，空闲时落盘 */
    App_SetMode(mode);
    App_SetAwake(1);                /* SLEEP 下 Modbus 始终可通信 */

    s_park_req = (mode == APP_MODE_LOWPOWER) ? 1 : 0;
}

/* ===================== 报警 / 自动联动 ===================== */

static void alarm_update(const app_state_t *s)
{
    uint8_t flags = 0;

    if (s->sensor_ok) {             /* 传感器无效时不误报 */
        int16_t t = s->temp_x10, h = s->hum_x10;
        if (t > (int16_t)s->cfg[CFG_TEMP_HI]) flags |= APP_ALARM_TEMP_HI;
        if (t < (int16_t)s->cfg[CFG_TEMP_LO]) flags |= APP_ALARM_TEMP_LO;
        if (h > (int16_t)s->cfg[CFG_HUM_HI])  flags |= APP_ALARM_HUM_HI;
        if (h < (int16_t)s->cfg[CFG_HUM_LO])  flags |= APP_ALARM_HUM_LO;
    }

    if (flags != s->alarm_flags) App_SetAlarm(flags);

    /* 报警功能关闭时 PC13 是纯手动输出（Modbus 线圈 0x0004） */
    if (s->cfg[CFG_ALARM_EN] & 0x01) {
        Led_Set(flags ? 1 : 0);
    }
}

/* 自动联动（保持寄存器 0x0008 = 1 时生效）：
 *   R1 = 温度超上限（制冷/风扇）  R2 = 湿度超上限（除湿）
 *   R3 = 温度低于下限（加热）     R4 = 任意报警
 * 带 0.5 单位迟滞，避免在阈值附近抖动 */
static void auto_linkage(const app_state_t *s)
{
    uint8_t mask = s->relay_mask;
    int16_t t  = s->temp_x10;
    int16_t h  = s->hum_x10;
    int16_t thi = (int16_t)s->cfg[CFG_TEMP_HI];
    int16_t tlo = (int16_t)s->cfg[CFG_TEMP_LO];
    int16_t hhi = (int16_t)s->cfg[CFG_HUM_HI];

    /* R1 温度高 */
    if (t >= thi)                    mask |= 0x01;
    else if (t <= thi - AUTO_HYST)   mask &= (uint8_t)~0x01;
    /* R2 湿度高 */
    if (h >= hhi)                    mask |= 0x02;
    else if (h <= hhi - AUTO_HYST)   mask &= (uint8_t)~0x02;
    /* R3 温度低 */
    if (t <= tlo)                    mask |= 0x04;
    else if (t >= tlo + AUTO_HYST)   mask &= (uint8_t)~0x04;
    /* R4 任意报警（含湿度过高/过低） */
    if (s->alarm_flags)              mask |= 0x08;
    else                             mask &= (uint8_t)~0x08;

    if (mask != s->relay_mask) Relay_Apply(mask);
}

/* ===================== 配置落盘 ===================== */

static void cfg_commit_check(void)
{
    uint16_t cur[CFG_IDX_NUM];
    uint8_t  i, changed = 0;

    App_CfgExport(cur);

    /* 用"和上次观察到的值比较"来判断最后一次修改时刻，
       这样主站连写 5 条寄存器只会产生一次擦写 */
    for (i = 0; i < CFG_IDX_NUM; i++) {
        if (cur[i] != s_last_seen[i]) {
            s_last_seen[i] = cur[i];
            changed = 1;
        }
    }
    if (changed) s_cfg_tick = HAL_GetTick();

    if (!App_CfgDirty()) return;
    if (!s_save_now && (HAL_GetTick() - s_cfg_tick) < CFG_SETTLE_MS) return;

    /* 与 Flash 中已有内容一致就不擦了（保护 1 万次擦写寿命） */
    for (i = 0; i < CFG_IDX_NUM; i++) {
        if (cur[i] != s_last_saved[i]) break;
    }

    if (i < CFG_IDX_NUM) {
        /* 擦除会停顿 CPU 20~40ms（UsartPrintf/UART 都会抖一下），
           放在最低优先级任务里、且要求配置已静默 500ms 才做 */
        if (CfgFlash_Save()) {
            for (i = 0; i < CFG_IDX_NUM; i++) s_last_saved[i] = cur[i];
            App_CfgDirtyClear();
        } else {
            s_cfg_tick = HAL_GetTick();     /* 失败：500ms 后重试 */
        }
    } else {
        App_CfgDirtyClear();
    }
    s_save_now = 0;
}

/* ===================== 低功耗（SLEEP） =====================
 * 进入低功耗只做一件事：apply_mode() 置停车请求，让显示/云端任务
 * 关掉背光、LCD 显示和 ESP8266。
 *
 * CPU 不需要显式睡眠 —— 空闲任务里的 __WFI() 自动进 SLEEP，
 * 任何中断都能唤醒。USART3 的接收中断保证 Modbus 随时应答，
 * 所以不再需要 RTC 闹钟周期唤醒，也不需要按键唤醒。
 */

/* ===================== 任务 ===================== */

void PowerMgr_Task(void const *argument)
{
    uint32_t tick_1s = 0;

    (void)argument;

    /* --- 任务级初始化（这些 API 会取互斥锁，不能在调度器启动前调用） --- */
    App_CfgExport(s_last_seen);
    {
        uint8_t i;
        for (i = 0; i < CFG_IDX_NUM; i++) s_last_saved[i] = s_last_seen[i];
    }
    /* 恢复掉电前的继电器状态（Relay_Init 里先全关，这里再按存的值打开）。
       不想要这个行为的话，把下面一行注释掉即可 —— 上电永远全部断开 */
    Relay_Apply((uint8_t)App_GetCfg(CFG_RELAY_MASK));
    /* 恢复掉电前的工作模式（低功耗时会顺手让显示/云端停车） */
    apply_mode((uint8_t)App_GetCfg(CFG_MODE));

    for (;;) {
        app_state_t s;
        App_Snapshot(&s);

        /* ---- 1. 模式切换请求（Modbus / 云端 / 触摸屏） ---- */
        if (s_mode_req != 0xFF && s_mode_req != s.mode) {
            apply_mode(s_mode_req);
            s.mode = s_mode_req;
        }
        s_mode_req = 0xFF;

        /* ---- 2. 报警判断 + 自动联动 ---- */
        alarm_update(&s);
        if (s.cfg[CFG_AUTO_CTRL] && s.sensor_ok) {
            auto_linkage(&s);
        }

        /* ---- 3. 运行时间 +1s ---- */
        if ((HAL_GetTick() - tick_1s) >= 1000U) {
            tick_1s = HAL_GetTick();
            App_AddUptime(1);
        }

        /* ---- 4. 配置落盘 ---- */
        cfg_commit_check();

        /* ---- 5. 低功耗：显示/云端已由 apply_mode() 停车，
                   CPU 在空闲任务里 __WFI()，USART3 中断保证 Modbus 在线 ---- */
        os_task_delay(50);
    }
}

/* ===================== 初始化 ===================== */

void PowerMgr_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* PB0 背光，上电先点亮（具体开关由显示任务/配置决定） */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin   = BL_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BL_PORT, &gpio);
    HAL_GPIO_WritePin(BL_PORT, BL_PIN, GPIO_PIN_SET);
}
