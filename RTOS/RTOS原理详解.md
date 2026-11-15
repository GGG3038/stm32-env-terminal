# 这套轻量级 RTOS 是怎么工作的

> 面向第一次阅读这套代码的人。先建立整体模型，再顺着代码读下去。
> API 速查和构建方法见 [README.md](README.md)。

---

## 目录

1. [一句话理解 RTOS](#1-一句话理解-rtos)
2. [最小使用示例](#2-最小使用示例)
3. [内核的全局视角](#3-内核的全局视角)
4. [一个任务的一生](#4-一个任务的一生)
5. [TCB 里每个字段是干什么的](#5-tcb-里每个字段是干什么的)
6. [调度器：O(1) 找到最高优先级](#6-调度器o1-找到最高优先级)
7. [上下文切换：汇编到底做了什么](#7-上下文切换汇编到底做了什么)
8. [节拍与超时](#8-节拍与超时)
9. [阻塞模型：等待—重试](#9-阻塞模型等待重试)
10. [信号量、互斥锁、消息队列](#10-信号量互斥锁消息队列)
11. [优先级继承到底解决了什么](#11-优先级继承到底解决了什么)
12. [启动时序：为什么绕这么一圈](#12-启动时序为什么绕这么一圈)
13. [演示程序的完整时间线](#13-演示程序的完整时间线)
14. [推荐阅读代码的顺序](#14-推荐阅读代码的顺序)
15. [常见坑与调试方法](#15-常见坑与调试方法)
16. [三个动手实验](#16-三个动手实验)

---

## 1. 一句话理解 RTOS

**RTOS 本质上只做三件事：**

1. **任务管理**：把多个"永不返回的函数"（任务）保存下来，记住每个任务执行到哪了；
2. **调度**：决定此刻 CPU 该运行哪个任务，并在任务之间快速切换；
3. **同步与通信**：任务等资源时把它挂起，资源就绪时把它唤醒，并让任务之间安全地传数据。

没有 RTOS 时，你只能写一个 `while(1)` 大循环，靠状态机和 `HAL_Delay()` 轮流处理。
RTOS 把"谁先跑、跑多久、什么时候让出"这件事交给内核，你只需要把功能拆成一个个
独立的任务。

这套 RTOS 的特点：

- 抢占式：高优先级任务就绪后可以立刻打断低优先级任务
- 固定优先级 + 同优先级时间片轮转
- 全静态内存：没有 `malloc`，任务数、栈大小编译期确定
- 代码量小：三个 `.c` 文件合计约 2000 行（含详细注释）

---

## 2. 最小使用示例

```c
#include "os.h"

static uint32_t led_stack[128];    /* 栈由用户提供，单位是 word（4 字节） */
static os_task_t *led_task;

static void led_entry(void *arg)   /* 任务入口，永不返回 */
{
    (void)arg;
    for (;;)
    {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        os_task_delay(500);        /* 阻塞 500 个 tick（默认 1ms/tick） */
    }
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    os_init();                     /* 初始化内核，创建空闲任务 */

    os_task_create(&led_task, "led", led_entry, NULL,
                   led_stack, 128, 2);   /* 优先级 2，数值越大越高 */

    os_start();                    /* 启动调度器，永不返回 */
}
```

需要记住的几条规则：

- 任务函数通常写成 `for (;;)`；如果它真的 `return`，内核会删除这个任务
- `os_task_delay()` 不是"忙等"，它会立刻把 CPU 让给别的任务
- 栈大小单位是 **word**，128 word = 512 字节，不是 128 字节
- 优先级 0 保留给空闲任务，用户任务从 1 开始

---

## 3. 内核的全局视角

从内存角度，内核维护的东西并不多：

```
RAM（逻辑组成，非物理地址顺序）
+------------------------------------------------------+
| 启动文件预留的 MSP 栈（0x800 = 2KB）                  |
|   异常/中断使用，任务运行时不占用                       |
+------------------------------------------------------+
| TCB 池：os_tcb_pool[8]                                |
|   每个任务一个任务控制块，记录状态、栈指针、优先级……    |
+------------------------------------------------------+
| 调度数据：os_ready_head[32] + os_ready_bitmap          |
|   32 条就绪链表，位图标记哪些优先级非空                 |
+------------------------------------------------------+
| 空闲任务栈：os_idle_stack[128]                         |
+------------------------------------------------------+
| 用户任务栈：led 256 / producer 320 / consumer 320 /   |
|             monitor 384（word）                        |
+------------------------------------------------------+
| 应用程序的全局变量、HAL 状态、外设缓冲                  |
+------------------------------------------------------+
```

内核的"大脑"只有几个全局变量：

| 变量 | 作用 |
|---|---|
| `os_current_tcb` | 此刻正在运行的任务 |
| `os_tick` | 系统启动以来的节拍数 |
| `os_started` | 调度器是否已启动 |
| `os_ready_head[32]` | 每个优先级的就绪环形链表 |
| `os_ready_bitmap` | 哪些优先级有就绪任务 |
| `os_idle_tcb` | 空闲任务 |
| `os_tcb_pool[8]` | 所有 TCB 的静态存储 |

> 为什么全静态？嵌入式系统需要确定性：不会因为堆碎片导致运行几小时后分配失败，
> 也不需要线程安全的 `malloc`。代价是任务数上限固定为 `OS_CFG_MAX_TASKS`。

---

## 4. 一个任务的一生

```
                  os_task_create()
                         |
                         v
   +-----------------> READY <-------------------------+
   |                    |                              |
   |          调度器选中 |                              |
   |                    v                              |
   |                 RUNNING                           |
   |                    |                              |
   |    os_task_delay   |   os_sem_wait                |
   |    os_queue_recv   |   os_mutex_lock              |
   |    os_task_suspend |                              |
   |                    v                              |
   |                 BLOCKED                           |
   |                    |                              |
   |      被唤醒/超时    |  os_task_suspend             |
   |                    v                              |
   |                SUSPENDED                          |
   |                    |                              |
   |          os_task_resume（计数归零）                 |
   |                    |                              |
   +--------------------+------------------------------+
                         |
                   os_task_delete()
                         v
                     DELETED
```

对应到函数调用链：

| 阶段 | 发生的事 | 关键函数 |
|---|---|---|
| 创建 | 找空闲 TCB、填栈魔数、构造首次栈帧、加入就绪链表 | `os_task_create_internal` |
| 就绪 | 停留在就绪链表中，等待调度器选中 | `os_ready_add` |
| 运行 | PendSV 恢复它的寄存器，CPU 开始执行任务代码 | `os_pendsv_switch` |
| 阻塞 | 从就绪链表摘除；等对象时挂到其等待链表，纯延时只标记 BLOCKED | `os_block_current` / `os_task_delay` |
| 唤醒 | 从等待链表摘除（如有），放回就绪链表尾部 | `os_wait_wake_one` / `os_tick_handler` |
| 删除 | 摘除链表、释放持有的锁、标记 DELETED | `os_task_delete` |

**重要状态规则：**

- 一个任务同一时刻要么在就绪链表里，要么在某个等待链表里，要么两者都不在
- `RUNNING` 的任务也存在于就绪链表中（位于其优先级链表头部）
- `SUSPENDED` 优先于 `BLOCKED`：阻塞中的任务被挂起后，等待会被中止

**挂起的任务在哪里？**

`SUSPENDED` 任务不在就绪链表，也不在任何等待链表。它只存在于 TCB 池里：
`state == OS_TASK_SUSPENDED`、`suspend_count > 0`。如果它是从阻塞态被挂起的，
`os_task_suspend()` 已经调用 `os_wait_unlink()` 把它从对象等待链表摘除
（若等的是互斥锁，还会回退持有者的优先级），并清空了 `wait_head` / `wait_tail`。

内核没有"挂起链表"，因为没有任何调度路径需要遍历所有挂起任务：挂起任务只能
通过显式调用 `os_task_resume(task)` 恢复，而调用者本来就持有任务句柄；
tick 扫描超时也只检查 `state == BLOCKED` 的任务，会直接跳过 `SUSPENDED`。
所以挂起任务和纯延时任务类似，都是"TCB 池中的一个条目"，区别是它没有
`wake_deadline`，永远不会被时间唤醒，只能被 `resume`。

> 注意：挂起不会释放任务已经持有的互斥锁。如果低优先级任务持锁后被挂起，
> 等待该锁的高优先级任务会一直阻塞到它被恢复。设计时应避免长时间挂起持锁任务。

---

## 5. TCB 里每个字段是干什么的

`os_internal.h` 中的 `struct os_tcb` 是理解整个内核的钥匙。按用途分组：

### 5.1 上下文

```c
uint32_t *sp;    /* 必须是第一个成员，偏移为 0 */
```

`sp` 指向该任务栈上保存的寄存器现场。PendSV 汇编直接通过 `[TCB + 0]` 读写它，
因此结构体中有静态断言：

```c
OS_STATIC_ASSERT(offsetof(struct os_tcb, sp) == 0U);
```

如果有人在 `sp` 前面加字段，编译会直接失败，而不是运行时出现难以定位的 HardFault。

### 5.2 身份与栈信息

```c
const char *name;       /* 任务名，仅保存指针 */
os_task_entry_t entry;  /* 入口函数 */
void *arg;              /* 传给入口的参数 */
uint32_t *stack_base;   /* 栈底（低地址） */
uint32_t stack_words;   /* 栈大小，word */
```

这些主要用于构造首次栈帧、调试和栈用量统计。

### 5.3 调度

```c
uint8_t base_prio;      /* 创建时指定的优先级 */
uint8_t eff_prio;       /* 有效优先级（含优先级继承） */
volatile uint8_t state; /* READY/RUNNING/BLOCKED/... */
```

调度器只看 `eff_prio`。正常情况下它等于 `base_prio`；只有持有互斥锁并被
高优先级任务等待时，才会被临时抬高。

### 5.4 阻塞与超时

```c
uint8_t suspend_count;  /* 嵌套挂起计数 */
uint8_t has_timeout;    /* 本次等待是否带超时 */
uint8_t wait_type;      /* 在等什么：信号量/互斥锁/队列发/队列收 */
int8_t  wait_result;    /* 等待结果：OS_OK / OS_ERR_TIMEOUT / OS_ERR_ABORT */
uint32_t wake_deadline; /* 绝对超时节拍 */
void *wait_obj;         /* 等待的内核对象 */
os_task_t **wait_head;  /* 所在等待链表头指针的地址 */
os_task_t **wait_tail;  /* 所在等待链表尾指针的地址 */
```

`wait_head` / `wait_tail` 存的是"指针的地址"（二级指针），这是本内核的一个
小巧设计：任务超时或被删除时，内核不需要知道它等的是信号量还是队列，直接通过
这两个地址修改链表，就能 O(1) 摘除自己。

### 5.5 链表节点

```c
struct os_tcb *ready_next, *ready_prev;  /* 就绪环形链表节点 */
struct os_tcb *wait_next,  *wait_prev;   /* 等待链表节点（非环形） */
```

同一个 TCB 可以同时"挂在就绪链表"或"挂在等待链表"，但不会同时挂两边。

### 5.6 资源所有权

```c
struct os_mutex *owned_mutexes;  /* 本任务持有的互斥锁单链表 */
```

任务删除时需要释放它持有的所有锁；优先级继承也需要遍历这个链表重算优先级。

---

## 6. 调度器：O(1) 找到最高优先级

### 6.1 就绪链表

每个优先级一条**环形双向链表**：

```
os_ready_head[5] ---> +----------+      +----------+
                      | consumer | <--> | task B   |
                      +----------+      +----------+
                           ^                 |
                           +-----------------+   （环形）

os_ready_bitmap:  bit5 = 1
```

- 链表头就是该优先级当前应该运行的任务
- 同优先级任务按创建/唤醒顺序排队
- 运行中的任务保持在链表头部

#### 为什么是"环形 + 双向"？

先澄清一个常见误解：RTOS 的任务通常是 `for(;;)` 死循环，不存在"执行完成后
进入等待链表"。离开 RUNNING 只有四种去向：

| 情况 | 去向 |
|---|---|
| 调用 `os_task_delay()` | 不挂等待链表，只标记 BLOCKED，由 tick 扫描到期唤醒 |
| 等信号量/队列/互斥锁 | 移到该对象的**等待链表** |
| 被更高优先级任务抢占 | **仍在就绪链表**，状态改回 READY |
| 同优先级时间片用完 | **仍在就绪链表**，只是链表头后移一位 |
| 挂起或删除 | 移出所有链表 |

所以就绪链表不是一个"一次性任务队列"，而是**当前所有可运行任务的集合**，
正在运行的任务也在其中（位于表头）。

在这个前提下，环形双向链表让所有操作都是 O(1)：

| 操作 | 实现 | 复杂度 |
|---|---|---|
| 取最高优先级任务 | `os_ready_head[prio]` | O(1) |
| 同优先级轮转 | `head = head->ready_next` | O(1) |
| 尾插新任务 | `tail = head->ready_prev`，插到 tail 后 | O(1) |
| 删除任意任务 | `prev->next = next`；`next->prev = prev` | O(1) |

删除必须是 O(1) 的"任意位置删除"，因为挂起、删除、修改有效优先级都可能发生在
链表中间的某个任务上。单链表能 O(1) 删除表头，但删除中间节点要先从头遍历找
前驱，最坏 O(n)。环形则让"表尾"等于 `head->prev`，只用一个 head 指针就能
同时完成头插、尾插和轮转，不需要额外维护 tail 指针，也不用处理 NULL 边界。

> 在本项目最多 8 个任务的规模下，单链表的 O(n) 实际开销很小；选择环形双向链表
> 主要是为了让调度器的所有操作都有确定的 O(1) 上界，代码不变量也更简单。

### 6.2 位图 + CLZ

`os_ready_bitmap` 的第 N 位表示"优先级 N 有就绪任务"。找最高优先级只需：

```c
prio = 31 - __CLZ(os_ready_bitmap);
```

例如 `bitmap = 0x28`（二进制 `101000`，bit5 和 bit3 置位）：

```
__CLZ(0x28) = 26
31 - 26 = 5    -> 最高优先级是 5
```

无论创建了多少任务，这个查找都是固定时间，不需要遍历任务列表。

### 6.3 时间片轮转

同优先级有多个任务时，每个 tick 把链表头指针后移一位：

```c
os_ready_head[prio] = os_ready_head[prio]->ready_next;
```

这样同一优先级的任务轮流获得一个 tick 的运行时间（默认 `OS_CFG_TIME_SLICE_TICKS = 1`）。

### 6.4 `os_schedule()` 只负责"打个招呼"

它并不直接切换任务，只做一件事：

```c
SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;   /* 挂起 PendSV */
```

真正的切换发生在所有中断处理完之后，由 PendSV 完成。这样做的好处是：

- 内核函数可以在任意位置请求切换，不必关心当前是否在中断里
- 不会在修改链表的中途被切换打断
- 中断可以快速返回，切换开销集中在一处

---

## 7. 上下文切换：汇编到底做了什么

### 7.1 双栈模型

Cortex-M3 有两个栈指针：

- **MSP**：主栈，复位后默认使用，所有异常/中断使用它
- **PSP**：进程栈，每个任务有自己的 PSP 值

本内核中：**任务用 PSP，异常用 MSP**。这样任务栈只保存任务自己的数据，
中断嵌套不会吃掉任务栈。

### 7.2 首次运行：SVC 构造现场

任务还没运行过，栈上是内核提前构造好的"假现场"。`os_task_create_internal()`
在栈顶写入 16 个 word：

```
高地址（8 字节对齐）
+-------------------------------+
| xPSR = 0x01000000             |  Thumb 状态位
| PC   = 任务入口地址 | 1        |  地址最低位为 1 表示 Thumb
| LR   = os_task_exit | 1        |  任务 return 时的去处
| R12  = 0                      |
| R3   = 0                      |
| R2   = 0                      |
| R1   = 0                      |
| R0   = arg                    |  任务入口的第一个参数
+-------------------------------+  <- PSP 初始值，硬件异常帧从这里弹出
| R11 ... R4 = 0                |  <- TCB->sp 指向这里
+-------------------------------+
低地址（栈底，填满 0xA5）
```

`SVC_Handler` 做四件事：

1. 从 `TCB->sp` 恢复 R4-R11
2. 把 PSP 指向硬件异常帧的起点
3. **打开 SysTick**（写 `SYST_CSR = 7`）
4. 把 EXC_RETURN 改成"返回 Thread 模式 + 使用 PSP"，执行 `BX LR`

硬件随后自动弹出 R0-R3、R12、LR、PC、xPSR，任务开始执行。

### 7.3 后续切换：PendSV

任务 A 正在运行时，硬件已经用 PSP 把 R0-R3/R12/LR/PC/xPSR 压入 A 的栈。
PendSV 汇编再补做几件事：

```
1. mrs r0, psp                 ; 取 A 的 PSP
2. stmdb r0!, {r4-r11}         ; 保存 A 的 R4-R11
3. str r0, [A_TCB]             ; A_TCB->sp = r0
4. cpsid i; bl os_pendsv_switch; cpsie i
      -> 把 A 改回 READY（若它还是 RUNNING）
      -> 从位图选出最高优先级任务 B
      -> 标记 B 为 RUNNING，os_current_tcb = B
5. 从 B_TCB->sp 恢复 R4-R11
6. msr psp, r0                 ; PSP 指向 B 的硬件异常帧
7. bx lr                       ; 异常返回，硬件恢复 B 的剩余寄存器
```

一次切换保存/恢复 8 个软件寄存器 + 硬件自动保存的 8 个寄存器，共 16 个 word。

### 7.4 为什么 PendSV 优先级最低

PendSV 和 SysTick 都设为优先级 15（最低）。这样：

- 任何外设中断都可以先于 PendSV 执行，不会被上下文切换拖慢
- 多个中断同时挂起切换请求时，只在最后统一切换一次
- 中断处理期间不会发生任务切换

SVC 则设为最高优先级 0，因为它是启动第一个任务的"钥匙"，必须立刻执行。

### 7.5 为什么 SysTick 要等到 SVC 里才打开

这是一个真实的竞态：

```
os_start():
  1. 选第一个任务，os_current_tcb = 它
  2. 配好 SysTick 的 LOAD/VAL，但 CTRL 保持 0（不计数）
  3. svc 0
```

如果提前打开 SysTick，那么在 SVC 还没来得及把 PSP 设置好时，tick 就可能到来并
触发 PendSV；此时 `os_current_tcb->sp` 还没被正确加载/PSP 仍为 0，切换会直接
HardFault。

把"开 tick"这一步放进 SVC，在 PSP 已经有效之后执行，就从根上消除了这个窗口。

---

## 8. 节拍与超时

### 8.1 SysTick 中断

`SysTick_Handler` 每 1ms 执行一次：

```c
void SysTick_Handler(void)
{
    HAL_IncTick();       /* 维持 HAL 的 uwTick，HAL_Delay 仍可用 */
    os_tick_handler();   /* 内核对节拍的处理 */
}
```

`os_tick_handler()` 做三件事：

1. `os_tick++`
2. 扫描 TCB 池，把到期的阻塞任务唤醒，等待结果记为 `OS_ERR_TIMEOUT`
3. 处理同优先级时间片轮转，必要时请求调度

调度器未启动时（`os_started == 0`）直接返回，所以启动前调用 `HAL_Delay()` 不受影响。

### 8.2 超时比较为什么能处理回绕

`os_tick` 是 32 位无符号数，49 天左右回绕一次。判断"是否到期"不能直接用
`now >= deadline`，而是：

```c
if ((int32_t)(os_tick - task->wake_deadline) >= 0)  { 到期 }
```

无符号减法在回绕时仍然得到正确的时间差，再转成有符号数判断正负。
只要单次等待不超过 2^31 个 tick（约 24 天），结果就是正确的。

### 8.3 延时就是"带超时的阻塞"

`os_task_delay(n)` 不占用等待对象，只做：

```c
current->wake_deadline = os_tick + n;
current->has_timeout   = 1;
current->state         = OS_TASK_BLOCKED;
os_ready_remove(current);
os_schedule();
```

到期后由 `os_tick_handler()` 把它放回就绪链表。`os_task_delay(0)` 退化为
`os_task_yield()`，只让出 CPU，不阻塞。

注意：**纯延时任务不在任何等待链表里**。它没有等待对象，`wait_obj`、
`wait_head`、`wait_tail` 都是 `NULL`，只是 TCB 池中一个状态为 `BLOCKED`、
带有 `wake_deadline` 的条目。tick 处理时直接扫描 TCB 池（最多
`OS_CFG_MAX_TASKS` 个）找出到期任务。只有等信号量/队列/互斥锁的任务，
才会挂到各自对象的等待链表上。

---

## 9. 阻塞模型：等待—重试

这是理解信号量/队列行为的关键。所有阻塞 API 都是同一个套路：

```c
for (;;)
{
    enter_critical();

    if (条件满足)               /* 例如 count > 0、队列非空 */
    {
        取走资源;
        exit_critical();
        return OS_OK;
    }

    if (超时或 NO_WAIT)
    {
        exit_critical();
        return OS_ERR_TIMEOUT / OS_ERR_EMPTY / OS_ERR_FULL;
    }

    block_current(..., 剩余超时);   /* 挂到等待链表并触发切换 */
    exit_critical();

    if (OS_ERR_ABORT == wait_result)  /* 被挂起/删除中止 */
        return OS_ERR_ABORT;

    /* 被正常唤醒：不能直接返回成功，必须回到循环顶部重新检查 */
}
```

**为什么被唤醒后还要重新检查？**

因为"发布者唤醒你"和"你真正开始运行"之间有时间差。比如：

1. 任务 A 等待信号量，被挂起；
2. 任务 B 发布信号量，唤醒 A，但此时 A 还不能立刻运行（PendSV 尚未执行）；
3. 在这之前，更高优先级的任务 C 抢走了信号量；
4. A 终于运行，如果直接返回成功，就会出现"信号量计数为负"或"队列取到空数据"。

等待—重试模型保证：**只有真正拿到资源时才返回成功**。代价是被唤醒后可能
重新阻塞，但逻辑永远正确。

---

## 10. 信号量、互斥锁、消息队列

### 10.1 计数信号量：停车场车位

```c
typedef struct os_sem
{
    volatile uint32_t count;    /* 当前空位数量 */
    uint32_t max_count;         /* 最多多少空位 */
    os_task_t *wait_head;       /* 排队等空位的任务 */
    os_task_t *wait_tail;
} os_sem_t;
```

- `os_sem_wait()`：有空位就占一个（count--），没有就排队
- `os_sem_post()`：释放一个空位（count++），并唤醒排队中优先级最高的任务
- 计数达到 `max_count` 时再 post 返回 `OS_ERR_FULL`

典型用途：中断通知任务"数据来了"、任务间事件计数、生产者/消费者节流。

### 10.2 互斥锁：一把唯一的钥匙

```c
typedef struct os_mutex
{
    os_task_t *owner;           /* 谁拿着钥匙 */
    uint16_t nest_count;
    struct os_mutex *owner_next;/* 持有者所有锁的链表 */
    os_task_t *wait_head;       /* 排队等钥匙的任务 */
    os_task_t *wait_tail;
} os_mutex_t;
```

与信号量的关键区别：

- 有**所有者**概念，只有持有者能解锁
- 支持**优先级继承**，避免优先级反转
- 不可递归加锁：同一任务再次 lock 返回 `OS_ERR_STATE`
- **不能在中断中使用**：中断没有任务身份，也没有优先级可继承

### 10.3 消息队列：有格子的传送带

```c
typedef struct os_queue
{
    void **buffer;              /* 用户提供的 void* 数组 */
    uint32_t capacity;          /* 格子数量 */
    volatile uint32_t head;     /* 下一个要取的位置 */
    volatile uint32_t count;    /* 当前消息数量 */
    os_task_t *send_wait_head;  /* 队列满时排队等待发送的任务 */
    os_task_t *send_wait_tail;
    os_task_t *recv_wait_head;  /* 队列空时排队等待接收的任务 */
    os_task_t *recv_wait_tail;
} os_queue_t;
```

- 消息本身是 `void *`（4 字节），可以传整数、指针或指向静态缓冲区的地址
- 发送：`buffer[(head + count) % capacity] = msg`
- 接收：取 `buffer[head]`，`head = (head + 1) % capacity`
- 发送成功唤醒一个接收者；接收成功唤醒一个发送者
- **内存生命周期由用户负责**：队列只存指针，消息指向的数据必须一直有效

---

## 11. 优先级继承到底解决了什么

### 11.1 优先级反转

假设三个任务：

| 任务 | 优先级 | 行为 |
|---|---|---|
| H（High） | 5 | 需要互斥锁 |
| M（Medium） | 4 | 纯计算，不让出 |
| L（Low） | 2 | 先拿到互斥锁 |

没有优先级继承时：

```
L 拿到锁 -> H 就绪，抢占 L，H 等锁阻塞
         -> M 就绪，抢占 L（因为 L 优先级只有 2）
         -> M 一直计算，L 无法运行，也就无法解锁
         -> H 无限等待，直到 M 算完
```

H 被 M 间接"卡住"，这就是优先级反转。火星探路者号曾因这个问题反复复位。

### 11.2 本内核的做法

每个任务有 `base_prio`（原始）和 `eff_prio`（实际调度）：

```
H 等 L 的锁  ->  L.eff_prio = max(2, H.eff_prio 5) = 5
             ->  M（4）无法抢占 L（5）
             ->  L 顺利执行到 unlock
             ->  unlock 后重算 L.eff_prio = 2
             ->  H 被唤醒并拿到锁
```

代码路径：

- `os_mutex_lock()`：发现持有者优先级更低，调用 `os_task_set_eff_prio()` 抬高持有者
- `os_mutex_unlock()`：唤醒最高优先级等待者，再 `os_task_recompute_prio()` 回退自己
- 等待者超时/挂起/删除：`os_wait_unlink()` 通知持有者回退
- `os_task_recompute_prio()`：扫描自己持有的所有锁的等待者，取最大值

### 11.3 限制

- 不做多级链式传播（A 等 B、B 等 C 的环路本身是设计错误）
- 互斥锁不可递归
- 中断中不能使用互斥锁

对教学和常见嵌入式场景，这已经足够；如果需要完整链式继承，可以在重算函数中
沿等待关系递归。

### 11.4 代码对照

| 时机 | 函数 | 关键动作 |
|---|---|---|
| 新等待者到来 | `os_mutex_lock()` | 等待者优先级更高时，`os_task_set_eff_prio(owner, waiter->eff_prio)` |
| 获取到锁 | `os_mutex_lock()` | `os_task_recompute_prio(current)`，继承剩余等待者的优先级 |
| 释放锁 | `os_mutex_unlock()` | 唤醒最高优先级等待者，`os_task_recompute_prio(current)` 回退自己 |
| 等待者超时/挂起/删除 | `os_wait_unlink()` → `os_mutex_waiter_removed()` | 重算持有者优先级 |
| 任务删除 | `os_mutex_release_all()` | 逐个释放锁并唤醒最高优先级等待者 |
| 优先级真正生效 | `os_task_set_eff_prio()` | 在就绪链表中按新优先级重新挂载并触发抢占 |

---

## 12. 启动时序：为什么绕这么一圈

完整启动流程：

```
main()
 |
 | HAL_Init()            配置 SysTick、NVIC
 | SystemClock_Config()  72MHz，更新 SystemCoreClock
 | GPIO/FSMC/SPI 初始化
 |
 | os_init()
 |    |- 清空 TCB 池、位图
 |    |- 创建 idle 任务（优先级 0，栈 128 word）
 |    |- os_kernel_initialized = 1
 |
 | os_task_create(...)   创建 4 个演示任务，全部进入就绪链表
 |
 | os_start()
 |    |- 关中断
 |    |- 选出最高优先级任务（consumer，优先级 4），标记 RUNNING
 |    |- 设置 SVC=0，PendSV=15，SysTick=15
 |    |- 清除 PendSV/SysTick pending
 |    |- 配置 SysTick LOAD/VAL，但 CTRL=0（暂不计数）
 |    |- 开中断
 |    |- svc 0
 |
 v
SVC_Handler
 |    |- 从 consumer 的 TCB 恢复 R4-R11
 |    |- PSP 指向 consumer 的硬件异常帧
 |    |- SYST_CSR = 7（此刻才打开 tick）
 |    |- 返回 Thread 模式 + PSP
 |
 v
consumer 任务开始运行（从此 os_start 不再返回）
```

之后任何一次切换都走 PendSV，不再经过 SVC。

---

## 13. 演示程序的完整时间线

演示有四个用户任务：

| 任务 | 优先级 | 行为 |
|---|---|---|
| consumer | 4（最高） | 阻塞收队列，打印，发布信号量 |
| producer | 3 | 每 200ms 加锁递增计数，发送到队列 |
| led | 2 | 每 500ms 翻转 PC13 |
| monitor | 1（最低） | 等信号量，打印任务状态，延时 3s |
| idle | 0 | `__WFI()` |

前 3 秒发生的事：

| 时刻 | 事件 |
|---|---|
| 0ms | `os_start()` 选 consumer（优先级 4）先运行；队列为空，consumer 阻塞 |
| 0ms | producer（3）运行：加锁，计数=1，解锁；入队时唤醒 consumer |
| 0ms | consumer 优先级更高，立即抢占：打印 `value=1`，post 信号量，再次阻塞等队列 |
| 0ms | producer 继续运行到 `os_task_delay(200)`，阻塞 |
| 0ms | led（2）运行：翻转 PC13，延时 500ms |
| 0ms | monitor（1）运行：信号量已有计数，立即拿到；打印监控信息；延时 3000ms |
| 0ms+ | 所有用户任务阻塞，idle 运行 `__WFI()`，CPU 休眠 |
| 200ms | tick 唤醒 producer，生产 value=2，consumer 抢占并打印 |
| 400ms | producer 生产 value=3 |
| 500ms | led 醒来，再次翻转 PC13 |
| 600ms | producer 生产 value=4 |
| 800ms | producer 生产 value=5 |
| 1000ms | producer 生产 value=6 |
| 3000ms | monitor 醒来，再次打印监控信息 |

从这个时间线可以直观看到：

- 高优先级任务醒来后可以立即打断低优先级任务（consumer 抢占 producer）
- 低优先级任务在等待期间完全不占用 CPU（idle 执行 WFI）
- 所有延时都是并行的，不是 `HAL_Delay()` 那种阻塞整个程序

监控输出示例：

```
==== RTOS monitor ====
  led state=BLOCKED prio=2/2 stack=xx/256 words
  producer state=BLOCKED prio=3/3 stack=xx/320 words
  consumer state=BLOCKED prio=4/4 stack=xx/320 words
  monitor state=RUNNING prio=1/1 stack=xx/384 words
======================
```

`stack=xx/yy` 是历史最大使用量/总大小（word）。如果 xx 接近 yy，说明该任务栈
需要加大。

---

## 14. 推荐阅读代码的顺序

不要从 `os_kernel.c` 第一行开始顺序读，容易迷路。建议按"数据 → 启动 → 切换 →
调度 → 通信"的顺序：

| 步骤 | 文件/函数 | 目标 |
|---|---|---|
| 1 | `os_internal.h` 的 `struct os_tcb` | 认识任务的全部状态 |
| 2 | `os_init()` / `os_task_create_internal()` | 任务是怎么被创建出来的 |
| 3 | `os_start()` + `os_port.c` 的 `SVC_Handler` | 第一个任务怎么跑起来 |
| 4 | `os_port.c` 的 `PendSV_Handler` + `os_pendsv_switch()` | 任务怎么切换 |
| 5 | `os_ready_add/remove/pick_highest` | 调度器的数据结构 |
| 6 | `os_block_current()` + `os_tick_handler()` | 阻塞和超时 |
| 7 | `os_ipc.c` 的 `os_sem_wait()` | 等待—重试模型的典型实现 |
| 8 | `os_mutex_lock/unlock()` + `os_task_recompute_prio()` | 优先级继承 |
| 9 | `main.c` 的四个任务 | 看实际怎么用 |

如果只有 10 分钟，只读：

1. `struct os_tcb` 的字段分组
2. `os_task_create_internal()` 里的 16 个 word 栈帧
3. `PendSV_Handler` 汇编的 8 行核心
4. `os_sem_wait()` 的 `for(;;)` 循环

这四处理解了，整个内核就没有神秘感了。

---

## 15. 常见坑与调试方法

### 15.1 栈溢出

- 栈单位是 word，不是字节；`32` 表示 128 字节，不是 32 字节
- 首次启动栈帧就占 64 字节，任务函数调用、局部变量、中断嵌套都叠加在上面
- 定期看 monitor 输出的 `stack=已用/总量`，建议保留 20%~30% 余量
- 栈溢出通常表现为莫名其妙的 HardFault 或变量被改写
- 栈用量统计是近似值：如果任务数据中恰好出现 `0xA5A5A5A5`，统计会偏小

### 15.2 在中断里调用了阻塞 API

```c
void USART1_IRQHandler(void)
{
    os_sem_wait(&sem, OS_WAIT_FOREVER);   /* 错误 */
}
```

阻塞 API 会检查 `__get_IPSR()`，在中断中返回 `OS_ERR_CONTEXT`，不会挂起。
中断里只能用：

- `os_sem_post_isr()`
- `os_queue_send_isr()`
- `os_queue_recv_isr()`

互斥锁在任何情况下都不能在中断中使用。

### 15.3 在 `os_start()` 之前使用阻塞 API

`os_task_delay()`、`os_sem_wait()` 等在调度器启动前会直接返回
（`os_started == 0`），不会产生延时效果。启动前请用 `HAL_Delay()`。

### 15.4 优先级 0

优先级 0 保留给空闲任务。`os_task_create()` 传 0 会返回 `OS_ERR_PARAM`。
用户任务优先级范围是 1~31。

### 15.5 任务函数 return

任务函数返回后会跳转到 `os_task_exit()`，该函数删除任务并死循环。这不是
"正常退出"，任务被删除了，所以任务通常写成 `for(;;)`。

### 15.6 共享数据保护

- 两个任务共享变量：用互斥锁或临界区保护
- 任务与中断共享变量：在任务侧用临界区，在中断侧无需加锁（中断不会被打断）
- 只读常量不需要保护

### 15.7 HardFault 排查顺序

1. 看 `os_assert_line` 是否非 0（断言失败会关中断死循环）
2. 检查任务栈是否够大（monitor 输出、map 文件）
3. 检查是否在中断中调用了阻塞 API
4. 检查是否重复定义了 `SVC_Handler` / `PendSV_Handler`
5. 检查任务函数是否发生了栈溢出或数组越界

### 15.8 ITM 打印

- `ITM_SendChar` 在没有调试器时不会阻塞（ITM 未使能会直接返回）
- 调试器已使能 ITM 但 SWO 缓冲满时，`ITM_SendChar` 可能忙等；不要在中断里
  大量打印
- Keil 中 Core Clock 要设为 72MHz，并勾选 ITM Stimulus Port 0

---

## 16. 三个动手实验

### 实验一：观察优先级抢占

把 `monitor` 的优先级从 1 改成 5（高于 consumer），重新编译烧录。

预期：启动后 monitor 先运行；此时信号量还没有计数，monitor 会阻塞到 1 秒超时；
期间 producer/consumer 正常跑。1 秒后 monitor 打印一次，然后延时 3 秒。你会
在输出里看到 monitor 的第一次打印明显晚于 `value=1`。

### 实验二：观察队列积压

把 `consumer` 的优先级从 4 改成 1（最低），producer 仍是 3。

预期：producer 发送消息后 consumer 不会立即抢占，队列开始积压；consumer 只能
在 producer 延时期间运行，一次处理多条消息。ITM 输出会呈现"批量打印"的节奏。

### 实验三：观察栈溢出

把 `led` 任务的栈从 256 word 改成 32 word（最小允许值），重新编译运行。

预期：可能启动后很快 HardFault，或者 monitor 打印的 `led stack=32/32 words`
表明栈已经用满。改回 256 即可恢复。这个实验能直观说明"栈必须留余量"。

---

## 附：术语表

| 术语 | 含义 |
|---|---|
| 任务（Task） | 一个独立运行的函数，拥有自己的栈和优先级 |
| TCB | 任务控制块，保存任务的全部状态 |
| tick | 系统节拍，本内核默认 1ms |
| 抢占（Preemption） | 高优先级任务就绪后打断低优先级任务 |
| 上下文切换 | 保存当前任务寄存器、恢复另一个任务寄存器的过程 |
| 就绪链表 | 所有可以运行的任务按优先级组织的链表 |
| 等待链表 | 阻塞在同一个对象上的任务队列 |
| 优先级反转 | 高优先级任务被低优先级任务间接阻塞 |
| 优先级继承 | 临时抬高锁持有者的优先级以解除反转 |
| PendSV | Cortex-M 专用于上下文切换的异常，优先级最低 |
| SVC | 系统服务调用异常，用于启动第一个任务 |
| PSP / MSP | 进程栈指针 / 主栈指针 |
| ISR | 中断服务程序 |
| `_isr` API | 专门供中断上下文调用的非阻塞接口 |

---

理解这套 RTOS 后，你可以尝试的扩展方向：软件定时器、事件标志组、动态任务、
BASEPRI 临界区、Tickless 低功耗、MPU 栈保护。它们都建立在这里已经讲清楚的
TCB、就绪链表和等待链表之上。
