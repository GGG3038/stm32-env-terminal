/**
 * @file    app_state.h
 * @brief   全局唯一数据源：所有任务共享的系统状态（互斥锁保护）
 *
 * 读取请用 App_Snapshot() 一次取全量快照（锁内只做 memcpy），
 * 写入请用对应的 App_SetXxx()，不要直接访问内部结构。
 */
#ifndef __APP_STATE_H
#define __APP_STATE_H

#include <stdint.h>

/* 工作模式 */
#define APP_MODE_NORMAL     0
#define APP_MODE_LOWPOWER   1

/* 报警位 (app_state_t.alarm_flags) */
#define APP_ALARM_TEMP_HI   0x01    /* 温度越上限 */
#define APP_ALARM_TEMP_LO   0x02    /* 温度越下限 */
#define APP_ALARM_HUM_HI    0x04    /* 湿度越上限 */
#define APP_ALARM_HUM_LO    0x08    /* 湿度越下限 */

/* 波特率码 */
#define APP_BAUD_4800       0
#define APP_BAUD_9600       1
#define APP_BAUD_19200      2
#define APP_BAUD_38400      3
#define APP_BAUD_115200     4

/* 可写配置项索引（掉电保存，与 Modbus 保持寄存器 0x0000 起一一对应） */
typedef enum {
    CFG_TEMP_HI = 0,        /* 温度上限 ×10     默认 350 */
    CFG_TEMP_LO,            /* 温度下限 ×10     默认 100 */
    CFG_HUM_HI,             /* 湿度上限 ×10     默认 800 */
    CFG_HUM_LO,             /* 湿度下限 ×10     默认 300 */
    CFG_UPLOAD_PERIOD,      /* 云端上传周期 s   默认 30  (5~3600) */
    CFG_SLAVE_ADDR,         /* Modbus 从机地址  默认 1   (1~247) */
    CFG_BAUD_CODE,          /* 波特率码         默认 1=9600 (重启生效) */
    CFG_MODE,               /* 工作模式 0正常/1低功耗  默认 0 */
    CFG_AUTO_CTRL,          /* 自动联动 0手动/1自动    默认 1 */
    CFG_RELAY_MASK,         /* 继电器手动控制字 b0~b3  默认 0 */
    CFG_BACKLIGHT,          /* 背光 0/1         默认 1 */
    CFG_ALARM_EN,           /* 报警使能 b0报警 b1蜂鸣器  默认 0x01 */
    CFG_AWAKE_WINDOW,       /* 保留（SLEEP 方案不再使用，兼容寄存器 0x000D） */
    CFG_IDX_NUM
} cfg_idx_t;

typedef struct {
    /* ---- 传感器 ---- */
    int16_t  temp_x10;          /* 温度 ×10，可负 */
    int16_t  hum_x10;           /* 湿度 ×10 */
    uint8_t  sensor_ok;

    /* ---- 输出 ---- */
    uint8_t  relay_mask;        /* 继电器输出状态 b0~b3 */
    uint8_t  led_on;            /* PC13 LED */
    uint8_t  backlight;         /* 背光 */

    /* ---- 报警 ---- */
    uint8_t  alarm_flags;       /* APP_ALARM_xxx 位组合 */

    /* ---- 模式 / 连接 ---- */
    uint8_t  mode;              /* APP_MODE_xxx */
    uint8_t  awake;             /* 1=Modbus 可通信（SLEEP 方案恒为 1，保留兼容） */
    uint8_t  wifi_conn;
    uint8_t  cloud_conn;

    /* ---- 配置（掉电保存）---- */
    uint16_t cfg[CFG_IDX_NUM];

    /* ---- 统计 ---- */
    uint32_t uptime_s;
    uint32_t mb_frames;         /* Modbus 正常处理帧数 */
    uint32_t mb_errors;         /* Modbus 异常帧数 */
} app_state_t;

/* 创建互斥锁并载入默认配置，需在调度器启动前或任务中调用一次 */
void App_Init(void);

/* 取全量快照（任意任务可调用） */
void App_Snapshot(app_state_t *out);

/* ---- 写入接口 ---- */
void App_SetSensor(int16_t temp_x10, int16_t hum_x10, uint8_t ok);
void App_SetRelay(uint8_t mask);
void App_SetLed(uint8_t on);
void App_SetBacklight(uint8_t on);
void App_SetAlarm(uint8_t flags);
void App_SetMode(uint8_t mode);
void App_SetAwake(uint8_t awake);
void App_SetCloudLink(uint8_t wifi, uint8_t cloud);
void App_AddUptime(uint32_t sec);
void App_CountMbFrame(uint8_t ok);

/* ---- 配置读写 ----
 * App_SetCfg 只做"范围校验 + 存值 + 置 dirty"，不改硬件；
 * 硬件副作用（继电器/背光/模式）由调用方（modbus/ui）按需施加。
 * 返回 1=成功 0=校验失败
 */
uint8_t  App_SetCfg(cfg_idx_t idx, uint16_t val);
uint16_t App_GetCfg(cfg_idx_t idx);
void     App_CfgDefaults(void);
void     App_CfgExport(uint16_t *out);          /* 导出 CFG_IDX_NUM 个 */
void     App_CfgImport(const uint16_t *in);     /* 导入（已校验来源） */
uint8_t  App_CfgDirty(void);
void     App_CfgDirtyClear(void);

/* 配置生效范围检查（给 modbus/ui 做写前校验用） */
uint8_t  App_CfgValid(cfg_idx_t idx, uint16_t val);

#endif /* __APP_STATE_H */
