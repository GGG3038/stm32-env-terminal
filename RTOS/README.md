# 轻量级 RTOS（Cortex-M3 / STM32F103）设计与使用说明

本项目为从零实现的抢占式轻量级 RTOS，替换原有的 LVGL 演示。全部代码使用 C99 编写，
仅依赖 CMSIS 与少量 Keil ARMCC 内嵌汇编，不依赖任何第三方 RTOS。

- 目标芯片：STM32F103ZE（Cortex-M3，72 MHz，256 KB Flash / 48 KB RAM）
- 开发环境：Keil MDK-ARM 5（ARMCC 5.06），C99
- 版本：1.0.0
- 构建结果：0 Error(s)，0 Warning(s)；Code 8368 B，RO 372 B，RW 72 B，ZI 9264 B

> 说明：Keil 工程名/输出名仍为 `lvgl1`，属于历史命名，仅用于兼容工程文件，与 LVGL 无关。
>
> 想从原理上理解内核，建议先读 [RTOS原理详解.md](RTOS原理详解.md)（通俗讲解），
> 再查阅本文的 API 表、配置项和验证方法。

---

## 1. 文件结构

| 文件 | 说明 |
|---|---|
| `RTOS/Inc/os_config.h` | 编译期配置：任务数、优先级数、tick 频率、空闲栈、时间片、断言开关 |
| `RTOS/Inc/os.h` | 公共 API：任务、信号量、互斥锁、消息队列、时间、临界区 |
| `RTOS/Inc/os_internal.h` | 内核内部结构：TCB、等待类型、全局量、内部函数、断言 |
| `RTOS/Inc/os_port.h` | Cortex-M3 移植层接口：栈帧常量、ISR 判断、首次启动 |
| `RTOS/Src/os_kernel.c` | 调度器、就绪链表、tick/超时、任务管理、空闲任务、启动流程 |
| `RTOS/Src/os_ipc.c` | 计数信号量、互斥锁（优先级继承）、定长消息队列 |
| `RTOS/Src/os_port.c` | PRIMASK 临界区、SVC/PendSV 汇编、首次任务启动 |
| `RTOS/README.md` | 本文档 |

应用示例位于 `Core/Src/main.c`，中断向量接缝位于 `Core/Src/stm32f1xx_it.c`。

---

## 2. 功能特性

- 32 级优先级抢占式调度，优先级数值越大越高；空闲任务固定优先级 0
- 同优先级任务按 `OS_CFG_TIME_SLICE_TICKS`（默认 1 tick）时间片轮转
- 任务：创建、删除、挂起/恢复（可嵌套）、延时、主动让出、运行时修改优先级
- 计数信号量：任务上下文等待/发布，中断上下文发布
- 互斥锁：优先级继承，检测递归加锁并返回错误
- 定长消息队列：元素为 `void *`（4 字节），支持任务/中断上下文收发
- 全部内核对象与 TCB 静态分配，不使用 `malloc`
- 栈填充魔数 `0xA5A5A5A5`，可查询历史最大栈使用量
- 参数断言与关键链表不变量检查（可配置关闭）
- 空闲任务使用 `__WFI()`，无任务可运行时自动休眠

---

## 3. 调度器设计

### 3.1 就绪队列

- 每个优先级维护一条**环形双向链表** `os_ready_head[prio]`
- `uint32_t os_ready_bitmap` 标记非空优先级
- 取最高优先级：`__CLZ(bitmap)`，O(1)
- 同优先级轮转：tick 中把当前优先级链表头指针后移一位，触发重调度
- 新任务/唤醒任务插入同优先级链表尾部，保证轮转公平

### 3.2 调度时机

以下操作会调用 `os_schedule()`，通过设置 `SCB->ICSR = PENDSVSET` 挂起 PendSV：

- 任务阻塞（等待信号量/互斥锁/队列、延时、挂起）
- 唤醒更高优先级任务（信号量发布、队列收发、恢复任务）
- 任务让出、删除、修改有效优先级
- tick 超时唤醒与时间片轮转

PendSV 优先级设为最低（15），保证所有 ISR 处理完毕后统一切换，避免在中途切换上下文。

### 3.3 任务状态

```
             os_task_create()
                    |
                    v
  +------------> READY <---------------------+
  |              |    ^                       |
  |        被调度 |    | 唤醒 / 超时 / 恢复      |
  |              v    |                       |
  |           RUNNING |                       |
  |              |    |                       |
  |   阻塞等待    |    |                       |
  |              v    |                       |
  |           BLOCKED |                       |
  |                   |                       |
  |  os_task_suspend  |                       |
  +-> SUSPENDED ------+--- os_task_resume ---+
                    |
              os_task_delete()
                    |
                    v
                DELETED（资源回收，TCB 变为 UNUSED）
```

`SUSPENDED` 优先级高于 `BLOCKED`：阻塞中的任务被挂起后，其等待会被中止
（返回 `OS_ERR_ABORT`），恢复后不会继续原来的等待。挂起计数 `suspend_count`
支持嵌套挂起，只有计数归零才真正恢复。

---

## 4. 上下文切换与栈帧

### 4.1 双栈模型

- 任务运行在 Thread 模式，使用 **PSP**
- 异常（SVC/PendSV/SysTick/外设中断）运行在 Handler 模式，使用 **MSP**
- 启动文件 `Stack_Size` 已从 `0x400` 增加到 `0x800`，为 MSP 留出余量

### 4.2 TCB 约束

`struct os_tcb` 的第一个成员必须是 `uint32_t *sp;`，PendSV 汇编直接通过
`[TCB + 0]` 读写栈指针。头文件中有编译期静态断言：

```c
OS_STATIC_ASSERT(offsetof(struct os_tcb, sp) == 0U);
```

### 4.3 首次启动栈帧

`os_task_create_internal()` 在任务栈顶构造 16 个 word（64 字节）的初始帧：

```
高地址（8 字节对齐的栈顶）
+-------------------------------+
| xPSR = 0x01000000 (Thumb 位)   |
| PC   = entry | 1               |
| LR   = os_task_exit | 1        |
| R12  = 0                       |
| R3   = 0                       |
| R2   = 0                       |
| R1   = 0                       |
| R0   = arg                     |
+-------------------------------+  <- 硬件异常帧起点，SVC 把 PSP 指向这里
| R11 ... R4 = 0                 |  <- TCB->sp 指向这里（软件保存区）
+-------------------------------+
低地址（栈底，填充 0xA5）
```

任务入口返回时会跳转到 `os_task_exit()`，该函数删除自身并进入死循环，
防止任务函数意外返回造成未定义行为。

### 4.4 PendSV 切换流程

1. 异常进入时硬件把 `R0-R3 / R12 / LR / PC / xPSR` 压入当前任务栈（PSP）
2. 汇编 `stmdb r0!, {r4-r11}` 保存 `R4-R11`，把新 PSP 写入 `TCB->sp`
3. `cpsid i` 关中断，调用 C 函数 `os_pendsv_switch()`：
   - 当前任务若为 `RUNNING` 改为 `READY`
   - 从就绪位图取最高优先级任务，设为 `RUNNING`
   - 重置时间片计数
4. `cpsie i` 开中断，从新任务 `TCB->sp` 恢复 `R4-R11` 与 PSP
5. `BX LR` 异常返回，硬件弹出剩余寄存器，任务继续运行

### 4.5 首次启动的竞态处理

`os_start()` 中：

1. 关中断，选最高优先级任务并设为 `RUNNING`
2. 置 `os_started = 1`
3. 设置 `SVCall_IRQn = 0`、`PendSV_IRQn = 15`、`SysTick_IRQn = 15`
4. 清除 PendSV/SysTick pending
5. **配置 SysTick 的 LOAD/VAL，但保持 `CTRL = 0`（关闭）**
6. 开中断，执行 `svc 0`

`SVC_Handler` 汇编在恢复第一个任务的 R4-R11、把 PSP 指向硬件栈帧**之后**，
才写 `SYST_CSR = 7` 启动 tick。这样保证 SysTick/PendSV 第一次可能发生时，
PSP 已经有效，不会出现 `PSP = 0` 时上下文切换导致的 HardFault。

---

## 5. API 参考

所有状态码定义于 `os_status_t`：`OS_OK`、`OS_ERR`、`OS_ERR_PARAM`、`OS_ERR_TIMEOUT`、
`OS_ERR_FULL`、`OS_ERR_EMPTY`、`OS_ERR_CONTEXT`、`OS_ERR_STATE`、`OS_ERR_NOMEM`、
`OS_ERR_ABORT`。

超时参数：`OS_WAIT_FOREVER`（永久等待）或 `OS_NO_WAIT`（不等待），其他值为 tick 数。

### 5.1 内核

| 函数 | 说明 |
|---|---|
| `os_status_t os_init(void)` | 初始化内核并创建空闲任务；只能调用一次 |
| `void os_start(void)` | 启动调度器，永不返回 |
| `uint32_t os_tick_get(void)` | 当前系统节拍数（1 tick = 1 ms） |
| `void os_tick_handler(void)` | 由 `SysTick_Handler` 调用，用户不应直接调用 |
| `uint32_t os_enter_critical(void)` | 关中断并返回进入前的 PRIMASK |
| `void os_exit_critical(uint32_t state)` | 恢复 PRIMASK |
| `void os_idle_hook(void)` | 弱定义，默认 `__WFI()`，可重写 |

### 5.2 任务

| 函数 | 说明 |
|---|---|
| `os_task_create(&task, name, entry, arg, stack, stack_words, prio)` | 创建静态栈任务，prio 1~31 |
| `os_task_delete(task)` | 删除任务，`NULL` 表示删除自己 |
| `os_task_suspend(task)` | 挂起任务，`NULL` 表示挂起自己，可嵌套 |
| `os_task_resume(task)` | 恢复任务，计数归零后进入就绪 |
| `os_task_yield(void)` | 主动让出 CPU 给同优先级下一个任务 |
| `void os_task_delay(uint32_t ticks)` | 阻塞延时，单位 tick |
| `os_task_set_priority(task, prio)` | 修改基础优先级，自动重算有效优先级 |
| `os_task_self()` / `os_task_name(task)` | 获取当前任务 / 任务名 |
| `os_task_get_info(task, &info)` | 查询状态、基础/有效优先级、栈用量 |
| `os_task_stack_used(task)` | 历史最大栈使用量（word） |

栈由用户提供，单位为 `uint32_t`；最小值为 `OS_CFG_MIN_STACK_WORDS`（默认 32 word）。

### 5.3 计数信号量

| 函数 | 说明 |
|---|---|
| `os_sem_init(&sem, initial, max_count)` | 初始化计数与上限 |
| `os_sem_wait(&sem, timeout)` | 等待计数大于 0，计数减 1 |
| `os_sem_post(&sem)` | 计数加 1 并唤醒最高优先级等待者；满则返回 `OS_ERR_FULL` |
| `os_sem_post_isr(&sem)` | 中断上下文版本 |
| `os_sem_get_count(&sem)` | 读取当前计数 |

### 5.4 互斥锁

| 函数 | 说明 |
|---|---|
| `os_mutex_init(&mutex)` | 初始化 |
| `os_mutex_lock(&mutex, timeout)` | 加锁，支持优先级继承；递归加锁返回 `OS_ERR_STATE` |
| `os_mutex_unlock(&mutex)` | 解锁并唤醒最高优先级等待者 |
| `os_mutex_get_owner(&mutex)` | 查询持有者 |

互斥锁只能在任务上下文使用，不支持中断中加锁/解锁。

### 5.5 消息队列

| 函数 | 说明 |
|---|---|
| `os_queue_init(&queue, buffer, capacity)` | buffer 为 `void *` 数组，容量由用户提供 |
| `os_queue_send(&queue, msg, timeout)` | 入队，满则按超时阻塞 |
| `os_queue_recv(&queue, &msg, timeout)` | 出队，空则按超时阻塞 |
| `os_queue_send_isr(&queue, msg)` | 中断上下文入队，不阻塞 |
| `os_queue_recv_isr(&queue, &msg)` | 中断上下文出队，不阻塞 |
| `os_queue_count(&queue)` | 当前消息数量 |

---

## 6. 中断使用规则

1. **阻塞类 API 禁止在中断中调用**：`os_task_delay`、`os_sem_wait`、`os_mutex_lock`、
   `os_queue_send/recv`（不带 `_isr`）会检查 `__get_IPSR()` 并返回 `OS_ERR_CONTEXT`。
2. 中断中只能使用 `_isr` 后缀函数，它们不会阻塞，只修改对象状态并挂起 PendSV。
3. 互斥锁及其优先级继承逻辑不能在中断中使用。
4. 临界区通过 PRIMASK 关中断，必须保持短小；`os_enter_critical` 保存旧 PRIMASK，
   `os_exit_critical` 恢复，而不是无条件开中断，因此支持临界区嵌套。
5. 本项目 `SysTick_Handler` 的顺序固定为：

   ```c
   HAL_IncTick();       /* 维持 HAL 时基，HAL_Delay 仍可使用 */
   os_tick_handler();   /* 内核节拍、超时扫描、时间片 */
   ```

   `os_tick_handler()` 在 `os_started == 0` 时直接返回，因此启动前的 HAL 初始化不受影响。

6. 外设中断优先级必须设置为**高于** PendSV/SysTick（数值小于 15）时，才能抢占内核
   临界区之外的任务；若使用 FreeRTOS 风格的 `FromISR` 唤醒，请注意 PendSV 会在
   中断退出后执行，这是本内核的预期行为。

---

## 7. 优先级继承

每个任务有两个优先级：

- `base_prio`：创建/修改时指定的原始优先级
- `eff_prio`：实际参与调度的优先级，等于 `max(base_prio, 所持有互斥锁的所有等待者 eff_prio)`

规则：

1. 任务等待互斥锁时，若自身 `eff_prio` 高于持有者 `eff_prio`，立即提升持有者
2. 持有者释放互斥锁后，重新扫描自己持有的所有互斥锁的等待者，计算新的 `eff_prio`
3. 等待者超时、被挂起或被删除时，从等待链表移除并触发持有者优先级回退
4. 唤醒互斥锁等待者时，选择等待链表中 `eff_prio` 最高的任务
5. 任务删除时，其持有的互斥锁全部释放并唤醒等待者

**限制**：优先级继承只基于等待者的当前 `eff_prio` 做一层提升，不会递归传播整条
多级阻塞链（A 等 B 的锁、B 等 C 的锁、C 等 A 的锁这类环路本身也是设计错误）。
对教学和常见嵌入式场景足够；若需要完整链式继承，可在 `os_task_recompute_prio`
中沿等待关系递归传播。

---

## 8. 配置项（os_config.h）

| 宏 | 默认值 | 说明 |
|---|---|---|
| `OS_CFG_MAX_TASKS` | 8 | TCB 池大小，包含空闲任务 |
| `OS_CFG_PRIO_COUNT` | 32 | 优先级数量，1~32；0 保留给空闲任务 |
| `OS_CFG_TICK_HZ` | 1000 | SysTick 频率，1 ms/tick |
| `OS_CFG_IDLE_STACK_WORDS` | 128 | 空闲任务栈（word） |
| `OS_CFG_MIN_STACK_WORDS` | 32 | 用户任务栈最小大小（word） |
| `OS_CFG_TIME_SLICE_TICKS` | 1 | 同优先级时间片；0 关闭轮转 |
| `OS_CFG_USE_ASSERT` | 1 | 参数/状态断言；发布时可关闭 |
| `OS_CFG_STACK_FILL_WORD` | `0xA5A5A5A5` | 栈填充魔数 |

修改配置后需要重新编译整个工程。`OS_CFG_MAX_TASKS` 直接影响静态 RAM 占用
（每个 TCB 约百余字节）。

---

## 9. 演示程序说明（Core/Src/main.c）

初始化顺序：`HAL_Init()` → `SystemClock_Config()`（72 MHz）→ `MX_GPIO_Init()` →
`MX_FSMC_Init()` → `MX_SPI2_Init()` → `demo_led_init()` → `os_init()` →
创建队列/信号量/互斥锁 → 创建 4 个任务 → `os_start()`。

| 任务 | 优先级 | 栈（word） | 行为 |
|---|---|---|---|
| `consumer` | 4 | 320 | 阻塞接收队列，ITM 打印数值，发布信号量 |
| `producer` | 3 | 320 | 每 200 ms 在互斥锁保护下递增共享计数并入队 |
| `led` | 2 | 256 | 每 500 ms 翻转 PC13 |
| `monitor` | 1 | 384 | 等待信号量（1 s 超时），打印各任务状态与栈用量，然后延时 3 s |
| `idle` | 0 | 128 | 内核自动创建，`__WFI()` |

观察方式：

- **PC13 LED**：约 1 Hz 闪烁（500 ms 翻转一次）。若开发板 LED 为低电平点亮，
  PC13 输出低时亮。
- **ITM/SWO**：在 Keil `Options for Target → Debug → Settings → Trace` 中设置
  Core Clock = 72 MHz，勾选 ITM Stimulus Port 0，打开 `Debug (printf) Viewer`。
  预期输出：

  ```
  RTOS starting...
  [consumer] value=1
  [consumer] value=2
  ...
  ==== RTOS monitor ====
    led state=BLOCKED prio=2/2 stack=xx/256 words
    producer state=BLOCKED prio=3/3 stack=xx/320 words
    consumer state=BLOCKED prio=4/4 stack=xx/320 words
    monitor state=RUNNING prio=1/1 stack=xx/384 words
  ======================
  ```

  具体状态随调度时刻变化；`stack=xx/yy` 为历史最大使用量/总大小。无调试器连接时，
  `ITM_SendChar` 检测到 ITM 未使能会直接返回，不会阻塞程序。

- `DEMO_USE_ITM` 置 0 可完全关闭打印，消除 ITM 开销。

---

## 10. 构建与验证

### 10.1 Keil GUI

打开 `MDK-ARM/lvgl1.uvprojx`，目标 `lvgl1`，Build（F7）。

### 10.2 命令行

```bash
cd MDK-ARM
"D:/keil5/UV4/UV4.exe" -b lvgl1.uvprojx -t lvgl1 -j0 -o build.log
```

日志位于 `MDK-ARM/build.log`。

### 10.3 本次验证结果

```
Program Size: Code=8368 RO-data=372 RW-data=72 ZI-data=9264
"lvgl1\lvgl1.axf" - 0 Error(s), 0 Warning(s).
```

- Flash 占用：8368 + 372 + 72 = 8812 B，约占 256 KB 的 3.4%
- RAM 占用：72 + 9264 = 9336 B，约占 48 KB 的 19%
- 符号归属（来自 `lvgl1.map`）：
  - `SVC_Handler`、`PendSV_Handler` 位于 `os_port.o(.emb_text)`，大小为 32 B / 52 B
  - `SysTick_Handler` 位于 `stm32f1xx_it.o`，调用 `HAL_IncTick` 与 `os_tick_handler`
  - 映射文件中不存在任何 `lv_*` 符号
- 工程文件：6 个 group、27 个文件；无 `../lvgl`、`LV_LVGL` 引用

### 10.4 硬件检查清单

1. 烧录 `MDK-ARM/lvgl1/lvgl1.hex` 或 `.axf`
2. PC13 LED 以约 1 Hz 闪烁
3. SWO 可见 `[consumer] value=N` 递增与周期性的 monitor 输出
4. 长时间运行无 HardFault；monitor 输出中栈用量应低于栈总大小
5. 若栈用量接近上限，增大对应任务的 `DEMO_*_STACK_WORDS`

---

## 11. 已知限制

- 内核对象与 TCB 全部静态分配，任务数上限由 `OS_CFG_MAX_TASKS` 决定
- 消息队列元素固定为 `void *`，不支持变长消息
- 互斥锁不支持递归加锁（检测到同一持有者再次加锁返回 `OS_ERR_STATE`）
- 互斥锁及优先级继承不可在中断上下文使用
- 优先级继承不做多级链式递归传播
- 临界区通过 PRIMASK 关闭全部可屏蔽中断，关中断时间会直接影响中断延迟；
  若需更低延迟，可改为 BASEPRI 临界区并划定内核可屏蔽优先级阈值
- 同优先级时间片长度固定，不支持按任务配置
- 不提供软件定时器、事件标志组、动态内存、运行时栈溢出检测（仅高水位统计）
- 任务删除/自删除后栈内存由用户静态管理，内核不做回收
- `os_task_delete(NULL)` 删除自身时进入死循环，不会返回

后续可按需扩展：软件定时器、事件标志、BASEPRI 临界区、基于 MPU 的栈保护、
多级优先级继承等。
