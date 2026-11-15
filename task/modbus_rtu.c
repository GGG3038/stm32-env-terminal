/**
 * @file    modbus_rtu.c
 * @brief   Modbus RTU 从机协议栈
 *
 * 收帧方案：HAL_UARTEx_ReceiveToIdle_IT（IDLE 中断定帧）
 *   - 中断里把本段数据追加到组帧缓冲，记录时间戳
 *   - 任务里校验从"最后一字节"起已静默 t3.5，才认为一帧收完（符合 Modbus 规范）
 *   - HAL 在 IDLE 事件后会自行停止接收，必须由回调重新挂 ReceiveToIdle
 *
 * 应答：在 Modbus 任务内阻塞发送（9600 下 8 字节约 8ms），响应时间 ~ t3.5+2ms
 */
#include "modbus_rtu.h"
#include "app_state.h"
#include "relay.h"
#include "power_mgr.h"
#include "os.h"
#include "usart.h"      /* huart3 */
#include "main.h"
#include <string.h>

/* ---------------- 功能码 / 异常码 ---------------- */
#define FC_READ_COILS       0x01
#define FC_READ_DISCRETE    0x02
#define FC_READ_HOLDING     0x03
#define FC_READ_INPUT       0x04
#define FC_WRITE_COIL       0x05
#define FC_WRITE_REG        0x06
#define FC_WRITE_COILS      0x0F
#define FC_WRITE_REGS       0x10

#define EXC_ILLEGAL_FUNC    0x01
#define EXC_ILLEGAL_ADDR    0x02
#define EXC_ILLEGAL_VALUE   0x03
#define EXC_SLAVE_FAULT     0x04

/* 命令寄存器（保持寄存器 0x000C）取值 */
#define MB_CMD_SAVE         1
#define MB_CMD_DEFAULTS     2
#define MB_CMD_RESET        3

#define MB_BUF_SIZE         256
#define MB_MAX_REGS         125

/* ---------------- 模块状态 ---------------- */
static uint8_t  s_rx_buf[MB_BUF_SIZE];      /* HAL 接收缓冲 */
static uint8_t  s_frame[MB_BUF_SIZE];       /* 组帧缓冲 */
static volatile uint16_t s_frame_len;
static volatile uint32_t s_frame_tick;      /* 最后一字节到达时刻 */
static volatile uint8_t  s_frame_ready;

static uint32_t s_t35_ms  = 5;              /* t3.5 静默时间（ms） */
static uint8_t  s_addr    = 1;              /* 从机地址 */

/* 应答发完才执行的动作 */
static uint8_t  s_do_save;
static uint8_t  s_do_defaults;
static uint8_t  s_do_reset;

/* 波特率码 → 实际波特率（115200 放不进 uint16，用 uint32） */
static const uint32_t s_baud_tab[5] = {4800, 9600, 19200, 38400, 115200};

/* 保持寄存器地址 → 配置项索引（0xFF = 命令寄存器，不落盘） */
static const uint8_t s_hold_map[MB_HOLD_REG_NUM] = {
    CFG_TEMP_HI,        /* 0x0000 温度上限 ×10 */
    CFG_TEMP_LO,        /* 0x0001 温度下限 ×10 */
    CFG_HUM_HI,         /* 0x0002 湿度上限 ×10 */
    CFG_HUM_LO,         /* 0x0003 湿度下限 ×10 */
    CFG_UPLOAD_PERIOD,  /* 0x0004 云端上传周期 s */
    CFG_SLAVE_ADDR,     /* 0x0005 从机地址 1~247（立即生效） */
    CFG_BAUD_CODE,      /* 0x0006 波特率码（重启生效） */
    CFG_MODE,           /* 0x0007 工作模式 0 正常 / 1 低功耗 */
    CFG_AUTO_CTRL,      /* 0x0008 自动联动使能 */
    CFG_RELAY_MASK,     /* 0x0009 继电器手动控制字（自动模式=1 时拒写） */
    CFG_BACKLIGHT,      /* 0x000A 背光 */
    CFG_ALARM_EN,       /* 0x000B 报警使能 */
    0xFF,               /* 0x000C 命令：1 保存 2 恢复默认 3 重启 */
    CFG_AWAKE_WINDOW,   /* 0x000D 保留（SLEEP 方案不再使用） */
};

/* ---------------- CRC ----------------
 * 返回值为"整段数据的 CRC"，校验时对含 CRC 的整帧计算结果应为 0 */
uint16_t Crc16_Modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    uint16_t i;
    uint8_t  j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1U) crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            else          crc >>= 1;
        }
    }
    return crc;
}

uint8_t Modbus_GetSlaveAddr(void)
{
    return s_addr;
}

/* ---------------- 接收 ---------------- */
static void mb_arm_rx(void)
{
    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart3, s_rx_buf, MB_BUF_SIZE);
}

/* UART 出错（ORE/FE/NE/PE）后由 uart_handler 调用：丢掉半帧并重新挂接收 */
void Modbus_RxReset(void)
{
    s_frame_len   = 0;
    s_frame_ready = 0;
    mb_arm_rx();
}

/* 正常 IDLE 定帧后重新挂接收（不丢组帧缓冲） */
void Modbus_RxRearm(void)
{
    mb_arm_rx();
}

/* 由 uart_handler.c 的 HAL_UARTEx_RxEventCallback 调用（中断上下文） */
void Modbus_RxEventCallback(uint16_t len)
{
    uint16_t i;

    if (len > MB_BUF_SIZE) len = MB_BUF_SIZE;

    /* 上一帧已收满 t3.5 却还没被任务取走：直接开新帧，
       防止两帧粘在一起（正常情况下任务 1ms 内就会取走） */
    if (s_frame_ready && ((HAL_GetTick() - s_frame_tick) >= s_t35_ms)) {
        s_frame_len = 0;
    }

    for (i = 0; i < len; i++) {
        if (s_frame_len < MB_BUF_SIZE) {
            s_frame[s_frame_len++] = s_rx_buf[i];
        }
    }
    s_frame_tick  = HAL_GetTick();
    s_frame_ready = 1;
}

/* ---------------- 发送 ---------------- */
static void mb_transmit(uint8_t *buf, uint16_t len)
{
    uint16_t crc = Crc16_Modbus(buf, len);

    if (len + 2U > MB_BUF_SIZE) return;
    buf[len++] = (uint8_t)(crc & 0xFFU);
    buf[len++] = (uint8_t)(crc >> 8);

#if MODBUS_USE_DE_PIN
    {
        volatile uint8_t k;
        HAL_GPIO_WritePin(MODBUS_DE_PORT, MODBUS_DE_PIN, GPIO_PIN_SET);
        for (k = 0; k < 40; k++) { __NOP(); }       /* 等 DE 建立（约 1us） */
    }
#endif

    (void)HAL_UART_Transmit(&huart3, buf, len, 200);

    /* 等最后一个字节移位完成，避免过早释放总线/关 DE */
    {
        uint32_t t0 = HAL_GetTick();
        while (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_TC) == RESET) {
            if ((HAL_GetTick() - t0) > 20U) break;
        }
    }

#if MODBUS_USE_DE_PIN
    HAL_GPIO_WritePin(MODBUS_DE_PORT, MODBUS_DE_PIN, GPIO_PIN_RESET);
#endif
}

/* ---------------- 寄存器视图 ---------------- */
static void mb_fill_input(uint16_t *r, const app_state_t *s)
{
    uint16_t st = 0;

    if (s->sensor_ok)                  st |= 0x0001;    /* b0 传感器正常 */
    if (s->alarm_flags)                st |= 0x0002;    /* b1 报警中 */
    if (s->mode == APP_MODE_LOWPOWER)  st |= 0x0004;    /* b2 低功耗模式 */
    if (s->wifi_conn)                  st |= 0x0008;    /* b3 WiFi 已连 */
    if (s->cloud_conn)                 st |= 0x0010;    /* b4 云端已连 */
    if (s->backlight)                  st |= 0x0020;    /* b5 背光 */
    if (s->awake)                      st |= 0x0040;    /* b6 Modbus 可通信（SLEEP 方案恒为 1） */

    r[0] = (uint16_t)s->temp_x10;                       /* 温度 ×10，有符号 */
    r[1] = (uint16_t)s->hum_x10;                        /* 湿度 ×10 */
    r[2] = st;                                          /* 状态字 */
    r[3] = (uint16_t)(Relay_GetActual() | (s->led_on ? 0x10 : 0));  /* 继电器实际电平+LED */
    r[4] = s->alarm_flags;                              /* 报警明细 */
    r[5] = MB_FW_VERSION;
    r[6] = (uint16_t)(s->uptime_s >> 16);               /* 运行时间 s (Hi) */
    r[7] = (uint16_t)(s->uptime_s & 0xFFFFU);           /* 运行时间 s (Lo) */
}

static uint16_t mb_hold_read(uint8_t idx, const app_state_t *s)
{
    uint8_t m = s_hold_map[idx];
    if (m == 0xFF) return 0;            /* 命令寄存器读回 0 */
    return s->cfg[m];
}

static uint8_t mb_coil_get(uint8_t idx, const app_state_t *s)
{
    switch (idx) {
    case 0: case 1: case 2: case 3: return (uint8_t)((s->relay_mask >> idx) & 1U);
    case 4: return s->led_on ? 1 : 0;
    case 5: return s->backlight ? 1 : 0;
    default: return 0;
    }
}

static uint8_t mb_discrete_get(uint8_t idx, const app_state_t *s)
{
    switch (idx) {
    case 0: case 1: case 2: case 3: return (uint8_t)((Relay_GetActual() >> idx) & 1U);
    case 4: return s->alarm_flags ? 1 : 0;
    case 5: return s->sensor_ok ? 1 : 0;
    default: return 0;
    }
}

/* 单笔保持寄存器写入校验：merged 是"含本帧已接受新值"的配置视图，
   这样多寄存器连写时上下限用一组新值一起判断 */
static uint8_t mb_hold_valid(uint16_t *merged, uint8_t idx, uint16_t val)
{
    uint8_t m = s_hold_map[idx];

    if (m == 0xFF) {                    /* 命令寄存器 */
        return (val == MB_CMD_SAVE || val == MB_CMD_DEFAULTS || val == MB_CMD_RESET);
    }

    if (m == CFG_RELAY_MASK && merged[CFG_AUTO_CTRL] != 0) {
        return 0;                       /* 自动联动态下由系统控制，拒绝手动写 */
    }

    if (m == CFG_TEMP_HI || m == CFG_TEMP_LO) {
        if ((int16_t)val < -400 || (int16_t)val > 1250) return 0;
        merged[m] = val;
        return ((int16_t)merged[CFG_TEMP_LO] <= (int16_t)merged[CFG_TEMP_HI]);
    }
    if (m == CFG_HUM_HI || m == CFG_HUM_LO) {
        if ((int16_t)val < 0 || (int16_t)val > 1000) return 0;
        merged[m] = val;
        return ((int16_t)merged[CFG_HUM_LO] <= (int16_t)merged[CFG_HUM_HI]);
    }

    if (App_CfgValid((cfg_idx_t)m, val)) {
        merged[m] = val;
        return 1;
    }
    return 0;
}

/* 上下限成对写入：先写不会与旧值冲突的一侧，避免中间态被判越界 */
static void mb_apply_cross(cfg_idx_t hi_i, cfg_idx_t lo_i,
                           uint16_t hi, uint16_t lo, uint8_t hi_ch, uint8_t lo_ch)
{
    if (hi_ch && lo_ch) {
        if ((int16_t)hi >= (int16_t)App_GetCfg(lo_i)) {
            App_SetCfg(hi_i, hi);
            App_SetCfg(lo_i, lo);
        } else {
            App_SetCfg(lo_i, lo);
            App_SetCfg(hi_i, hi);
        }
    } else if (hi_ch) {
        App_SetCfg(hi_i, hi);
    } else {
        App_SetCfg(lo_i, lo);
    }
}

/* 把 merged 视图落到配置 + 硬件副作用。返回异常码，0 = 无异常 */
static uint8_t mb_commit(uint16_t *merged)
{
    uint8_t changed[CFG_IDX_NUM];
    uint8_t i;

    for (i = 0; i < CFG_IDX_NUM; i++) {
        changed[i] = (merged[i] != App_GetCfg((cfg_idx_t)i)) ? 1 : 0;
    }

    if (changed[CFG_TEMP_HI] || changed[CFG_TEMP_LO]) {
        mb_apply_cross(CFG_TEMP_HI, CFG_TEMP_LO, merged[CFG_TEMP_HI], merged[CFG_TEMP_LO],
                       changed[CFG_TEMP_HI], changed[CFG_TEMP_LO]);
    }
    if (changed[CFG_HUM_HI] || changed[CFG_HUM_LO]) {
        mb_apply_cross(CFG_HUM_HI, CFG_HUM_LO, merged[CFG_HUM_HI], merged[CFG_HUM_LO],
                       changed[CFG_HUM_HI], changed[CFG_HUM_LO]);
    }

    for (i = 0; i < CFG_IDX_NUM; i++) {
        if (i == CFG_TEMP_HI || i == CFG_TEMP_LO || i == CFG_HUM_HI || i == CFG_HUM_LO) continue;
        if (!changed[i]) continue;
        if (!App_SetCfg((cfg_idx_t)i, merged[i])) return EXC_SLAVE_FAULT;

        /* 硬件副作用：状态类配置立即生效 */
        switch ((cfg_idx_t)i) {
        case CFG_RELAY_MASK: Relay_Apply((uint8_t)merged[i]);               break;
        case CFG_BACKLIGHT:  PowerMgr_SetBacklight((uint8_t)merged[i]);     break;
        case CFG_MODE:       PowerMgr_RequestMode((uint8_t)merged[i]);      break;
        case CFG_SLAVE_ADDR: s_addr = (uint8_t)merged[i];                   break;
        default: break;     /* 波特率：重启后生效 */
        }
    }
    return 0;
}

/* ---------------- 应答组帧 ---------------- */
static uint16_t mb_exception(uint8_t *tx, uint8_t addr, uint8_t func, uint8_t code)
{
    tx[0] = addr;
    tx[1] = (uint8_t)(func | 0x80U);
    tx[2] = code;
    return 3;
}

/* ---------------- 帧处理 ---------------- */
static void mb_process(uint8_t *f, uint16_t len)
{
    uint8_t  addr = f[0];
    uint8_t  func = f[1];
    uint8_t  broadcast = (addr == 0) ? 1 : 0;
    uint8_t  tx[MB_BUF_SIZE];
    uint16_t txlen = 0;
    uint16_t start, qty, i;
    app_state_t snap;
    uint8_t  exc = 0;

    switch (func) {

    /* ---- 读线圈 / 读离散输入 ---- */
    case FC_READ_COILS:
    case FC_READ_DISCRETE: {
        uint8_t nbytes;

        if (len != 8) { exc = EXC_ILLEGAL_VALUE; break; }
        start = (uint16_t)((f[2] << 8) | f[3]);
        qty   = (uint16_t)((f[4] << 8) | f[5]);
        if (qty < 1 || qty > 2000) { exc = EXC_ILLEGAL_VALUE; break; }
        if (start + qty > ((func == FC_READ_COILS) ? MB_COIL_NUM : MB_DISCRETE_NUM)) {
            exc = EXC_ILLEGAL_ADDR; break;
        }

        App_Snapshot(&snap);
        nbytes = (uint8_t)((qty + 7) / 8);
        tx[0] = addr; tx[1] = func; tx[2] = nbytes;
        memset(&tx[3], 0, nbytes);
        for (i = 0; i < qty; i++) {
            uint8_t bit = (func == FC_READ_COILS)
                        ? mb_coil_get((uint8_t)(start + i), &snap)
                        : mb_discrete_get((uint8_t)(start + i), &snap);
            if (bit) tx[3 + (i / 8)] |= (uint8_t)(1U << (i % 8));
        }
        txlen = (uint16_t)(3 + nbytes);
        break;
    }

    /* ---- 读保持 / 读输入寄存器 ---- */
    case FC_READ_HOLDING:
    case FC_READ_INPUT: {
        uint8_t  nreg = (func == FC_READ_HOLDING) ? MB_HOLD_REG_NUM : MB_INPUT_REG_NUM;
        uint16_t regs[16];

        if (len != 8) { exc = EXC_ILLEGAL_VALUE; break; }
        start = (uint16_t)((f[2] << 8) | f[3]);
        qty   = (uint16_t)((f[4] << 8) | f[5]);
        if (qty < 1 || qty > MB_MAX_REGS) { exc = EXC_ILLEGAL_VALUE; break; }
        if (start + qty > nreg) { exc = EXC_ILLEGAL_ADDR; break; }

        App_Snapshot(&snap);
        if (func == FC_READ_HOLDING) {
            for (i = 0; i < qty; i++) {
                regs[i] = mb_hold_read((uint8_t)(start + i), &snap);
            }
        } else {
            uint16_t all[MB_INPUT_REG_NUM];
            mb_fill_input(all, &snap);
            for (i = 0; i < qty; i++) {
                regs[i] = all[start + i];
            }
        }

        tx[0] = addr; tx[1] = func; tx[2] = (uint8_t)(qty * 2);
        for (i = 0; i < qty; i++) {
            tx[3 + i * 2] = (uint8_t)(regs[i] >> 8);
            tx[4 + i * 2] = (uint8_t)(regs[i] & 0xFF);
        }
        txlen = (uint16_t)(3 + qty * 2);
        break;
    }

    /* ---- 写单个线圈 ---- */
    case FC_WRITE_COIL: {
        uint16_t val;

        if (len != 8) { exc = EXC_ILLEGAL_VALUE; break; }
        start = (uint16_t)((f[2] << 8) | f[3]);
        val   = (uint16_t)((f[4] << 8) | f[5]);
        if (start >= MB_COIL_NUM) { exc = EXC_ILLEGAL_ADDR; break; }
        if (val != 0x0000 && val != 0xFF00) { exc = EXC_ILLEGAL_VALUE; break; }
        if (start < RELAY_NUM && App_GetCfg(CFG_AUTO_CTRL)) { exc = EXC_ILLEGAL_VALUE; break; }
        if (start < RELAY_NUM)      Relay_Set((uint8_t)start, (uint8_t)(val != 0));
        else if (start == 4)        Led_Set((uint8_t)(val != 0));           /* 报警输出 */
        else                        PowerMgr_SetBacklight((uint8_t)(val != 0));
        memcpy(tx, f, 6);
        txlen = 6;
        break;
    }

    /* ---- 写单个保持寄存器 ---- */
    case FC_WRITE_REG: {
        uint16_t merged[CFG_IDX_NUM];
        uint16_t val;

        if (len != 8) { exc = EXC_ILLEGAL_VALUE; break; }
        start = (uint16_t)((f[2] << 8) | f[3]);
        val   = (uint16_t)((f[4] << 8) | f[5]);
        if (start >= MB_HOLD_REG_NUM) { exc = EXC_ILLEGAL_ADDR; break; }

        App_CfgExport(merged);
        if (!mb_hold_valid(merged, (uint8_t)start, val)) { exc = EXC_ILLEGAL_VALUE; break; }
        exc = mb_commit(merged);
        if (exc) break;
        if (s_hold_map[start] == 0xFF) {    /* 命令寄存器 */
            if (val == MB_CMD_SAVE)          s_do_save = 1;
            else if (val == MB_CMD_DEFAULTS) s_do_defaults = 1;
            else                             s_do_reset = 1;   /* 应答发完再重启 */
        }
        memcpy(tx, f, 6);
        txlen = 6;
        break;
    }

    /* ---- 写多个线圈 ---- */
    case FC_WRITE_COILS: {
        uint8_t bc, rel_changed = 0;
        uint8_t rel_mask;

        if (len < 8) { exc = EXC_ILLEGAL_VALUE; break; }
        start = (uint16_t)((f[2] << 8) | f[3]);
        qty   = (uint16_t)((f[4] << 8) | f[5]);
        bc    = f[6];
        if (qty < 1 || qty > 1968 || bc != (uint8_t)((qty + 7) / 8)) { exc = EXC_ILLEGAL_VALUE; break; }
        if (len != (uint16_t)(7 + bc + 2)) { exc = EXC_ILLEGAL_VALUE; break; }
        if (start + qty > MB_COIL_NUM) { exc = EXC_ILLEGAL_ADDR; break; }
        if (start < RELAY_NUM && App_GetCfg(CFG_AUTO_CTRL)) { exc = EXC_ILLEGAL_VALUE; break; }

        App_Snapshot(&snap);
        rel_mask = snap.relay_mask;
        for (i = 0; i < qty; i++) {
            uint8_t idx = (uint8_t)(start + i);
            uint8_t on  = (f[7 + (i / 8)] >> (i % 8)) & 1U;
            if (idx < RELAY_NUM) {
                if (on) rel_mask |= (uint8_t)(1U << idx);
                else    rel_mask &= (uint8_t)~(1U << idx);
                rel_changed = 1;
            } else if (idx == 4) {
                Led_Set(on);
            } else if (idx == 5) {
                PowerMgr_SetBacklight(on);
            }
        }
        if (rel_changed) Relay_Apply(rel_mask);

        tx[0] = addr; tx[1] = func;
        tx[2] = (uint8_t)(start >> 8); tx[3] = (uint8_t)(start & 0xFF);
        tx[4] = (uint8_t)(qty >> 8);   tx[5] = (uint8_t)(qty & 0xFF);
        txlen = 6;
        break;
    }

    /* ---- 写多个保持寄存器 ---- */
    case FC_WRITE_REGS: {
        uint16_t merged[CFG_IDX_NUM];
        uint8_t  bc;

        if (len < 11) { exc = EXC_ILLEGAL_VALUE; break; }
        start = (uint16_t)((f[2] << 8) | f[3]);
        qty   = (uint16_t)((f[4] << 8) | f[5]);
        bc    = f[6];
        if (qty < 1 || qty > 123 || bc != (uint8_t)(qty * 2)) { exc = EXC_ILLEGAL_VALUE; break; }
        if (len != (uint16_t)(7 + bc + 2)) { exc = EXC_ILLEGAL_VALUE; break; }
        if (start + qty > MB_HOLD_REG_NUM) { exc = EXC_ILLEGAL_ADDR; break; }

        /* 先整帧校验，全部合法才提交，避免写一半 */
        App_CfgExport(merged);
        for (i = 0; i < qty; i++) {
            uint16_t v = (uint16_t)((f[7 + i * 2] << 8) | f[8 + i * 2]);
            if (!mb_hold_valid(merged, (uint8_t)(start + i), v)) { exc = EXC_ILLEGAL_VALUE; break; }
        }
        if (exc) break;

        for (i = 0; i < qty; i++) {
            uint16_t v = (uint16_t)((f[7 + i * 2] << 8) | f[8 + i * 2]);
            if (s_hold_map[start + i] == 0xFF) {    /* 命令寄存器 */
                if (v == MB_CMD_SAVE)          s_do_save = 1;
                else if (v == MB_CMD_DEFAULTS) s_do_defaults = 1;
                else                           s_do_reset = 1;
            }
        }
        exc = mb_commit(merged);
        if (exc) break;

        tx[0] = addr; tx[1] = func;
        tx[2] = (uint8_t)(start >> 8); tx[3] = (uint8_t)(start & 0xFF);
        tx[4] = (uint8_t)(qty >> 8);   tx[5] = (uint8_t)(qty & 0xFF);
        txlen = 6;
        break;
    }

    default:
        exc = EXC_ILLEGAL_FUNC;
        break;
    }

    if (exc) {
        App_CountMbFrame(0);
        if (!broadcast) mb_transmit(tx, mb_exception(tx, addr, func, exc));
        return;
    }

    App_CountMbFrame(1);
    if (!broadcast && txlen) mb_transmit(tx, txlen);
}

/* ---------------- 取帧 / 轮询 ---------------- */
void Modbus_Poll(void)
{
    uint8_t  frame[MB_BUF_SIZE];
    uint16_t len;
    uint8_t  ready;
    uint32_t primask;

    ready = s_frame_ready;
    if (ready && (HAL_GetTick() - s_frame_tick) < s_t35_ms) return;   /* 还在 t3.5 静默期 */

    if (ready) {
        /* 极短临界区：只做一次拷贝+复位（256 字节 memcpy 约 6us，
           9600 下一个字节 1.04ms，不会丢字节） */
        primask = os_enter_critical();
        len = s_frame_len;
        if (len > MB_BUF_SIZE) len = MB_BUF_SIZE;
        memcpy(frame, s_frame, len);
        s_frame_len   = 0;
        s_frame_ready = 0;
        os_exit_critical(primask);
    } else {
        return;
    }

    /* 长度不足 / CRC 错：按规范静默丢弃（不上报异常） */
    if (len < 4 || len > MB_BUF_SIZE - 2) { App_CountMbFrame(0); return; }
    if (Crc16_Modbus(frame, len) != 0)    { App_CountMbFrame(0); return; }

    /* 地址过滤：非本机地址不响应（广播地址 0 执行但不应答） */
    if (frame[0] != s_addr && frame[0] != 0) return;

    mb_process(frame, len);

    /* 应答已发完，再做重启/恢复默认这类"慢动作" */
    if (s_do_defaults) {
        s_do_defaults = 0;
        App_CfgDefaults();
        Relay_Apply((uint8_t)App_GetCfg(CFG_RELAY_MASK));
        PowerMgr_SetBacklight((uint8_t)App_GetCfg(CFG_BACKLIGHT));
        PowerMgr_RequestMode(APP_MODE_NORMAL);
    }
    if (s_do_save) {
        s_do_save = 0;
        PowerMgr_RequestSaveNow();
    }
    if (s_do_reset) {
        s_do_reset = 0;
        os_task_delay(50);          /* 等最后一个字节发完 */
        NVIC_SystemReset();
    }
}

/* ---------------- 初始化 ---------------- */
void Modbus_Init(void)
{
    uint16_t baud_code;
    uint32_t baud;

    s_addr       = (uint8_t)App_GetCfg(CFG_SLAVE_ADDR);
    baud_code    = App_GetCfg(CFG_BAUD_CODE);
    if (baud_code > APP_BAUD_115200) baud_code = APP_BAUD_9600;
    baud         = s_baud_tab[baud_code];

    /* t3.5 = 3.5 个字符时间（11 bit/字符），向上取整到 ms
       9600 → 4.0ms 取 5ms；115200 → 0.33ms 取 1ms */
    s_t35_ms = (uint32_t)((38500UL / baud) + 1UL);

#if MODBUS_USE_DE_PIN
    {
        GPIO_InitTypeDef gpio = {0};
        __HAL_RCC_GPIOC_CLK_ENABLE();
        gpio.Pin   = MODBUS_DE_PIN;
        gpio.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio.Pull  = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(MODBUS_DE_PORT, &gpio);
        HAL_GPIO_WritePin(MODBUS_DE_PORT, MODBUS_DE_PIN, GPIO_PIN_RESET);
    }
#endif

    /* MX_USART3_UART_Init 已配好 GPIO/时钟（默认 115200），这里按配置重设波特率 */
    huart3.Init.BaudRate = baud;
    if (HAL_UART_Init(&huart3) != HAL_OK) {
        /* 初始化失败也不阻塞系统，Modbus 不可用但其它功能照常 */
        App_CountMbFrame(0);
        return;
    }

    /* NVIC 使能放在这里而不是 usart.c，避免 CubeMX 重新生成时被删掉。
       优先级 6：高于 SysTick/PendSV(15)，DHT11 屏蔽调度时 USART3 仍能收字节；
       本中断不调用 RTOS API */
    HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    mb_arm_rx();
}

void Modbus_Task(void const *argument)
{
    (void)argument;

    Modbus_Init();

    for (;;) {
        Modbus_Poll();
        os_task_delay(1);
    }
}
