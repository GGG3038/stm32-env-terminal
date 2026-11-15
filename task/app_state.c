/**
 * @file    app_state.c
 * @brief   全局唯一数据源实现（互斥锁保护）
 */
#include "app_state.h"
#include "os.h"
#include <string.h>

static app_state_t s_state;
static volatile uint8_t s_cfg_dirty = 0;

static os_mutex_t s_mutex;
static uint8_t s_mutex_ready = 0;

/* 默认配置 */
static const uint16_t s_defaults[CFG_IDX_NUM] = {
    [CFG_TEMP_HI]       = 350,      /* 35.0'C */
    [CFG_TEMP_LO]       = 100,      /* 10.0'C */
    [CFG_HUM_HI]        = 800,      /* 80.0%  */
    [CFG_HUM_LO]        = 300,      /* 30.0%  */
    [CFG_UPLOAD_PERIOD] = 30,       /* 30s    */
    [CFG_SLAVE_ADDR]    = 1,
    [CFG_BAUD_CODE]     = APP_BAUD_9600,
    [CFG_MODE]          = APP_MODE_NORMAL,
    [CFG_AUTO_CTRL]     = 0,        /* 默认手动：上电后继电器只受手动命令控制，
                                       自动联动需在设置页/写 0x0008=1 才开启 */
    [CFG_RELAY_MASK]    = 0,
    [CFG_BACKLIGHT]     = 1,
    [CFG_ALARM_EN]      = 0x01,     /* 报警使能，蜂鸣器关 */
    [CFG_AWAKE_WINDOW]  = 5,
};

static void lock(void)
{
    /* 调度器启动前是单线程，os_mutex_lock 会返回 OS_ERR_STATE，直接忽略 */
    if (s_mutex_ready) (void)os_mutex_lock(&s_mutex, OS_WAIT_FOREVER);
}

static void unlock(void)
{
    if (s_mutex_ready) (void)os_mutex_unlock(&s_mutex);
}

void App_Init(void)
{
    if (!s_mutex_ready && os_mutex_init(&s_mutex) == OS_OK) {
        s_mutex_ready = 1;
    }
    memset(&s_state, 0, sizeof(s_state));
    App_CfgDefaults();
    s_state.backlight = 1;
    s_state.sensor_ok  = 0;
}

void App_Snapshot(app_state_t *out)
{
    if (out == NULL) return;
    lock();
    memcpy(out, &s_state, sizeof(app_state_t));
    unlock();
}

/* ---------------- 写入接口 ---------------- */

void App_SetSensor(int16_t temp_x10, int16_t hum_x10, uint8_t ok)
{
    lock();
    if (ok) {
        s_state.temp_x10 = temp_x10;
        s_state.hum_x10  = hum_x10;
    }
    s_state.sensor_ok = ok;
    unlock();
}

void App_SetRelay(uint8_t mask)
{
    lock();
    s_state.relay_mask = mask & 0x0F;
    /* cfg[CFG_RELAY_MASK] 是继电器状态的掉电镜像：这里同步但不置 dirty，
       避免每次继电器动作都擦写 Flash（寿命 1 万次），
       它会随下一次真正的配置保存一起写入，重启后恢复继电器状态 */
    s_state.cfg[CFG_RELAY_MASK] = s_state.relay_mask;
    unlock();
}

void App_SetLed(uint8_t on)
{
    lock();
    s_state.led_on = on ? 1 : 0;
    unlock();
}

void App_SetBacklight(uint8_t on)
{
    lock();
    s_state.backlight = on ? 1 : 0;
    unlock();
}

void App_SetAlarm(uint8_t flags)
{
    lock();
    s_state.alarm_flags = flags;
    unlock();
}

void App_SetMode(uint8_t mode)
{
    lock();
    s_state.mode = (mode == APP_MODE_LOWPOWER) ? APP_MODE_LOWPOWER : APP_MODE_NORMAL;
    unlock();
}

void App_SetAwake(uint8_t awake)
{
    lock();
    s_state.awake = awake ? 1 : 0;
    unlock();
}

void App_SetCloudLink(uint8_t wifi, uint8_t cloud)
{
    lock();
    s_state.wifi_conn  = wifi ? 1 : 0;
    s_state.cloud_conn = cloud ? 1 : 0;
    unlock();
}

void App_AddUptime(uint32_t sec)
{
    lock();
    s_state.uptime_s += sec;
    unlock();
}

void App_CountMbFrame(uint8_t ok)
{
    lock();
    if (ok) s_state.mb_frames++;
    else    s_state.mb_errors++;
    unlock();
}

/* ---------------- 配置读写 ---------------- */

uint8_t App_CfgValid(cfg_idx_t idx, uint16_t val)
{
    int16_t sv = (int16_t)val;
    uint16_t hi, lo;

    switch (idx) {
    case CFG_TEMP_HI:
    case CFG_TEMP_LO:
        if (sv < -400 || sv > 1250) return 0;
        /* 上下限互斥检查 */
        hi = s_state.cfg[CFG_TEMP_HI];
        lo = s_state.cfg[CFG_TEMP_LO];
        if (idx == CFG_TEMP_HI) hi = val; else lo = val;
        if ((int16_t)lo > (int16_t)hi) return 0;
        return 1;

    case CFG_HUM_HI:
    case CFG_HUM_LO:
        if (sv < 0 || sv > 1000) return 0;
        hi = s_state.cfg[CFG_HUM_HI];
        lo = s_state.cfg[CFG_HUM_LO];
        if (idx == CFG_HUM_HI) hi = val; else lo = val;
        if ((int16_t)lo > (int16_t)hi) return 0;
        return 1;

    case CFG_UPLOAD_PERIOD:  return (val >= 5 && val <= 3600);
    case CFG_SLAVE_ADDR:     return (val >= 1 && val <= 247);
    case CFG_BAUD_CODE:      return (val <= APP_BAUD_115200);
    case CFG_MODE:           return (val <= APP_MODE_LOWPOWER);
    case CFG_AUTO_CTRL:      return (val <= 1);
    case CFG_RELAY_MASK:     return (val <= 0x0F);
    case CFG_BACKLIGHT:      return (val <= 1);
    case CFG_ALARM_EN:       return (val <= 0x03);
    case CFG_AWAKE_WINDOW:   return (val >= 1 && val <= 60);
    default:                 return 0;
    }
}

uint8_t App_SetCfg(cfg_idx_t idx, uint16_t val)
{
    if (idx >= CFG_IDX_NUM) return 0;

    lock();
    if (!App_CfgValid(idx, val)) {
        unlock();
        return 0;
    }
    s_state.cfg[idx] = val;
    unlock();

    s_cfg_dirty = 1;
    return 1;
}

uint16_t App_GetCfg(cfg_idx_t idx)
{
    uint16_t v;
    if (idx >= CFG_IDX_NUM) return 0;
    lock();
    v = s_state.cfg[idx];
    unlock();
    return v;
}

void App_CfgDefaults(void)
{
    lock();
    memcpy(s_state.cfg, s_defaults, sizeof(s_state.cfg));
    unlock();
    s_cfg_dirty = 1;
}

void App_CfgExport(uint16_t *out)
{
    lock();
    memcpy(out, s_state.cfg, sizeof(s_state.cfg));
    unlock();
}

void App_CfgImport(const uint16_t *in)
{
    uint8_t i;
    for (i = 0; i < CFG_IDX_NUM; i++) {
        lock();
        if (App_CfgValid((cfg_idx_t)i, in[i])) {
            s_state.cfg[i] = in[i];
        }
        unlock();
    }
    s_cfg_dirty = 0;    /* 刚载入的不需要回写 */
}

uint8_t App_CfgDirty(void)
{
    return s_cfg_dirty;
}

void App_CfgDirtyClear(void)
{
    s_cfg_dirty = 0;
}
