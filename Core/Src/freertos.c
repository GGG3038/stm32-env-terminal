/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * @brief              任务创建 + 各任务主体（轻量 RTOS 版）
  *
  * 本文件只放"创建任务 + 任务函数"：
  *   - 模式切换 / SLEEP 低功耗        → power_mgr.c
  *   - 全局状态（温湿度/继电器/配置）    → app_state.c
  *   - Modbus 从机                      → modbus_rtu.c
  *   - 触摸屏界面                       → picture_lvgl.c
  *
  * 任务表（数值越大优先级越高，0 保留给空闲任务）：
  *   Task_Sensor   6   DHT11 采集，1s 一次
  *   Task_Cloud    6   ESP8266 + OneNet 上报 / 接收下发
  *   Task_Display  4   LVGL 刷新 + 触摸 + 低功耗时关屏
  *   Task_Modbus   4   USART3 Modbus RTU 从机
  *   Task_PowerMgr 2   模式切换 / 报警联动 / 配置落盘 / SLEEP 低功耗
  *
  * 轻量 RTOS 使用静态内存：每个任务的栈由本文件提供（单位 word）。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "DHT11.h"
#include "esp8266.h"
#include "onenet.h"
#include "app_state.h"
#include "relay.h"
#include "cfg_flash.h"
#include "power_mgr.h"
#include "modbus_rtu.h"
#include "picture_lvgl.h"
#include "watchdog.h"
#include "usart_wifi.h"
#include "usart.h"
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define PRIO_SENSOR     6U
#define PRIO_CLOUD      6U
#define PRIO_DISPLAY    4U
#define PRIO_MODBUS     4U
#define PRIO_POWER      2U
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
os_task_t *Task_SensorHandle;
os_task_t *Task_DisplayHandle;
os_task_t *Task_CloudHandle;
os_task_t *Task_PowerMgrHandle;
os_task_t *Task_ModbusHandle;

/* 任务栈：单位 word（4 字节），原来 CMSIS osThreadDef 的数量直接沿用 */
static uint32_t s_stack_sensor[384U];
static uint32_t s_stack_display[1024U];
static uint32_t s_stack_cloud[1536U];
static uint32_t s_stack_modbus[512U];
static uint32_t s_stack_power[384U];
/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
void StartTask_Sensor(void *argument);
void StartTask_Display(void *argument);
void StartTask_Cloud(void *argument);
void StartTask_PowerMgr(void *argument);
void StartTask_Modbus(void *argument);

void MX_FREERTOS_Init(void);

void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* 必须先初始化内核（创建空闲任务），再创建用户任务 */
  if (os_init() != OS_OK) {
    for (;;) {
    }
  }

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* 互斥锁由各模块自己创建（app_state 一把锁管全部状态） */
  /* USER CODE END RTOS_MUTEX */
  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */
  /* USER CODE BEGIN RTOS_TIMERS */
  /* 轻量 RTOS 没有软件定时器；LVGL 的 tick 直接取 os_tick_get() */
  /* USER CODE END RTOS_TIMERS */
  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  (void)os_task_create(&Task_SensorHandle, "sensor", StartTask_Sensor, NULL,
                       s_stack_sensor, 384U, PRIO_SENSOR);
  (void)os_task_create(&Task_DisplayHandle, "display", StartTask_Display, NULL,
                       s_stack_display, 1024U, PRIO_DISPLAY);
  (void)os_task_create(&Task_CloudHandle, "cloud", StartTask_Cloud, NULL,
                       s_stack_cloud, 1536U, PRIO_CLOUD);
  (void)os_task_create(&Task_ModbusHandle, "modbus", StartTask_Modbus, NULL,
                       s_stack_modbus, 512U, PRIO_MODBUS);
  (void)os_task_create(&Task_PowerMgrHandle, "power", StartTask_PowerMgr, NULL,
                       s_stack_power, 384U, PRIO_POWER);

  /* USER CODE BEGIN RTOS_THREADS */
  /* USER CODE END RTOS_THREADS */
}

/* ===================== 传感器 ===================== */

void StartTask_Sensor(void *argument)
{
  /* USER CODE BEGIN StartTask_Sensor */
  uint8_t temp_raw, hum_raw;

  (void)argument;

  dht11_init();             /* 里面有延时，必须在任务里（调度器已启动）调用 */
  os_task_delay(1000);

  for(;;)
  {
    if (dht11_readdata(&temp_raw, &hum_raw) == 0) {
      /* DHT11 只有整数位，×10 之后和 Modbus 的"温度×10"寄存器一致 */
      App_SetSensor((int16_t)temp_raw * 10, (int16_t)hum_raw * 10, 1);
    } else {
      /* 采集失败：保留上一次的数值但标记无效（报警判断会跳过无效值） */
      app_state_t s;
      App_Snapshot(&s);
      App_SetSensor(s.temp_x10, s.hum_x10, 0);
      UsartPrintf(USART_DEBUG, "DHT11 read fail");
    }

    os_task_delay(1000);
  }
  /* USER CODE END StartTask_Sensor */
}

/* ===================== 显示 ===================== */

void StartTask_Display(void *argument)
{
  /* USER CODE BEGIN StartTask_Display */
  Ui_Task(argument);        /* 显示逻辑都在 picture_lvgl.c */
  /* USER CODE END StartTask_Display */
}

/* ===================== 云端 ===================== */

/* 分段等待：期间响应低功耗停车请求，尽快停下来让 ESP 进休眠 */
static void cloud_wait(uint32_t ms)
{
  while (ms >= 100U) {
    if (PowerMgr_ParkReq()) return;
    os_task_delay(100);
    ms -= 100U;
  }
  if (ms) os_task_delay(ms);
}

void StartTask_Cloud(void *argument)
{
  /* USER CODE BEGIN StartTask_Cloud */
  static const char *sub_topics[1] = { ONENET_TOPIC_PROP_SET };
  uint8_t  want_link = 1;       /* 需要(重)连 */
  uint8_t  fast_relink = 0;     /* 1 = ESP 还活着，只要重新做 MQTT 登录 */
  uint8_t  force_upload = 1;    /* 1 = 下一轮立刻上报（刚唤醒时用） */
  uint32_t upload_tick = 0;

  (void)argument;

  for(;;)
  {
    /* ---- 1. 低功耗停车：让 ESP 进 modem sleep，再回报 ----
       先发完 AT 指令再回报；停车期间本任务不再访问 ESP */
    if (PowerMgr_ParkReq()) {
      ESP8266_Sleep();
      PowerMgr_ParkAck(PARK_CLOUD, 1);
      while (PowerMgr_ParkReq()) {
        os_task_delay(50);
      }
      PowerMgr_ParkAck(PARK_CLOUD, 0);

      want_link    = 1;         /* 睡了几十分钟，连接早掉了，醒来重新登录 */
      fast_relink  = 1;
      force_upload = 1;         /* 重新上线后立刻上报一次 */
      continue;
    }

    /* ---- 2. (重)连接 ---- */
    if (want_link) {
      if (fast_relink && ESP8266_Wake()) {
        /* ESP 没掉线：省掉 2s 的 AT 初始化，直接重新登录 MQTT */
        if (OneNet_DevLink() == 0) {
          OneNet_Subscribe(sub_topics, 1);
          want_link = 0;
        }
      } else if (ESP8266_Init() && OneNet_DevLink() == 0) {
        OneNet_Subscribe(sub_topics, 1);
        want_link = 0;
      }

      fast_relink = 0;

      /* 连不上：30s 后再试（可被停车打断）。
         别设太短：平台对短时间内大量连接会限流/临时封 IP，
         之前每 10s 重连会把封禁一直“续”下去 */
      if (want_link) {
        App_SetCloudLink(0, 0);
        cloud_wait(30000);
        continue;
      }
    }

    /* ---- 3. 收云端下发（property/set → 继电器/LED/背光/模式/阈值） ---- */
    {
      unsigned char *ipd = ESP8266_GetIPD(3);      /* 最多等 30ms */
      if (ipd != NULL) {
        OneNet_RevPro(ipd);
      }
    }

    /* ---- 4. 按上传周期上报全部属性 ---- */
    if (force_upload ||
        (HAL_GetTick() - upload_tick) >= ((uint32_t)App_GetCfg(CFG_UPLOAD_PERIOD) * 1000U)) {
      force_upload = 0;
      upload_tick  = HAL_GetTick();
      OneNet_ReportProps();
    }

    os_task_delay(100);
  }
  /* USER CODE END StartTask_Cloud */
}

/* ===================== Modbus ===================== */

void StartTask_Modbus(void *argument)
{
  /* USER CODE BEGIN StartTask_Modbus */
  Modbus_Task(argument);    /* 协议栈都在 modbus_rtu.c */
  /* USER CODE END StartTask_Modbus */
}

/* ===================== 电源管理 ===================== */

void StartTask_PowerMgr(void *argument)
{
  /* USER CODE BEGIN StartTask_PowerMgr */
  PowerMgr_Task(argument);  /* 模式/报警/落盘/SLEEP 低功耗都在 power_mgr.c */
  /* USER CODE END StartTask_PowerMgr */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* 轻量 RTOS 的空闲钩子：没有任务就绪时进 SLEEP。
 *
 * SLEEP 只停 CPU 时钟，外设（USART3 / TIM6 / EXTI）继续运行：
 *   - USART3 收到 Modbus 查询 → 接收中断唤醒 CPU → 从机正常应答
 *   - 这就是低功耗模式下 Modbus "随问随答" 的关键
 *
 * 任何中断都能唤醒 CPU，不需要显式清标志。
 * 代价是电流比 STOP 高（mA 级 vs µA 级），换来串口全时在线。
 *
 * 看门狗也在这里喂：能进空闲说明所有任务都在正常让出 CPU；
 * 若某个任务忙等/死循环把空闲任务饿死，IWDG 约 2s 后复位。 */
void os_idle_hook(void)
{
    Watchdog_Feed();
    __WFI();
}

/* USER CODE END Application */
