/**
 * @file    picture_lvgl.c
 * @brief   智能控制终端 · 触摸屏界面（320x240 横屏）
 *
 * 说明：LVGL 内置字体不含中文（LV_FONT_SIMSUN_16_CJK 是关闭的），
 *       所以标签用英文缩写 —— T-HI/T-LO 温度上下限、RH-HI/RH-LO 湿度上下限、
 *       PERIOD 上传周期、ADDR 从机地址、BAUD 波特率、
 *       MODE 工作模式、AUTO 自动联动。
 *       要中文界面就用 LVGL 字体转换器生成一个 16px 中文小字体（约 100 个汉字
 *       的取子集字体只有几 KB），把 LV_FONT_SIMSUN_16_CJK 打开或加自定义字体。
 *
 * 对象全部建在 LVGL 24KB 内存池里，设置页按需创建（首次进入才建）。
 */
#include "picture_lvgl.h"
#include "app_state.h"
#include "relay.h"
#include "power_mgr.h"
#include "os.h"
#include "lcd.h"
#include <stdio.h>
#include <string.h>

/* lcd.c 里定义了但 lcd.h 没声明 */
void ILI9341_Write_Cmd(uint16_t usCmd);

/* ILI9341 显开/显关。这里不用 0x10/0x11（sleep in/out），
   因为 sleep-in 会丢 GRAM，唤醒后必须整屏重绘；只关显示最省事 */
#define LCD_CMD_DISPOFF     0x28
#define LCD_CMD_DISPON      0x29

/* 颜色 */
#define COL_BG      0x08081A
#define COL_BAR     0x141428
#define COL_TEMP    0xFF6600
#define COL_HUM     0x3399FF
#define COL_ON      0x1E8E3E
#define COL_OFF     0x2A2A40
#define COL_TEXT    0xD0D0E0
#define COL_WARN    0xFF4040
#define COL_SEL     0x3A3A6A

/* ---------------- 主页对象 ---------------- */
static lv_obj_t *s_scr_main;
static lv_obj_t *s_lbl_mode;
static lv_obj_t *s_lbl_net;
static lv_obj_t *s_lbl_alarm;
static lv_obj_t *s_lbl_temp;
static lv_obj_t *s_lbl_humi;
static lv_obj_t *s_lbl_mb;
static lv_obj_t *s_btn_relay[RELAY_NUM];
static lv_obj_t *s_lbl_relay[RELAY_NUM];

/* 设置页对象（按需创建） */
#define SET_ROWS    9
static lv_obj_t *s_scr_set;
static lv_obj_t *s_row[SET_ROWS];
static lv_obj_t *s_row_val[SET_ROWS];
static lv_obj_t *s_lbl_hint;            /* 主页底部提示 */
static lv_obj_t *s_lbl_smsg;            /* 设置页底部提示 */
static uint8_t   s_sel = 0;
static uint8_t   s_set_ready = 0;

/* LVGL tick 直接取轻量 RTOS 的 1ms 节拍（lv_conf.h 里 LV_TICK_CUSTOM），
   不再需要软件定时器喂 lv_tick_inc() */

/* ---------------- 设置项表 ---------------- */
enum { K_TEMP = 0, K_HUM, K_U16, K_BOOL, K_BAUD, K_MODE };

typedef struct {
    const char *name;
    cfg_idx_t   idx;
    int16_t     step;
    uint8_t     kind;
} set_item_t;

static const set_item_t s_items[SET_ROWS] = {
    { "T-HI",   CFG_TEMP_HI,       5, K_TEMP  },
    { "T-LO",   CFG_TEMP_LO,       5, K_TEMP  },
    { "RH-HI",  CFG_HUM_HI,        5, K_HUM   },
    { "RH-LO",  CFG_HUM_LO,        5, K_HUM   },
    { "PERIOD", CFG_UPLOAD_PERIOD, 5, K_U16   },
    { "ADDR",   CFG_SLAVE_ADDR,    1, K_U16   },
    { "BAUD",   CFG_BAUD_CODE,     1, K_BAUD  },
    { "MODE",   CFG_MODE,          1, K_MODE  },
    { "AUTO",   CFG_AUTO_CTRL,     1, K_BOOL  },
};

static const char *BAUD_TXT[5] = { "4800", "9600", "19200", "38400", "115200" };

/* ---------------- 小工具 ---------------- */

/* 把配置值格式化成显示文本 */
static void fmt_cfg(char *buf, uint32_t n, const set_item_t *it, uint16_t v)
{
    int16_t sv = (int16_t)v;

    switch (it->kind) {
    case K_TEMP:
        if (sv < 0) lv_snprintf(buf, n, "-%d.%d C", (-sv) / 10, (-sv) % 10);
        else        lv_snprintf(buf, n, "%d.%d C", sv / 10, sv % 10);
        break;
    case K_HUM:
        lv_snprintf(buf, n, "%d.%d %%", sv / 10, sv % 10);
        break;
    case K_BAUD:
        lv_snprintf(buf, n, "%s", (v <= APP_BAUD_115200) ? BAUD_TXT[v] : "?");
        break;
    case K_BOOL:
        lv_snprintf(buf, n, "%s", v ? "ON" : "OFF");
        break;
    case K_MODE:
        lv_snprintf(buf, n, "%s", (v == APP_MODE_LOWPOWER) ? "SAVE" : "NORM");
        break;
    default:
        lv_snprintf(buf, n, "%d", sv);
        break;
    }
}

static void relay_btn_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uint32_t)lv_event_get_user_data(e);
    app_state_t s;

    App_Snapshot(&s);
    if (s.cfg[CFG_AUTO_CTRL]) {
        /* 自动联动态下手动控制无效（和 Modbus 拒绝写是同一规则） */
        if (s_lbl_hint) lv_label_set_text(s_lbl_hint, "AUTO mode: manual control disabled");
        return;
    }
    Relay_Toggle(idx);
}

static void set_btn_cb(lv_event_t *e);
static void set_refresh(void);

static void open_settings_cb(lv_event_t *e)
{
    (void)e;
    if (!s_set_ready) set_btn_cb(NULL);     /* 第一次进入：创建设置页 */
    else              set_refresh();        /* 再次进入：刷新一遍值 */
    lv_scr_load(s_scr_set);
}

/* ---------------- 主页 ---------------- */

void Ui_Init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *bar, *o;
    uint8_t i;

    s_scr_main = scr;                       /* 设置页返回时用 */
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 顶部状态栏 ---- */
    bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 320, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_mode = lv_label_create(bar);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_mode, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_lbl_mode, "NORM");
    lv_obj_set_pos(s_lbl_mode, 6, 4);

    s_lbl_net = lv_label_create(bar);
    lv_obj_set_style_text_font(s_lbl_net, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_lbl_net, "WIFI- CLOUD-");
    lv_obj_set_pos(s_lbl_net, 80, 4);

    s_lbl_alarm = lv_label_create(bar);
    lv_obj_set_style_text_font(s_lbl_alarm, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_alarm, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_lbl_alarm, "");
    lv_obj_align(s_lbl_alarm, LV_ALIGN_TOP_RIGHT, -6, 4);

    /* ---- 温湿度 ---- */
    o = lv_label_create(scr);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(o, "TEMP");
    lv_obj_set_pos(o, 12, 32);

    s_lbl_temp = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(COL_TEMP), 0);
    lv_label_set_text(s_lbl_temp, "--.- C");
    lv_obj_set_pos(s_lbl_temp, 10, 48);

    o = lv_label_create(scr);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(o, "HUMIDITY");
    lv_obj_set_pos(o, 12, 106);

    s_lbl_humi = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_humi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_humi, lv_color_hex(COL_HUM), 0);
    lv_label_set_text(s_lbl_humi, "--.- %");
    lv_obj_set_pos(s_lbl_humi, 10, 122);

    /* Modbus 统计（帧数 / 异常数）与 UI 提示 */
    s_lbl_mb = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_mb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_mb, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_lbl_mb, "MB 0/0");
    lv_obj_set_pos(s_lbl_mb, 12, 176);

    s_lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(COL_WARN), 0);
    lv_label_set_text(s_lbl_hint, "");
    lv_obj_set_pos(s_lbl_hint, 12, 200);

    /* ---- 4 路继电器按钮（2x2） ---- */
    for (i = 0; i < RELAY_NUM; i++) {
        lv_obj_t *b;
        char txt[12];

        b = lv_btn_create(scr);
        lv_obj_set_size(b, 72, 60);
        lv_obj_set_pos(b, (i % 2) ? 244 : 168, (i / 2) ? 110 : 34);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_OFF), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, relay_btn_cb, LV_EVENT_CLICKED, (void *)(uint32_t)i);
        s_btn_relay[i] = b;

        lv_snprintf(txt, sizeof(txt), "R%d\nOFF", i + 1);
        s_lbl_relay[i] = lv_label_create(b);
        lv_obj_set_style_text_font(s_lbl_relay[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_relay[i], lv_color_white(), 0);
        lv_label_set_text(s_lbl_relay[i], txt);
        lv_obj_center(s_lbl_relay[i]);
    }

    /* ---- 设置入口 ---- */
    o = lv_btn_create(scr);
    lv_obj_set_size(o, 72, 30);
    lv_obj_set_pos(o, 244, 178);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x3355AA), 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_add_event_cb(o, open_settings_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *l = lv_label_create(o);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, "SET");
        lv_obj_center(l);
    }
}

/* 把 app_state 的变化刷到界面（值没变就不调 set_text，省重绘） */
static void ui_sync(void)
{
    static app_state_t prev;
    app_state_t s;
    char buf[24];
    uint8_t i;

    App_Snapshot(&s);

    if (s.temp_x10 != prev.temp_x10 || s.sensor_ok != prev.sensor_ok) {
        if (!s.sensor_ok) {
            lv_label_set_text(s_lbl_temp, "--.- C");
        } else if (s.temp_x10 < 0) {
            lv_snprintf(buf, sizeof(buf), "-%d.%d C", (-s.temp_x10) / 10, (-s.temp_x10) % 10);
            lv_label_set_text(s_lbl_temp, buf);
        } else {
            lv_snprintf(buf, sizeof(buf), "%d.%d C", s.temp_x10 / 10, s.temp_x10 % 10);
            lv_label_set_text(s_lbl_temp, buf);
        }
        prev.temp_x10  = s.temp_x10;
        prev.sensor_ok = s.sensor_ok;
    }

    if (s.hum_x10 != prev.hum_x10) {
        if (s.sensor_ok) {
            lv_snprintf(buf, sizeof(buf), "%d.%d %%", s.hum_x10 / 10, s.hum_x10 % 10);
            lv_label_set_text(s_lbl_humi, buf);
        } else {
            lv_label_set_text(s_lbl_humi, "--.- %");
        }
        prev.hum_x10 = s.hum_x10;
    }

    if (s.mode != prev.mode || s.awake != prev.awake) {
        const char *m = (s.mode == APP_MODE_LOWPOWER)
                      ? (s.awake ? "SAVE(awake)" : "SAVE")
                      : "NORM";
        if (strcmp(lv_label_get_text(s_lbl_mode), m) != 0) lv_label_set_text(s_lbl_mode, m);
        prev.mode  = s.mode;
        prev.awake = s.awake;
    }

    if (s.wifi_conn != prev.wifi_conn || s.cloud_conn != prev.cloud_conn) {
        lv_snprintf(buf, sizeof(buf), "WIFI%c CLOUD%c",
                    s.wifi_conn ? '+' : '-', s.cloud_conn ? '+' : '-');
        lv_label_set_text(s_lbl_net, buf);
        lv_obj_set_style_text_color(s_lbl_net,
            lv_color_hex(s.cloud_conn ? 0x40C060 : COL_TEXT), 0);
        prev.wifi_conn  = s.wifi_conn;
        prev.cloud_conn = s.cloud_conn;
    }

    if (s.alarm_flags != prev.alarm_flags) {
        if (s.alarm_flags == 0) {
            lv_label_set_text(s_lbl_alarm, "");
        } else {
            lv_snprintf(buf, sizeof(buf), "ALARM%s%s%s%s",
                        (s.alarm_flags & APP_ALARM_TEMP_HI) ? " TH" : "",
                        (s.alarm_flags & APP_ALARM_TEMP_LO) ? " TL" : "",
                        (s.alarm_flags & APP_ALARM_HUM_HI)  ? " HH" : "",
                        (s.alarm_flags & APP_ALARM_HUM_LO)  ? " HL" : "");
            lv_label_set_text(s_lbl_alarm, buf);
            lv_obj_set_style_text_color(s_lbl_alarm, lv_color_hex(COL_WARN), 0);
        }
        prev.alarm_flags = s.alarm_flags;
    }

    if (s.relay_mask != prev.relay_mask) {
        for (i = 0; i < RELAY_NUM; i++) {
            uint8_t on = (s.relay_mask >> i) & 1U;
            lv_snprintf(buf, sizeof(buf), "R%d\n%s", i + 1, on ? "ON" : "OFF");
            lv_label_set_text(s_lbl_relay[i], buf);
            lv_obj_set_style_bg_color(s_btn_relay[i],
                lv_color_hex(on ? COL_ON : COL_OFF), 0);
        }
        prev.relay_mask = s.relay_mask;
    }

    if (s.mb_frames != prev.mb_frames || s.mb_errors != prev.mb_errors) {
        lv_snprintf(buf, sizeof(buf), "MB %u/%u",
                    (unsigned)s.mb_frames, (unsigned)s.mb_errors);
        lv_label_set_text(s_lbl_mb, buf);
        prev.mb_frames = s.mb_frames;
        prev.mb_errors = s.mb_errors;
    }
}

/* ---------------- 设置页 ---------------- */

static void row_click_cb(lv_event_t *e)
{
    uint8_t i;
    s_sel = (uint8_t)(uint32_t)lv_event_get_user_data(e);
    for (i = 0; i < SET_ROWS; i++) {
        lv_obj_set_style_bg_color(s_row[i],
            lv_color_hex(i == s_sel ? COL_SEL : COL_BAR), 0);
    }
}

/* 刷新设置页所有行的值 */
static void set_refresh(void)
{
    char buf[24];
    uint8_t i;

    for (i = 0; i < SET_ROWS; i++) {
        fmt_cfg(buf, sizeof(buf), &s_items[i], App_GetCfg(s_items[i].idx));
        lv_label_set_text(s_row_val[i], buf);
    }
}

/* delta != 0：调整当前选中项；delta == 0 且 e == NULL：创建页面 */
static void set_btn_cb(lv_event_t *e)
{
    if (e == NULL) {
        /* ---- 首次进入：创建设置页 ---- */
        lv_obj_t *scr, *o, *title;
        uint8_t i;

        s_scr_set = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_scr_set, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(s_scr_set, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_scr_set, 0, 0);
        lv_obj_clear_flag(s_scr_set, LV_OBJ_FLAG_SCROLLABLE);
        scr = s_scr_set;

        title = lv_label_create(scr);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(title, "SETTINGS");
        lv_obj_set_pos(title, 6, 4);

        for (i = 0; i < SET_ROWS; i++) {
            lv_obj_t *row = lv_btn_create(scr);
            lv_obj_set_size(row, 308, 17);
            lv_obj_set_pos(row, 6, 22 + i * 18);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_BAR), 0);
            lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(uint32_t)i);
            s_row[i] = row;

            o = lv_label_create(row);
            lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(o, lv_color_hex(COL_TEXT), 0);
            lv_label_set_text(o, s_items[i].name);
            lv_obj_set_pos(o, 4, 2);

            s_row_val[i] = lv_label_create(row);
            lv_obj_set_style_text_font(s_row_val[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_row_val[i], lv_color_hex(0x80D0FF), 0);
            lv_label_set_text(s_row_val[i], "-");
            lv_obj_align(s_row_val[i], LV_ALIGN_RIGHT_MID, -4, 0);
        }

        /* ---- 底部按钮：- / + / SAVE / BACK ---- */
        static const char *btxt[4] = { "-", "+", "SAVE", "BACK" };
        static const int   bx[4]   = { 6, 60, 130, 240 };
        static const int   bw[4]   = { 50, 50, 100, 74 };

        for (i = 0; i < 4; i++) {
            o = lv_btn_create(scr);
            lv_obj_set_size(o, bw[i], 32);
            lv_obj_set_pos(o, bx[i], 204);
            lv_obj_set_style_radius(o, 6, 0);
            lv_obj_set_style_shadow_width(o, 0, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(i == 3 ? 0x3355AA : 0x2A2A54), 0);
            lv_obj_add_event_cb(o, set_btn_cb, LV_EVENT_CLICKED, (void *)(uint32_t)(i + 1));
            {
                lv_obj_t *l = lv_label_create(o);
                lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
                lv_label_set_text(l, btxt[i]);
                lv_obj_center(l);
            }
        }

        /* ---- 底部提示行 ---- */
        s_lbl_smsg = lv_label_create(scr);
        lv_obj_set_style_text_font(s_lbl_smsg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_smsg, lv_color_hex(COL_WARN), 0);
        lv_label_set_text(s_lbl_smsg, "");
        lv_obj_set_pos(s_lbl_smsg, 6, 186);

        s_sel = 0;
        s_set_ready = 1;
        set_refresh();
        return;
    }

    /* ---- 按钮：1=-  2=+  3=SAVE  4=BACK ---- */
    {
        uint8_t btn = (uint8_t)(uint32_t)lv_event_get_user_data(e);

        if (btn == 4) {
            lv_label_set_text(s_lbl_smsg, "");
            lv_scr_load(s_scr_main);        /* 切回主页 */
            return;
        }
        if (btn == 3) {
            PowerMgr_RequestSaveNow();
            lv_label_set_text(s_lbl_smsg, "SAVING...");
            return;
        }
        {
            const set_item_t *it = &s_items[s_sel];
            int16_t v  = (int16_t)App_GetCfg(it->idx);
            int16_t nv = (int16_t)(v + ((btn == 2) ? it->step : -it->step));

            if (App_SetCfg(it->idx, (uint16_t)nv)) {
                lv_label_set_text(s_lbl_smsg, "");
                /* MODE 项要让电源管理立刻执行（设成 SAVE 会关屏进低功耗）。
                   注意：低功耗下云端已停车、屏幕已关闭，退出只能由主站
                   写 Modbus 40008 = 0；模式会掉电保存，重新上电仍进低功耗 */
                if (it->idx == CFG_MODE) PowerMgr_RequestMode((uint8_t)nv);
                set_refresh();
            } else {
                /* 校验不过（比如上限低于下限）——值保持不变 */
                lv_label_set_text(s_lbl_smsg, "invalid (check range)");
            }
        }
    }
}

/* ---------------- 任务 ---------------- */

static void ui_park(void)
{
    ILI9341_Write_Cmd(LCD_CMD_DISPOFF);     /* 关显示，GRAM 保留 */
    PowerMgr_SetBacklight(0);
}

static void ui_unpark(void)
{
    ILI9341_Write_Cmd(LCD_CMD_DISPON);
    PowerMgr_SetBacklight((uint8_t)App_GetCfg(CFG_BACKLIGHT));
    lv_obj_invalidate(lv_scr_act());        /* 停了这么久，整屏重绘 */
    if (s_set_ready) set_refresh();
}

void Ui_Task(void const *argument)
{
    uint32_t set_tick = 0;

    (void)argument;

    for (;;) {
        /* ---- 低功耗停车：关屏 → 回报 → 等待放行 → 恢复 ---- */
        if (PowerMgr_ParkReq()) {
            ui_park();
            PowerMgr_ParkAck(PARK_DISP, 1);
            while (PowerMgr_ParkReq()) {
                os_task_delay(50);
            }
            PowerMgr_ParkAck(PARK_DISP, 0);
            ui_unpark();
            continue;
        }

        ui_sync();
        lv_timer_handler();

        /* 设置页开着时定期刷新（值可能被 Modbus/云端改掉） */
        if (s_set_ready && lv_scr_act() == s_scr_set &&
            (HAL_GetTick() - set_tick) > 500U) {
            set_tick = HAL_GetTick();
            set_refresh();
        }

        os_task_delay(10);
    }
}
