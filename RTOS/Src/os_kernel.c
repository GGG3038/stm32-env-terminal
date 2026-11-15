/**
  ******************************************************************************
  * @file    os_kernel.c
  * @brief   轻量级 RTOS 内核：任务管理、调度、节拍、时间管理
  *
  * 就绪链表：
  *   - 每个优先级一条环形双向链表，os_ready_head[prio] 为链表头
  *   - os_ready_bitmap 的 bit N 表示优先级 N 的链表非空
  *   - 正在运行的任务始终位于其优先级链表的头部
  *
  * 等待链表：
  *   - 每个内核对象（信号量/互斥锁/队列）各有一条双向链表
  *   - 任务 TCB 中保存链表头/尾指针的地址，便于 O(1) 摘除
  ******************************************************************************
  */
#include "os_internal.h"

/* 配置合法性检查 */
OS_STATIC_ASSERT(OS_CFG_PRIO_COUNT > 0U && OS_CFG_PRIO_COUNT <= 32U);
OS_STATIC_ASSERT(OS_CFG_MAX_TASKS > 0U && OS_CFG_MAX_TASKS < 256U);
OS_STATIC_ASSERT(OS_CFG_IDLE_STACK_WORDS >= OS_CFG_MIN_STACK_WORDS);
OS_STATIC_ASSERT(OS_CFG_TIME_SLICE_TICKS < 0x80000000U);

/*---------------------------------------------------------------------------
 * 内核全局量
 *--------------------------------------------------------------------------*/
static os_task_t os_tcb_pool[OS_CFG_MAX_TASKS];
static uint32_t os_idle_stack[OS_CFG_IDLE_STACK_WORDS];

static os_task_t *os_ready_head[OS_CFG_PRIO_COUNT];
static uint32_t os_ready_bitmap;
static os_task_t *os_idle_tcb;
static uint8_t os_kernel_initialized;
#if (OS_CFG_TIME_SLICE_TICKS > 0U)
static uint32_t os_slice_left;
#endif

os_task_t * volatile os_current_tcb = NULL;
volatile uint32_t os_tick = 0U;
volatile uint8_t os_started = 0U;
volatile uint32_t os_assert_line = 0U;

/*---------------------------------------------------------------------------
 * 断言
 *--------------------------------------------------------------------------*/
/*
 * 功能：内核断言失败时的统一出口。
 * 说明：保存出错行号后关闭中断并原地死循环，方便调试器暂停时通过
 *       os_assert_line 定位问题；OS_CFG_USE_ASSERT=0 时不会被调用。
 */
void os_assert_fail(const char *file, int line)
{
    (void)file;
    os_assert_line = (uint32_t)line;
    __disable_irq();
    for (;;)
    {
        __NOP();
    }
}

/*---------------------------------------------------------------------------
 * 就绪链表操作
 *--------------------------------------------------------------------------*/
/*
 * 功能：把任务插入其有效优先级对应的就绪环形链表。
 * 说明：at_head 非 0 时插入链表头（运行中的任务始终保持在表头），
 *       否则插入表尾；同时置位 os_ready_bitmap 中对应的优先级位。
 */
static void os_ready_add(os_task_t *task, int at_head)
{
    uint8_t prio = task->eff_prio;
    os_task_t *head = os_ready_head[prio];

    if (head == NULL)
    {
        task->ready_next = task;
        task->ready_prev = task;
        os_ready_head[prio] = task;
    }
    else
    {
        os_task_t *tail = head->ready_prev;

        task->ready_next = head;
        task->ready_prev = tail;
        tail->ready_next = task;
        head->ready_prev = task;

        if (at_head != 0)
        {
            os_ready_head[prio] = task;
        }
    }

    os_ready_bitmap |= (1UL << prio);
}

/*
 * 功能：把任务从它所在优先级的就绪环形链表中摘除。
 * 说明：兼容链表中只剩一个节点的情况；链表变空时清除位图中的对应位。
 *       任务阻塞、挂起、删除或改变优先级时调用。
 */
static void os_ready_remove(os_task_t *task)
{
    uint8_t prio = task->eff_prio;
    os_task_t *head = os_ready_head[prio];

    if (head == NULL)
    {
        return;
    }

    if (task->ready_next == task)
    {
        os_ready_head[prio] = NULL;
    }
    else
    {
        task->ready_prev->ready_next = task->ready_next;
        task->ready_next->ready_prev = task->ready_prev;

        if (head == task)
        {
            os_ready_head[prio] = task->ready_next;
        }
    }

    task->ready_next = NULL;
    task->ready_prev = NULL;

    if (os_ready_head[prio] == NULL)
    {
        os_ready_bitmap &= ~(1UL << prio);
    }
}

/*
 * 功能：找出当前就绪任务中优先级最高的任务。
 * 说明：用 __CLZ 在就绪位图中定位最高有效位，再取该优先级链表的表头，
 *       因此查找是 O(1) 的；没有任何就绪任务时返回 NULL。
 */
static os_task_t *os_ready_pick_highest(void)
{
    uint8_t prio;

    if (os_ready_bitmap == 0U)
    {
        return NULL;
    }

    prio = (uint8_t)(31U - (uint32_t)__CLZ(os_ready_bitmap));
    return os_ready_head[prio];
}

/*---------------------------------------------------------------------------
 * 等待链表操作
 *--------------------------------------------------------------------------*/
/*
 * 功能：把任务追加到指定等待链表的尾部。
 * 说明：head/tail 是内核对象中等待链表头/尾指针的地址，会记录到 TCB，
 *       以便任务超时或被中止时无需知道对象类型也能 O(1) 摘除。
 */
static void os_wait_list_add(os_task_t **head, os_task_t **tail,
                             os_task_t *task)
{
    task->wait_head = head;
    task->wait_tail = tail;
    task->wait_next = NULL;
    task->wait_prev = *tail;

    if (*tail != NULL)
    {
        (*tail)->wait_next = task;
    }
    else
    {
        *head = task;
    }

    *tail = task;
}

/*
 * 功能：把任务从它当前所在的等待链表中摘除。
 * 说明：利用 TCB 中记录的头/尾指针地址直接修改链表，并清空等待节点
 *       字段；任务不在等待链表时直接返回，可安全重复调用。
 */
static void os_wait_list_remove(os_task_t *task)
{
    os_task_t **head = task->wait_head;
    os_task_t **tail = task->wait_tail;

    if ((head == NULL) || (tail == NULL))
    {
        return;
    }

    if (task->wait_prev != NULL)
    {
        task->wait_prev->wait_next = task->wait_next;
    }
    else
    {
        *head = task->wait_next;
    }

    if (task->wait_next != NULL)
    {
        task->wait_next->wait_prev = task->wait_prev;
    }
    else
    {
        *tail = task->wait_prev;
    }

    task->wait_next = NULL;
    task->wait_prev = NULL;
    task->wait_head = NULL;
    task->wait_tail = NULL;
}

/*---------------------------------------------------------------------------
 * 调度触发与上下文切换
 *--------------------------------------------------------------------------*/
/*
 * 功能：请求一次任务切换。
 * 说明：调度器启动后向 ICSR 写入 PENDSVSET 挂起 PendSV，实际切换在
 *       中断退出后由 PendSV 完成；只置位不阻塞，可在临界区内调用。
 */
void os_schedule(void)
{
    if (os_started != 0U)
    {
        /* ICSR 的 PENDSVSET 是写 1 有效，不能使用 |= */
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
        __DSB();
        __ISB();
    }
}

/* 由 PendSV 汇编在关中断状态下调用 */
/*
 * 功能：PendSV 中的调度决策（由汇编在关中断状态下调用）。
 * 说明：当前任务若为 RUNNING 则改回 READY，再从就绪位图选出最高优先级
 *       任务设为 RUNNING、更新 os_current_tcb，并重置时间片计数；
 *       返回后由 PendSV 汇编恢复新任务的上下文。
 */
void os_pendsv_switch(void)
{
    os_task_t *current = os_current_tcb;
    os_task_t *next;

    if ((current != NULL) && (current->state == OS_TASK_RUNNING))
    {
        current->state = OS_TASK_READY;
    }

    next = os_ready_pick_highest();
    OS_ASSERT(next != NULL);

    next->state = OS_TASK_RUNNING;
    os_current_tcb = next;

#if (OS_CFG_TIME_SLICE_TICKS > 0U)
    os_slice_left = OS_CFG_TIME_SLICE_TICKS;
#endif
}

/*---------------------------------------------------------------------------
 * 阻塞 / 唤醒 / 等待中止
 *--------------------------------------------------------------------------*/
/*
 * 功能：把当前任务由运行态转为阻塞态并挂到指定等待链表。
 * 说明：记录等待对象、等待类型和绝对超时时刻后触发调度；调用后当前
 *       任务不再运行，直到被唤醒或超时。必须在临界区内调用。
 */
void os_block_current(os_task_t **head, os_task_t **tail,
                      void *obj, uint8_t wait_type, uint32_t timeout)
{
    os_task_t *current = os_current_tcb;

    OS_ASSERT(current != NULL);
    OS_ASSERT(current->state == OS_TASK_RUNNING);

    current->wait_obj = obj;
    current->wait_type = wait_type;
    current->wait_result = (int8_t)OS_ERR;
    current->has_timeout = (timeout != OS_WAIT_FOREVER) ? 1U : 0U;

    if (current->has_timeout != 0U)
    {
        current->wake_deadline = os_tick + timeout;
    }

    current->state = OS_TASK_BLOCKED;
    os_ready_remove(current);
    os_wait_list_add(head, tail, current);
    os_schedule();
}

/*
 * 功能：中止任务当前的等待（超时、挂起、删除时使用）。
 * 说明：从等待链表摘除，清空等待信息并记录等待结果；若原本在等互斥锁，
 *       还会通知持有者重算优先级，及时撤销优先级继承。
 */
void os_wait_unlink(os_task_t *task, os_status_t result)
{
    void *obj = task->wait_obj;
    uint8_t type = task->wait_type;

    os_wait_list_remove(task);

    task->wait_obj = NULL;
    task->wait_type = OS_WAIT_NONE;
    task->has_timeout = 0U;
    task->wait_result = (int8_t)result;

    /* 超时/挂起/删除导致离开互斥锁等待队列，需要回退持有者的优先级 */
    if (type == OS_WAIT_MUTEX)
    {
        os_mutex_waiter_removed((os_mutex_t *)obj);
    }
}

/*
 * 功能：从等待链表中唤醒有效优先级最高的一个任务。
 * 说明：摘除该任务、记录唤醒结果 OS_OK 并放回就绪链表；用于信号量发布、
 *       队列收发和互斥锁释放等"只唤醒一个等待者"的场景。
 * 返回：被唤醒的任务；链表为空时返回 NULL。
 */
os_task_t *os_wait_wake_one(os_task_t **head, os_task_t **tail,
                            os_status_t result)
{
    os_task_t *best = NULL;
    os_task_t *task;

    for (task = *head; task != NULL; task = task->wait_next)
    {
        if ((best == NULL) || (task->eff_prio > best->eff_prio))
        {
            best = task;
        }
    }

    if (best == NULL)
    {
        return NULL;
    }

    os_wait_list_remove(best);
    best->wait_obj = NULL;
    best->wait_type = OS_WAIT_NONE;
    best->has_timeout = 0U;
    best->wait_result = (int8_t)result;
    best->state = OS_TASK_READY;
    os_ready_add(best, 0);

    return best;
}

/*---------------------------------------------------------------------------
 * 优先级管理
 *--------------------------------------------------------------------------*/
/*
 * 功能：修改任务的有效优先级（调度实际使用的优先级）。
 * 说明：若任务处于就绪/运行态，先从旧优先级链表摘除，修改后按原位置
 *       重新插入；新优先级高于当前任务时触发抢占。
 *       供优先级继承和 os_task_recompute_prio 使用。
 */
void os_task_set_eff_prio(os_task_t *task, uint8_t prio)
{
    if (task->eff_prio == prio)
    {
        return;
    }

    if ((task->state == OS_TASK_READY) || (task->state == OS_TASK_RUNNING))
    {
        int was_running = (task->state == OS_TASK_RUNNING) ? 1 : 0;

        os_ready_remove(task);
        task->eff_prio = prio;
        os_ready_add(task, was_running);

        if ((was_running != 0) ||
            ((os_current_tcb != NULL) &&
             (task->eff_prio > os_current_tcb->eff_prio)))
        {
            os_schedule();
        }
    }
    else
    {
        task->eff_prio = prio;
    }
}

/* 有效优先级 = max(基础优先级, 本任务持有的所有互斥锁等待者的有效优先级) */
/*
 * 功能：重算任务的有效优先级。
 * 说明：有效优先级 = max(base_prio, 该任务持有的所有互斥锁的等待者
 *       有效优先级)；互斥锁解锁、等待者超时/挂起/删除时调用，
 *       用于撤销不再需要的优先级继承。
 */
void os_task_recompute_prio(os_task_t *task)
{
    uint8_t prio = task->base_prio;
    os_mutex_t *mutex;
    os_task_t *waiter;

    for (mutex = task->owned_mutexes; mutex != NULL; mutex = mutex->owner_next)
    {
        for (waiter = mutex->wait_head; waiter != NULL; waiter = waiter->wait_next)
        {
            if (waiter->eff_prio > prio)
            {
                prio = waiter->eff_prio;
            }
        }
    }

    os_task_set_eff_prio(task, prio);
}

/*---------------------------------------------------------------------------
 * 节拍处理
 *--------------------------------------------------------------------------*/
/*
 * 功能：系统节拍处理，由 SysTick_Handler 每 1ms 调用一次。
 * 说明：节拍计数加 1；扫描阻塞任务并把到期者唤醒（记录 OS_ERR_TIMEOUT）；
 *       按时间片轮转同优先级任务；需要时挂起 PendSV 触发调度。
 *       调度器未启动时直接返回，保证启动前的 HAL 时基正常。
 */
void os_tick_handler(void)
{
    uint32_t primask;
    uint32_t i;
    uint8_t need_sched = 0U;

    if (os_started == 0U)
    {
        return;
    }

    primask = os_enter_critical();
    os_tick++;

    /* 扫描所有阻塞任务，处理超时 */
    for (i = 0U; i < OS_CFG_MAX_TASKS; i++)
    {
        os_task_t *task = &os_tcb_pool[i];

        if ((task->state == OS_TASK_BLOCKED) && (task->has_timeout != 0U) &&
            ((int32_t)(os_tick - task->wake_deadline) >= 0))
        {
            os_wait_unlink(task, OS_ERR_TIMEOUT);
            task->state = OS_TASK_READY;
            os_ready_add(task, 0);
            need_sched = 1U;
        }
    }

#if (OS_CFG_TIME_SLICE_TICKS > 0U)
    /* 同优先级时间片轮转 */
    if (os_slice_left > 0U)
    {
        os_slice_left--;
    }

    if (os_slice_left == 0U)
    {
        os_task_t *current = os_current_tcb;

        os_slice_left = OS_CFG_TIME_SLICE_TICKS;

        if (current != NULL)
        {
            os_task_t *head = os_ready_head[current->eff_prio];

            if ((head != NULL) && (head->ready_next != head))
            {
                os_ready_head[current->eff_prio] = head->ready_next;
                need_sched = 1U;
            }
        }
    }
#endif

    if (need_sched != 0U)
    {
        os_schedule();
    }

    os_exit_critical(primask);
}

/*---------------------------------------------------------------------------
 * 任务创建
 *--------------------------------------------------------------------------*/
/*
 * 功能：任务创建的内核实现（空闲任务也通过它创建）。
 * 说明：从 TCB 池找空闲槽位，用 0xA5 填充任务栈，在栈顶构造首次运行的
 *       异常帧（R0=arg、PC=入口、LR=os_task_exit、xPSR 置 Thumb 位），
 *       初始化 TCB 并加入就绪链表；新任务优先级更高时触发抢占。
 * 参数：task_out 可为 NULL；stack 由用户提供，单位为 uint32_t。
 * 返回：OS_OK 成功；OS_ERR_PARAM 参数非法；OS_ERR_NOMEM 无空闲 TCB。
 */
static os_status_t os_task_create_internal(os_task_t **task_out,
                                           const char *name,
                                           os_task_entry_t entry,
                                           void *arg,
                                           uint32_t *stack,
                                           uint32_t stack_words,
                                           uint8_t prio)
{
    os_task_t *task = NULL;
    uint32_t *top;
    uint32_t *sp;
    uint32_t i;
    uint32_t primask;

    if (task_out != NULL)
    {
        *task_out = NULL;
    }

    if ((entry == NULL) || (stack == NULL) ||
        (stack_words < OS_CFG_MIN_STACK_WORDS) ||
        (prio >= OS_CFG_PRIO_COUNT))
    {
        return OS_ERR_PARAM;
    }

    primask = os_enter_critical();

    for (i = 0U; i < OS_CFG_MAX_TASKS; i++)
    {
        if ((os_tcb_pool[i].state == OS_TASK_UNUSED) ||
            (os_tcb_pool[i].state == OS_TASK_DELETED))
        {
            task = &os_tcb_pool[i];
            break;
        }
    }

    if (task == NULL)
    {
        os_exit_critical(primask);
        return OS_ERR_NOMEM;
    }

    /* 填充栈魔数，用于统计高水位 */
    {
        volatile uint32_t *fill = stack;

        for (i = 0U; i < stack_words; i++)
        {
            fill[i] = OS_CFG_STACK_FILL_WORD;
        }
    }

    /* 栈顶 8 字节对齐 */
    top = (uint32_t *)(((uintptr_t)(stack + stack_words)) & ~(uintptr_t)7U);
    if ((uintptr_t)top < ((uintptr_t)stack +
                          (OS_PORT_STACK_FRAME_WORDS * sizeof(uint32_t))))
    {
        os_exit_critical(primask);
        return OS_ERR_PARAM;
    }

    sp = top - OS_PORT_STACK_FRAME_WORDS;

    for (i = 0U; i < OS_PORT_STACK_FRAME_WORDS; i++)
    {
        sp[i] = 0U;
    }

    sp[8] = (uint32_t)arg;                              /* R0  = arg          */
    sp[13] = ((uint32_t)os_task_exit) | 1U;             /* LR  = 任务退出     */
    sp[14] = ((uint32_t)entry) | 1U;                    /* PC  = 任务入口     */
    sp[15] = OS_PORT_INITIAL_XPSR;                      /* xPSR (T 位)        */

    task->sp = sp;
    task->name = name;
    task->entry = entry;
    task->arg = arg;
    task->stack_base = stack;
    task->stack_words = stack_words;
    task->base_prio = prio;
    task->eff_prio = prio;
    task->state = OS_TASK_READY;
    task->suspend_count = 0U;
    task->has_timeout = 0U;
    task->wait_type = OS_WAIT_NONE;
    task->wait_result = (int8_t)OS_OK;
    task->wake_deadline = 0U;
    task->wait_obj = NULL;
    task->wait_head = NULL;
    task->wait_tail = NULL;
    task->ready_next = NULL;
    task->ready_prev = NULL;
    task->wait_next = NULL;
    task->wait_prev = NULL;
    task->owned_mutexes = NULL;

    os_ready_add(task, 0);

    if ((os_started != 0U) && (os_current_tcb != NULL) &&
        (prio > os_current_tcb->eff_prio))
    {
        os_schedule();
    }

    os_exit_critical(primask);

    if (task_out != NULL)
    {
        *task_out = task;
    }

    return OS_OK;
}

/*---------------------------------------------------------------------------
 * 空闲任务与内核初始化/启动
 *--------------------------------------------------------------------------*/
/*
 * 功能：空闲任务主体，系统中没有其他就绪任务时运行。
 * 说明：循环调用 os_idle_hook()，默认执行 __WFI() 让 CPU 进入低功耗等待。
 */
static void os_idle_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        os_idle_hook();
    }
}

/*
 * 功能：空闲任务钩子，弱定义，允许用户提供同名函数覆盖。
 * 说明：默认执行 __WFI()；可在低功耗项目中改为进入 STOP/SLEEP 模式，
 *       或在此执行后台维护工作。
 */
__weak void os_idle_hook(void)
{
    __WFI();
}

/*
 * 功能：初始化内核。
 * 说明：清空全局状态和 TCB 池，并创建优先级 0 的空闲任务；必须在创建
 *       其他任务之前调用，且只能成功初始化一次。
 * 返回：OS_OK 成功；OS_ERR_STATE 重复初始化；其他为创建空闲任务失败。
 */
os_status_t os_init(void)
{
    uint32_t i;
    os_status_t status;

    if (os_kernel_initialized != 0U)
    {
        return OS_ERR_STATE;
    }

    os_current_tcb = NULL;
    os_tick = 0U;
    os_started = 0U;
    os_ready_bitmap = 0U;

    for (i = 0U; i < OS_CFG_PRIO_COUNT; i++)
    {
        os_ready_head[i] = NULL;
    }

    for (i = 0U; i < OS_CFG_MAX_TASKS; i++)
    {
        os_tcb_pool[i].state = OS_TASK_UNUSED;
        os_tcb_pool[i].owned_mutexes = NULL;
    }

    status = os_task_create_internal(&os_idle_tcb, "idle", os_idle_task, NULL,
                                     os_idle_stack, OS_CFG_IDLE_STACK_WORDS, 0U);
    if (status != OS_OK)
    {
        return status;
    }

    os_kernel_initialized = 1U;
    return OS_OK;
}

/*
 * 功能：启动调度器并运行第一个任务，永不返回。
 * 说明：选出最高优先级任务；配置 SVC 优先级 0、PendSV/SysTick 优先级 15；
 *       先设好 SysTick 重装值但保持关闭，再通过 SVC 启动第一个任务，
 *       由 SVC 在 PSP 有效后才打开 SysTick，避免启动瞬间的切换竞态。
 */
void os_start(void)
{
    uint32_t primask;

    OS_ASSERT(os_kernel_initialized != 0U);

    primask = os_enter_critical();

    if (os_started != 0U)
    {
        os_exit_critical(primask);
        return;
    }

    os_current_tcb = os_ready_pick_highest();
    OS_ASSERT(os_current_tcb != NULL);
    os_current_tcb->state = OS_TASK_RUNNING;

#if (OS_CFG_TIME_SLICE_TICKS > 0U)
    os_slice_left = OS_CFG_TIME_SLICE_TICKS;
#endif

    os_started = 1U;

    /* SVC 最高，PendSV / SysTick 最低 */
    NVIC_SetPriority(SVCall_IRQn, 0U);
    NVIC_SetPriority(PendSV_IRQn, 15U);
    NVIC_SetPriority(SysTick_IRQn, 15U);
    NVIC_ClearPendingIRQ(PendSV_IRQn);
    NVIC_ClearPendingIRQ(SysTick_IRQn);

    /* 先配置 SysTick 但保持关闭，等 PSP 初始化完成后再由 SVC 打开 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = (uint32_t)((SystemCoreClock / OS_CFG_TICK_HZ) - 1U);
    SysTick->VAL = 0U;

    os_exit_critical(primask);

    os_port_start_first_task();

    /* 不会返回 */
    for (;;)
    {
        __NOP();
    }
}

/*---------------------------------------------------------------------------
 * 任务管理 API
 *--------------------------------------------------------------------------*/
/*
 * 功能：检查任务句柄是否指向 TCB 池中的合法 TCB。
 * 说明：通过地址范围和"是否为 sizeof(os_task_t) 的整数倍"判断，防止
 *       野指针或非任务指针传给 API 造成内核数据结构破坏。
 * 返回：1 合法；0 非法。
 */
static int os_task_is_valid(const os_task_t *task)
{
    uintptr_t addr = (uintptr_t)task;
    uintptr_t base = (uintptr_t)&os_tcb_pool[0];
    uintptr_t end = base + sizeof(os_tcb_pool);

    if (addr < base || addr >= end)
    {
        return 0;
    }

    return (((addr - base) % sizeof(os_task_t)) == 0U) ? 1 : 0;
}

/*
 * 功能：创建一个新任务并使其进入就绪态。
 * 说明：公共 API 层做额外检查（内核已初始化、优先级不能为 0），实际
 *       创建工作由 os_task_create_internal 完成。
 * 参数：entry 任务入口；arg 传给入口的参数；stack 用户提供的栈；
 *       stack_words 栈大小（word）；prio 优先级 1~31，数值越大越高。
 * 返回：OS_OK 成功；OS_ERR_PARAM 参数非法；OS_ERR_NOMEM TCB 用尽。
 */
os_status_t os_task_create(os_task_t **task,
                           const char *name,
                           os_task_entry_t entry,
                           void *arg,
                           uint32_t *stack,
                           uint32_t stack_words,
                           uint8_t prio)
{
    if (os_kernel_initialized == 0U)
    {
        return OS_ERR_STATE;
    }

    /* 优先级 0 保留给空闲任务 */
    if ((prio == 0U) || (prio >= OS_CFG_PRIO_COUNT))
    {
        return OS_ERR_PARAM;
    }

    return os_task_create_internal(task, name, entry, arg, stack,
                                   stack_words, prio);
}

/*
 * 功能：删除任务；task 为 NULL 时删除当前任务。
 * 说明：把目标任务从就绪/等待链表摘除，释放它持有的全部互斥锁并唤醒
 *       相关等待者，标记为 DELETED，然后触发调度。
 * 返回：OS_OK 成功；OS_ERR_PARAM 句柄非法；OS_ERR_STATE 不允许删除。
 * 注意：删除自己时不会返回；空闲任务不允许删除。
 */
os_status_t os_task_delete(os_task_t *task)
{
    os_task_t *target;
    uint32_t primask;

    target = (task == NULL) ? os_current_tcb : task;

    if ((target == NULL) || (os_task_is_valid(target) == 0))
    {
        return OS_ERR_PARAM;
    }

    if ((target->state == OS_TASK_UNUSED) || (target->state == OS_TASK_DELETED))
    {
        return OS_ERR_STATE;
    }

    /* 空闲任务不允许删除 */
    if (target == os_idle_tcb)
    {
        return OS_ERR_STATE;
    }

    primask = os_enter_critical();

    if (target->state == OS_TASK_BLOCKED)
    {
        os_wait_unlink(target, OS_ERR_ABORT);
    }
    else if ((target->state == OS_TASK_READY) ||
             (target->state == OS_TASK_RUNNING))
    {
        os_ready_remove(target);
    }

    /* 释放该任务持有的互斥锁，避免死锁 */
    os_mutex_release_all(target);

    target->state = OS_TASK_DELETED;
    target->suspend_count = 0U;
    target->wait_result = (int8_t)OS_ERR;
    target->owned_mutexes = NULL;

    os_schedule();

    os_exit_critical(primask);

    if (target == os_current_tcb)
    {
        /* 等待 PendSV 切走，正常情况下不会执行到这里 */
        for (;;)
        {
            __NOP();
        }
    }

    return OS_OK;
}

/*
 * 功能：挂起任务；task 为 NULL 时挂起当前任务。
 * 说明：支持嵌套挂起，suspend_count 每调用一次加 1；若任务正在等待内核
 *       对象，其等待会被中止（返回 OS_ERR_ABORT）。挂起期间不参与调度。
 * 返回：OS_OK 成功；OS_ERR_PARAM 句柄非法；OS_ERR_STATE 不能挂起。
 */
os_status_t os_task_suspend(os_task_t *task)
{
    os_task_t *target;
    uint32_t primask;

    target = (task == NULL) ? os_current_tcb : task;

    if ((target == NULL) || (os_task_is_valid(target) == 0))
    {
        return OS_ERR_PARAM;
    }

    if ((target == os_idle_tcb) ||
        (target->state == OS_TASK_UNUSED) ||
        (target->state == OS_TASK_DELETED))
    {
        return OS_ERR_STATE;
    }

    primask = os_enter_critical();

    /* 支持嵌套挂起 */
    if (target->suspend_count > 0U)
    {
        if (target->suspend_count < 0xFFU)
        {
            target->suspend_count++;
        }
        os_exit_critical(primask);
        return OS_OK;
    }

    target->suspend_count = 1U;

    if (target->state == OS_TASK_BLOCKED)
    {
        os_wait_unlink(target, OS_ERR_ABORT);
        target->state = OS_TASK_SUSPENDED;
        os_schedule();
    }
    else if ((target->state == OS_TASK_READY) ||
             (target->state == OS_TASK_RUNNING))
    {
        os_ready_remove(target);
        target->state = OS_TASK_SUSPENDED;

        if (target == os_current_tcb)
        {
            os_schedule();
        }
    }

    os_exit_critical(primask);
    return OS_OK;
}

/*
 * 功能：恢复被挂起的任务。
 * 说明：suspend_count 减 1，减到 0 时把任务放回就绪链表；若恢复后的
 *       任务优先级高于当前任务，立即触发抢占。
 * 返回：OS_OK 成功；OS_ERR_PARAM 句柄非法；OS_ERR_STATE 任务未被挂起。
 */
os_status_t os_task_resume(os_task_t *task)
{
    uint32_t primask;

    if ((task == NULL) || (os_task_is_valid(task) == 0))
    {
        return OS_ERR_PARAM;
    }

    primask = os_enter_critical();

    if (task->state != OS_TASK_SUSPENDED)
    {
        os_exit_critical(primask);
        return OS_ERR_STATE;
    }

    if (task->suspend_count > 0U)
    {
        task->suspend_count--;
    }

    if (task->suspend_count == 0U)
    {
        task->state = OS_TASK_READY;
        os_ready_add(task, 0);

        if ((os_current_tcb != NULL) &&
            (task->eff_prio > os_current_tcb->eff_prio))
        {
            os_schedule();
        }
    }

    os_exit_critical(primask);
    return OS_OK;
}

/*
 * 功能：当前任务主动让出 CPU。
 * 说明：若同优先级就绪链表中还有别的任务，把链表头后移一位并触发调度，
 *       使下一个同优先级任务获得运行机会；没有同优先级任务时什么也不做。
 */
void os_task_yield(void)
{
    uint32_t primask;
    os_task_t *current;

    if ((os_started == 0U) || (os_current_tcb == NULL) ||
        (os_port_is_isr() != 0U))
    {
        return;
    }

    primask = os_enter_critical();
    current = os_current_tcb;
    {
        os_task_t *head = os_ready_head[current->eff_prio];

        if ((head != NULL) && (head->ready_next != head))
        {
            os_ready_head[current->eff_prio] = head->ready_next;
            os_schedule();
        }
    }
    os_exit_critical(primask);
}

/*
 * 功能：让当前任务阻塞指定 tick 数（默认 1 tick = 1ms）。
 * 说明：ticks 为 0 时退化为 os_task_yield()；否则记录绝对唤醒节拍并进入
 *       阻塞态，由 tick 中断到期唤醒。不能在中断中调用。
 */
void os_task_delay(uint32_t ticks)
{
    uint32_t primask;
    os_task_t *current;

    if ((os_started == 0U) || (os_current_tcb == NULL) ||
        (os_port_is_isr() != 0U))
    {
        return;
    }

    if (ticks == 0U)
    {
        os_task_yield();
        return;
    }

    primask = os_enter_critical();
    current = os_current_tcb;

    current->wait_obj = NULL;
    current->wait_type = OS_WAIT_NONE;
    current->wait_result = (int8_t)OS_OK;
    current->wait_head = NULL;
    current->wait_tail = NULL;
    current->has_timeout = 1U;
    current->wake_deadline = os_tick + ticks;
    current->state = OS_TASK_BLOCKED;

    os_ready_remove(current);
    os_schedule();

    os_exit_critical(primask);
}

/*
 * 功能：修改任务的基础优先级。
 * 说明：更新 base_prio 后重算有效优先级（可能仍被优先级继承抬高），并按
 *       新优先级重新放入就绪链表，必要时触发抢占。
 * 返回：OS_OK 成功；OS_ERR_PARAM 参数非法或目标为空闲任务。
 */
os_status_t os_task_set_priority(os_task_t *task, uint8_t prio)
{
    uint32_t primask;

    if ((task == NULL) || (os_task_is_valid(task) == 0))
    {
        return OS_ERR_PARAM;
    }

    if ((prio == 0U) || (prio >= OS_CFG_PRIO_COUNT) ||
        (task->state == OS_TASK_UNUSED) ||
        (task->state == OS_TASK_DELETED) || (task == os_idle_tcb))
    {
        return OS_ERR_PARAM;
    }

    primask = os_enter_critical();
    task->base_prio = prio;
    os_task_recompute_prio(task);
    os_exit_critical(primask);

    return OS_OK;
}

/*
 * 功能：返回当前正在运行的任务句柄。
 * 说明：即 os_current_tcb；任务可用它查询自身信息，或作为删除/挂起参数。
 */
os_task_t *os_task_self(void)
{
    return os_current_tcb;
}

/*
 * 功能：返回系统启动以来经过的节拍数。
 * 说明：1 tick 对应 OS_CFG_TICK_HZ 分之一秒（默认 1ms），用于延时、
 *       超时计算以及用户计时。
 */
uint32_t os_tick_get(void)
{
    return os_tick;
}

/*---------------------------------------------------------------------------
 * 任务信息与栈使用量
 *--------------------------------------------------------------------------*/
/*
 * 功能：统计任务栈的历史最大使用量（内部实现）。
 * 说明：从栈底开始数仍保持 0xA5 填充魔数的 word，剩余部分即历史最深
 *       使用量；该值只增不减，用于评估栈是否够用。
 * 返回：使用过的 word 数；栈信息无效时返回 0。
 */
static uint32_t os_stack_used(const os_task_t *task)
{
    uint32_t free_words = 0U;
    const volatile uint32_t *stack = task->stack_base;

    if ((stack == NULL) || (task->stack_words == 0U))
    {
        return 0U;
    }

    while ((free_words < task->stack_words) &&
           (stack[free_words] == OS_CFG_STACK_FILL_WORD))
    {
        free_words++;
    }

    return task->stack_words - free_words;
}

/*
 * 功能：一次性读取任务的名称、状态、优先级和栈用量。
 * 说明：在临界区内拷贝信息，保证得到一致的状态快照；供监控/调试任务使用。
 * 返回：OS_OK 成功；OS_ERR_PARAM 参数非法。
 */
os_status_t os_task_get_info(os_task_t *task, os_task_info_t *info)
{
    uint32_t primask;

    if ((task == NULL) || (info == NULL) ||
        (os_task_is_valid(task) == 0))
    {
        return OS_ERR_PARAM;
    }

    primask = os_enter_critical();

    info->name = task->name;
    info->state = (os_task_state_t)task->state;
    info->base_prio = task->base_prio;
    info->eff_prio = task->eff_prio;
    info->stack_words = task->stack_words;
    info->stack_used_words = os_stack_used(task);

    os_exit_critical(primask);
    return OS_OK;
}

/*
 * 功能：返回任务栈的历史最大使用量（单位 word）。
 * 说明：带句柄合法性检查；用于判断栈配置是否合理、能否缩小。
 * 返回：使用过的 word 数；句柄非法时返回 0。
 */
uint32_t os_task_stack_used(os_task_t *task)
{
    uint32_t used;
    uint32_t primask;

    if ((task == NULL) || (os_task_is_valid(task) == 0))
    {
        return 0U;
    }

    primask = os_enter_critical();
    used = os_stack_used(task);
    os_exit_critical(primask);

    return used;
}

/*
 * 功能：返回任务名称字符串。
 * 说明：名称是创建时传入的字符串指针，内核不做拷贝；句柄非法时返回 "?"。
 */
const char *os_task_name(os_task_t *task)
{
    if ((task == NULL) || (os_task_is_valid(task) == 0) ||
        (task->name == NULL))
    {
        return "?";
    }

    return task->name;
}

/*---------------------------------------------------------------------------
 * 任务入口返回时由此函数删除自己
 *--------------------------------------------------------------------------*/
/*
 * 功能：任务入口函数返回时的统一出口。
 * 说明：创建任务时 LR 被设为该函数地址，因此任务函数一旦 return 就会
 *       删除自身并进入死循环，避免返回到非法地址。
 */
void os_task_exit(void)
{
    (void)os_task_delete(NULL);

    for (;;)
    {
        __NOP();
    }
}
