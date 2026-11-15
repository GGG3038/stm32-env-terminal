# STM32 智能温湿度监控终端

基于 **STM32F103ZE** 的多功能环境监控终端：本地 ILI9341 触摸屏交互、DHT11 温湿度采集、4 路继电器控制、Modbus RTU 从机、ESP8266 + OneNet 云端接入，并运行一套**自研的静态分配抢占式轻量 RTOS**（已替换原 FreeRTOS + CMSIS-RTOS v1）。

## 功能特性

- **本地 UI**：LVGL 8.3.11，320×240 横屏，主页状态 + 4 路继电器控制 + 9 项设置页，XPT2046 触摸已实机标定
- **环境采集**：DHT11 单总线，1 Hz 采样，温度/湿度统一按 ×10 整数存储
- **继电器控制**：4 路低电平触发继电器 + PC13 LED，支持手动、触摸、云端、自动联动四种控制源
- **自动联动**：温度过高/过低、湿度过高自动驱动对应继电器，带 0.5 单位迟滞
- **Modbus RTU 从机**：8 种功能码，IDLE + t3.5 断帧，CRC16，广播地址，异常码，地址/波特率可配
- **云端接入**：ESP8266 AT + OneNet 物模型 MQTT，属性上报、云端下发、set_reply 回执、断线重连
- **掉电保存**：阈值/模式/继电器等配置存内部 Flash 最后一页，magic + CRC 校验，静默 500 ms 落盘
- **低功耗**：正常/低功耗双模式，低功耗下关屏、ESP modem sleep、MCU 空闲 `WFI`，USART3 保持在线，Modbus 随时应答
- **可靠性**：IWDG 约 2 s，空闲钩子喂狗；任务卡死会触发复位

## 硬件平台

| 功能 | 器件 / 接口 | 说明 |
|---|---|---|
| MCU | STM32F103ZE | Cortex-M3，72 MHz，512 KB Flash / 64 KB RAM |
| 显示屏 | ILI9341 320×240 | FSMC 16 位 8080 并口，Bank1 NOR/SRAM1 |
| 触摸 | XPT2046 | GPIO 软件 SPI，已按四角实测标定 |
| 温湿度 | DHT11 | PB4，TIM1 提供 µs 级延时 |
| 继电器 | 4 路，低电平触发 | PC0~PC3 |
| 指示 | LED / 背光 | PC13 / PB0 |
| 调试串口 | USART1 | PA9/PA10，115200 8N1，兼做 ISP 串口 |
| WiFi | ESP8266（AT 固件） | USART2，PA2/PA3，115200 |
| 工业总线 | Modbus RTU | USART3，PB10/PB11，默认 9600，可配 4800~115200 |
| 看门狗 | IWDG | 约 2 s |

## 软件架构

```
应用任务层   sensor / display / cloud / modbus / power
    ↓
共享服务层   app_state（全局状态 + 互斥锁）、relay、power_mgr、cfg_flash、watchdog
    ↓
协议驱动层   DHT11、Modbus RTU、ESP8266 AT、OneNet MQTT、LVGL UI、XPT2046、ILI9341
    ↓
系统层       自研 RTOS（os_kernel / os_ipc / os_port）+ STM32 HAL
```

启动流程：

```
SCB->VTOR = FLASH_BASE      // ISP/GO 启动时也必须指向应用向量表
HAL_Init()                  // HAL 时基 = TIM6
SystemClock_Config()        // HSE 8 MHz × PLL9 = 72 MHz
外设初始化 → LVGL/触摸 → App/Cfg/Relay/Power → RTOS 任务创建 → IWDG → os_start()
```

### 双时基设计

| 时基 | 硬件 | 用途 |
|---|---|---|
| RTOS 节拍 | SysTick（内核，HCLK 72 MHz） | `os_task_delay`、超时唤醒、时间片、LVGL tick |
| HAL 时基 | TIM6（1 ms 中断） | `HAL_GetTick()`、`HAL_Delay()`、HAL 超时判断 |

SysTick 由 RTOS 独占，HAL 改用 TIM6，避免两个系统争用同一个计数器和中断。

## 自研轻量 RTOS

代码在 [`RTOS/`](RTOS/)：

- 静态分配：TCB 池固定 8 个，任务栈由用户提供，无 malloc、无软件定时器
- 抢占式调度：32 个优先级（数字越大越高，0 保留给空闲任务），PendSV 切换上下文
- 同优先级 1 ms 时间片轮转
- IPC：计数信号量、带优先级继承的互斥锁、定长 `void*` 消息队列
- 栈填充魔数 `0xA5A5A5A5`，`os_task_stack_used()` 可查高水位
- 空闲钩子执行 `__WFI()` 并喂 IWDG

| 任务 | 优先级 | 栈（word） | 周期 | 职责 |
|---|---|---|---|---|
| sensor | 6 | 384 | 1 s | DHT11 采集与有效性标记 |
| display | 4 | 1024 | 10 ms | LVGL 刷新、触摸、低功耗关屏 |
| cloud | 6 | 1536 | 100 ms | ESP8266/OneNet 登录、上报、下发、重连 |
| modbus | 4 | 512 | 1 ms | USART3 收帧与 Modbus 应答 |
| power | 2 | 384 | 50 ms | 模式、报警、自动联动、运行时间、配置落盘 |
| idle | 0 | 128 | 空闲 | `WFI` + 喂狗 |

## 目录结构

```
Core/               CubeMX 生成的 HAL 初始化、时钟、中断
RTOS/               自研 RTOS 内核、IPC、Cortex-M3 移植层
task/               应用模块（传感器/显示/云端/Modbus/电源/配置/状态）
lvgl/               LVGL 8.3.11 与显示/输入移植
LCD/ touch/         ILI9341、XPT2046 驱动
ui/                 旧的车机风格 UI 资源（当前未引用）
Drivers/            STM32F1 HAL + CMSIS
Middlewares/        迁移前的 FreeRTOS 中间件（未参与构建）
MDK-ARM/            Keil 工程
```

## 构建

- Keil MDK5 + ARM Compiler 5.06，C99
- 打开 [`MDK-ARM/task6.uvprojx`](MDK-ARM/task6.uvprojx) 编译，或命令行：

```bat
UV4.exe -b task6.uvprojx -j0
fromelf.exe --bin -o task6\task6.bin task6\task6.axf
```

首次编译前需要准备私有凭据：

```bat
copy task\app_secrets.example.h task\app_secrets.h
```

然后在 `task/app_secrets.h` 填入 WiFi 账号密码和 OneNet 产品/设备/Token（该文件已被 `.gitignore` 忽略）。

## 烧录

通过 USART1 + CH340 使用系统 bootloader（8E1，115200）：

```bat
python -m stm32loader -p COM9 -b 115200 -e -w -v -g 0x08000000
```

- ISP 模式：BOOT0 接高电平后复位
- 正常运行：BOOT0 可靠接 GND 后复位（悬空会导致白屏、停在 bootloader）
- 全片擦除会清掉 0x0807F800 的保存配置，参数恢复默认

> 注意：用 `GO` 从 bootloader 启动应用时，BOOT0 仍为高，0x00000000 会别名到系统存储器。因此 `main()` 开头必须执行 `SCB->VTOR = FLASH_BASE;`，否则第一个中断会跳进 bootloader 向量表导致卡死。

## Modbus 寄存器

功能码：`0x01 0x02 0x03 0x04 0x05 0x06 0x0F 0x10`，支持广播地址 0，异常码 01~04。

### 输入寄存器（0x0000~0x0007）

| 地址 | 内容 |
|---|---|
| 0x0000 | 温度 ×10，有符号 |
| 0x0001 | 湿度 ×10 |
| 0x0002 | 状态字：b0 传感器正常 / b1 报警 / b2 低功耗 / b3 WiFi / b4 云端 / b5 背光 / b6 Modbus 在线 |
| 0x0003 | 继电器实际电平 b0~b3 + LED b4 |
| 0x0004 | 报警明细（温度高/低、湿度高/低） |
| 0x0005 | 固件版本 0x0101 |
| 0x0006/0x0007 | 运行时间（秒）高/低 16 位 |

### 保持寄存器（0x0000~0x000D）

| 地址 | 内容 | 默认值 |
|---|---|---|
| 0x0000 / 0x0001 | 温度上限 / 下限 ×10 | 350 / 100 |
| 0x0002 / 0x0003 | 湿度上限 / 下限 ×10 | 800 / 300 |
| 0x0004 | 云端上传周期（s） | 30 |
| 0x0005 | 从机地址（1~247，立即生效） | 1 |
| 0x0006 | 波特率码（0=4800 … 4=115200，重启生效） | 1（9600） |
| 0x0007 | 工作模式 0 正常 / 1 低功耗 | 0 |
| 0x0008 | 自动联动使能 | 1 |
| 0x0009 | 继电器手动控制字（自动模式拒写） | 0 |
| 0x000A | 背光 | 1 |
| 0x000B | 报警使能 b0 报警 / b1 蜂鸣器 | 0x01 |
| 0x000C | 命令：1 保存 / 2 恢复默认 / 3 重启 | — |
| 0x000D | 保留 | — |

线圈 0x0000~0x0005：R1~R4、LED、背光；离散输入 0x0000~0x0005：继电器实际回读、报警、传感器正常。

## 云端物模型

- 上报属性：`temp / hum / r1~r4 / led / bl / alm / mode / mbok / up`
- 下发主题：`$sys/{pid}/{dev}/thing/property/set`，支持继电器、LED、背光、模式、阈值
- 回执：`$sys/{pid}/{dev}/thing/property/set_reply`

## 已知说明

- LVGL 未启用中文字体，界面标签使用英文缩写
- WiFi 模块未接入时，云端任务会按 3 次重试后打印 `ATE0 FAIL`，系统其余功能正常
- 配置写 Flash 时 CPU 会停顿约 20~40 ms，因此只在空闲时、配置静默 500 ms 后落盘
- `ui/` 与 `Middlewares/Third_Party/FreeRTOS/` 为历史遗留，未参与实际功能
