/**
  ******************************************************************************
  * @file    os_ipc.c
  * @brief   RTOS 进程间通信：信号量、互斥锁（优先级继承）、消息队列
  *
  * 阻塞模型：等待-重试
  *   - 条件满足则立即返回
  *   - 否则挂到对象等待链表，带剩余超时阻塞
  *   - 被唤醒后重新检查条件，避免资源被其他任务抢走时错误返回成功
  ******************************************************************************
  */
#include "os_internal.h"

/*
 * 功能：计算距离绝对超时时刻还剩多少 tick。
 * 说明：timeout 为 OS_WAIT_FOREVER 时原样返回；否则用无符号减法计算
 *       剩余值，并通过 (int32_t) 判断是否已到期，可安全处理节拍回绕。
 * 返回：剩余 tick 数；0 表示已超时。
 */
static uint32_t os_remaining(uint32_t deadline, uint32_t timeout)
{
    uint32_t remaining;

    if (timeout == OS_WAIT_FOREVER)
    {
        return OS_WAIT_FOREVER;
    }

    remaining = deadline - os_tick_get();

    /* 回绕安全：剩余时间超过 2^31 视为已过期 */
    if ((int32_t)remaining <= 0)
    {
        return 0U;
    }

    return remaining;
}

/*============================================================================
 * 计数信号量
 *===========================================================================*/
/*
 * 功能：初始化计数信号量。
 * 说明：设置初始计数、最大计数并清空等待链表。
 * 返回：OS_OK 成功；OS_ERR_PARAM 指针为空、上限为 0 或初值超过上限。
 */
os_status_t os_sem_init(os_sem_t *sem, uint32_t initial, uint32_t max_count)
{
    if ((sem == NULL) || (max_count == 0U) || (initial > max_count))
    {
        return OS_ERR_PARAM;
    }

    sem->count = initial;
    sem->max_count = max_count;
    sem->wait_head = NULL;
    sem->wait_tail = NULL;

    return OS_OK;
}

/*
 * 功能：等待并获取信号量（计数减 1）。
 * 说明：计数大于 0 时立即成功；否则挂到对象等待链表，按 timeout 等待；
 *       被唤醒后重新检查计数，避免资源被其他任务抢走时错误返回成功。
 * 返回：OS_OK 成功；OS_ERR_TIMEOUT 超时；OS_ERR_CONTEXT 在中断中调用。
 */
os_status_t os_sem_wait(os_sem_t *sem, uint32_t timeout)
{
    uint32_t deadline;

    if (sem == NULL)
    {
        return OS_ERR_PARAM;
    }

    if (os_port_is_isr() != 0U)
    {
        return OS_ERR_CONTEXT;
    }

    if (os_started == 0U)
    {
        return OS_ERR_STATE;
    }

    deadline = os_tick_get() + timeout;

    for (;;)
    {
        uint32_t primask = os_enter_critical();

        if (sem->count > 0U)
        {
            sem->count--;
            os_exit_critical(primask);
            return OS_OK;
        }

        if (timeout == OS_NO_WAIT)
        {
            os_exit_critical(primask);
            return OS_ERR_TIMEOUT;
        }

        if ((timeout != OS_WAIT_FOREVER) &&
            ((int32_t)(os_tick_get() - deadline) >= 0))
        {
            os_exit_critical(primask);
            return OS_ERR_TIMEOUT;
        }

        os_block_current(&sem->wait_head, &sem->wait_tail, sem,
                         OS_WAIT_SEM, os_remaining(deadline, timeout));
        os_exit_critical(primask);

        {
            int8_t result = os_current_tcb->wait_result;

            if (result != (int8_t)OS_OK)
            {
                return (os_status_t)result;
            }
        }
    }
}

/*
 * 功能：信号量发布的内部实现（任务和中断共用）。
 * 说明：计数未达上限时加 1，唤醒一个等待者并触发调度；已满则失败。
 * 返回：OS_OK 成功；OS_ERR_FULL 计数已达上限。
 */
static os_status_t os_sem_post_internal(os_sem_t *sem)
{
    os_status_t status = OS_OK;
    uint32_t primask = os_enter_critical();

    if (sem->count < sem->max_count)
    {
        sem->count++;
        (void)os_wait_wake_one(&sem->wait_head, &sem->wait_tail, OS_OK);
        os_schedule();
    }
    else
    {
        status = OS_ERR_FULL;
    }

    os_exit_critical(primask);
    return status;
}

/*
 * 功能：在任务上下文中发布信号量。
 * 说明：禁止在中断中调用（中断请用 os_sem_post_isr），实际逻辑委托给
 *       os_sem_post_internal。
 * 返回：OS_OK 成功；OS_ERR_PARAM 指针为空；OS_ERR_CONTEXT 上下文错误；
 *       OS_ERR_FULL 计数已满。
 */
os_status_t os_sem_post(os_sem_t *sem)
{
    if (sem == NULL)
    {
        return OS_ERR_PARAM;
    }

    if (os_port_is_isr() != 0U)
    {
        return OS_ERR_CONTEXT;
    }

    return os_sem_post_internal(sem);
}

/*
 * 功能：在中断上下文中发布信号量。
 * 说明：不阻塞，只修改计数、唤醒等待者并挂起 PendSV，切换推迟到中断
 *       退出后执行。
 * 返回：OS_OK 成功；OS_ERR_PARAM 指针为空；OS_ERR_FULL 计数已满。
 */
os_status_t os_sem_post_isr(os_sem_t *sem)
{
    if (sem == NULL)
    {
        return OS_ERR_PARAM;
    }

    return os_sem_post_internal(sem);
}

/*
 * 功能：读取信号量当前计数。
 * 说明：用于调试或监控；指针为空时返回 0。
 */
uint32_t os_sem_get_count(os_sem_t *sem)
{
    return (sem == NULL) ? 0U : sem->count;
}

/*============================================================================
 * 互斥锁（优先级继承）
 *===========================================================================*/
/*
 * 功能：初始化互斥锁。
 * 说明：清空持有者、递归计数和等待链表，使互斥锁处于未锁定状态。
 * 返回：OS_OK 成功；OS_ERR_PARAM 指针为空。
 */
os_status_t os_mutex_init(os_mutex_t *mutex)
{
    if (mutex == NULL)
    {
        return OS_ERR_PARAM;
    }

    mutex->owner = NULL;
    mutex->nest_count = 0U;
    mutex->owner_next = NULL;
    mutex->wait_head = NULL;
    mutex->wait_tail = NULL;

    return OS_OK;
}

/*
 * 功能：获取互斥锁，带优先级继承和超时。
 * 说明：未锁定时直接获取；已锁定则把等待者的有效优先级提升给持有者，
 *       然后阻塞等待；被唤醒后重新尝试获取。同一任务重复加锁返回错误。
 * 返回：OS_OK 成功；OS_ERR_TIMEOUT 超时或立即返回失败；OS_ERR_STATE
 *       递归加锁或内核未启动；OS_ERR_CONTEXT 在中断中调用。
 */
os_status_t os_mutex_lock(os_mutex_t *mutex, uint32_t timeout)
{
    uint32_t deadline;
    os_task_t *current;

    if (mutex == NULL)
    {
        return OS_ERR_PARAM;
    }

    if (os_port_is_isr() != 0U)
    {
        return OS_ERR_CONTEXT;
    }

    if (os_started == 0U)
    {
        return OS_ERR_STATE;
    }

    current = os_current_tcb;
    deadline = os_tick_get() + timeout;

    for (;;)
    {
        uint32_t primask = os_enter_critical();

        if (mutex->owner == NULL)
        {
            mutex->owner = current;
            mutex->nest_count = 1U;
            mutex->owner_next = current->owned_mutexes;
            current->owned_mutexes = mutex;

            /* 若队列中还有别的等待者，继承它们的优先级 */
            os_task_recompute_prio(current);

            os_exit_critical(primask);
            return OS_OK;
        }

        /* 不支持递归加锁 */
        if (mutex->owner == current)
        {
            os_exit_critical(primask);
            return OS_ERR_STATE;
        }

        if (timeout == OS_NO_WAIT)
        {
            os_exit_critical(primask);
            return OS_ERR_TIMEOUT;
        }

        if ((timeout != OS_WAIT_FOREVER) &&
            ((int32_t)(os_tick_get() - deadline) >= 0))
        {
            os_exit_critical(primask);
            return OS_ERR_TIMEOUT;
        }

        /* 优先级继承：等待者把自己的有效优先级提升给持有者 */
        if (current->eff_prio > mutex->owner->eff_prio)
        {
            os_task_set_eff_prio(mutex->owner, current->eff_prio);
        }

        os_block_current(&mutex->wait_head, &mutex->wait_tail, mutex,
                         OS_WAIT_MUTEX, os_remaining(deadline, timeout));
        os_exit_critical(primask);

        {
            int8_t result = os_current_tcb->wait_result;

            if (result != (int8_t)OS_OK)
            {
                return (os_status_t)result;
            }
        }
    }
}

/*
 * 功能：释放互斥锁并唤醒一个等待者。
 * 说明：把锁从持有者的持有链表摘除，选出等待者中有效优先级最高的任务
 *       唤醒，然后重算持有者自己的有效优先级（撤销继承）并触发调度。
 * 返回：OS_OK 成功；OS_ERR_STATE 当前任务并非持有者；OS_ERR_CONTEXT
 *       在中断中调用。
 */
os_status_t os_mutex_unlock(os_mutex_t *mutex)
{
    os_task_t *current;
    os_mutex_t **link;
    uint32_t primask;

    if (mutex == NULL)
    {
        return OS_ERR_PARAM;
    }

    if (os_port_is_isr() != 0U)
    {
        return OS_ERR_CONTEXT;
    }

    current = os_current_tcb;
    if (current == NULL)
    {
        return OS_ERR_STATE;
    }

    primask = os_enter_critical();

    if ((mutex->owner != current) || (mutex->nest_count == 0U))
    {
        os_exit_critical(primask);
        return OS_ERR_STATE;
    }

    if (mutex->nest_count > 1U)
    {
        mutex->nest_count--;
        os_exit_critical(primask);
        return OS_OK;
    }

    /* 从持有者链表中摘除 */
    link = &current->owned_mutexes;
    while (*link != NULL)
    {
        if (*link == mutex)
        {
            *link = mutex->owner_next;
            break;
        }
        link = &(*link)->owner_next;
    }

    mutex->owner = NULL;
    mutex->owner_next = NULL;
    mutex->nest_count = 0U;

    (void)os_wait_wake_one(&mutex->wait_head, &mutex->wait_tail, OS_OK);

    /* 释放后重新计算自己的有效优先级 */
    os_task_recompute_prio(current);
    os_schedule();

    os_exit_critical(primask);
    return OS_OK;
}

/*
 * 功能：返回互斥锁当前的持有者任务。
 * 说明：未锁定或指针为空时返回 NULL，主要用于调试。
 */
os_task_t *os_mutex_get_owner(os_mutex_t *mutex)
{
    return (mutex == NULL) ? NULL : mutex->owner;
}

/*
 * 功能：等待者因超时/挂起/删除离开互斥锁等待链表后的回调。
 * 说明：让持有者重新计算有效优先级，及时撤销不再需要的优先级继承。
 */
void os_mutex_waiter_removed(os_mutex_t *mutex)
{
    if ((mutex != NULL) && (mutex->owner != NULL))
    {
        os_task_recompute_prio(mutex->owner);
    }
}

/*
 * 功能：释放任务持有的所有互斥锁。
 * 说明：任务被删除时调用，逐个把锁置为未锁定并唤醒最高优先级等待者，
 *       防止任务消失后锁永远无法释放造成死锁。
 */
void os_mutex_release_all(os_task_t *task)
{
    os_mutex_t *mutex;

    while (task->owned_mutexes != NULL)
    {
        mutex = task->owned_mutexes;
        task->owned_mutexes = mutex->owner_next;

        mutex->owner = NULL;
        mutex->owner_next = NULL;
        mutex->nest_count = 0U;

        (void)os_wait_wake_one(&mutex->wait_head, &mutex->wait_tail, OS_OK);
    }
}

/*============================================================================
 * 定长消息队列
 *===========================================================================*/
/*
 * 功能：初始化定长消息队列。
 * 说明：绑定用户提供的 void* 缓冲区，设置容量并清空计数和收发等待链表。
 * 返回：OS_OK 成功；OS_ERR_PARAM 指针为空或容量为 0。
 */
os_status_t os_queue_init(os_queue_t *queue, void **buffer, uint32_t capacity)
{
    if ((queue == NULL) || (buffer == NULL) || (capacity == 0U))
    {
        return OS_ERR_PARAM;
    }

    queue->buffer = buffer;
    queue->capacity = capacity;
    queue->head = 0U;
    queue->count = 0U;
    queue->send_wait_head = NULL;
    queue->send_wait_tail = NULL;
    queue->recv_wait_head = NULL;
    queue->recv_wait_tail = NULL;

    return OS_OK;
}

/*
 * 功能：向队列发送一条消息（任务上下文）。
 * 说明：队列未满时写入缓冲区并唤醒一个接收者；已满则按 timeout 阻塞，
 *       被唤醒后重新检查空间。消息本身为 void*，生命周期由用户管理。
 * 返回：OS_OK 成功；OS_ERR_FULL/OS_ERR_TIMEOUT 队列满/超时；
 *       OS_ERR_CONTEXT 在中断中调用。
 */
os_status_t os_queue_send(os_queue_t *queue, void *msg, uint32_t timeout)
{
    uint32_t deadline;

    if ((queue == NULL) || (queue->buffer == NULL))
    {
        return OS_ERR_PARAM;
    }

    if (os_port_is_isr() != 0U)
    {
        return OS_ERR_CONTEXT;
    }

    if (os_started == 0U)
    {
        return OS_ERR_STATE;
    }

    deadline = os_tick_get() + timeout;

    for (;;)
    {
        uint32_t primask = os_enter_critical();

        if (queue->count < queue->capacity)
        {
            uint32_t index = (queue->head + queue->count) % queue->capacity;

            queue->buffer[index] = msg;
            queue->count++;

            (void)os_wait_wake_one(&queue->recv_wait_head,
                                   &queue->recv_wait_tail, OS_OK);
            os_schedule();

            os_exit_critical(primask);
            return OS_OK;
        }

        if (timeout == OS_NO_WAIT)
        {
            os_exit_critical(primask);
            return OS_ERR_FULL;
        }

        if ((timeout != OS_WAIT_FOREVER) &&
            ((int32_t)(os_tick_get() - deadline) >= 0))
        {
            os_exit_critical(primask);
            return OS_ERR_TIMEOUT;
        }

        os_block_current(&queue->send_wait_head, &queue->send_wait_tail,
                         queue, OS_WAIT_QUEUE_SEND,
                         os_remaining(deadline, timeout));
        os_exit_critical(primask);

        {
            int8_t result = os_current_tcb->wait_result;

            if (result != (int8_t)OS_OK)
            {
                return (os_status_t)result;
            }
        }
    }
}

/*
 * 功能：从队列接收一条消息（任务上下文）。
 * 说明：队列非空时取出消息并唤醒一个发送者；为空则按 timeout 阻塞，
 *       被唤醒后重新检查。接收结果通过 *msg 输出。
 * 返回：OS_OK 成功；OS_ERR_EMPTY/OS_ERR_TIMEOUT 队列空/超时；
 *       OS_ERR_CONTEXT 在中断中调用。
 */
os_status_t os_queue_recv(os_queue_t *queue, void **msg, uint32_t timeout)
{
    uint32_t deadline;

    if ((queue == NULL) || (queue->buffer == NULL) || (msg == NULL))
    {
        return OS_ERR_PARAM;
    }

    if (os_port_is_isr() != 0U)
    {
        return OS_ERR_CONTEXT;
    }

    if (os_started == 0U)
    {
        return OS_ERR_STATE;
    }

    deadline = os_tick_get() + timeout;

    for (;;)
    {
        uint32_t primask = os_enter_critical();

        if (queue->count > 0U)
        {
            *msg = queue->buffer[queue->head];
            queue->head = (queue->head + 1U) % queue->capacity;
            queue->count--;

            (void)os_wait_wake_one(&queue->send_wait_head,
                                   &queue->send_wait_tail, OS_OK);
            os_schedule();

            os_exit_critical(primask);
            return OS_OK;
        }

        if (timeout == OS_NO_WAIT)
        {
            os_exit_critical(primask);
            return OS_ERR_EMPTY;
        }

        if ((timeout != OS_WAIT_FOREVER) &&
            ((int32_t)(os_tick_get() - deadline) >= 0))
        {
            os_exit_critical(primask);
            return OS_ERR_TIMEOUT;
        }

        os_block_current(&queue->recv_wait_head, &queue->recv_wait_tail,
                         queue, OS_WAIT_QUEUE_RECV,
                         os_remaining(deadline, timeout));
        os_exit_critical(primask);

        {
            int8_t result = os_current_tcb->wait_result;

            if (result != (int8_t)OS_OK)
            {
                return (os_status_t)result;
            }
        }
    }
}

/*
 * 功能：在中断上下文中向队列发送消息。
 * 说明：不阻塞；有空间则入队并唤醒接收者，无空间立即返回 OS_ERR_FULL。
 * 返回：OS_OK 成功；OS_ERR_PARAM 参数非法；OS_ERR_FULL 队列已满。
 */
os_status_t os_queue_send_isr(os_queue_t *queue, void *msg)
{
    uint32_t primask;

    if ((queue == NULL) || (queue->buffer == NULL))
    {
        return OS_ERR_PARAM;
    }

    primask = os_enter_critical();

    if (queue->count >= queue->capacity)
    {
        os_exit_critical(primask);
        return OS_ERR_FULL;
    }

    queue->buffer[(queue->head + queue->count) % queue->capacity] = msg;
    queue->count++;

    (void)os_wait_wake_one(&queue->recv_wait_head, &queue->recv_wait_tail,
                           OS_OK);
    os_schedule();

    os_exit_critical(primask);
    return OS_OK;
}

/*
 * 功能：在中断上下文中从队列接收消息。
 * 说明：不阻塞；有消息则出队并唤醒发送者，为空立即返回 OS_ERR_EMPTY。
 * 返回：OS_OK 成功；OS_ERR_PARAM 参数非法；OS_ERR_EMPTY 队列为空。
 */
os_status_t os_queue_recv_isr(os_queue_t *queue, void **msg)
{
    uint32_t primask;

    if ((queue == NULL) || (queue->buffer == NULL) || (msg == NULL))
    {
        return OS_ERR_PARAM;
    }

    primask = os_enter_critical();

    if (queue->count == 0U)
    {
        os_exit_critical(primask);
        return OS_ERR_EMPTY;
    }

    *msg = queue->buffer[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count--;

    (void)os_wait_wake_one(&queue->send_wait_head, &queue->send_wait_tail,
                           OS_OK);
    os_schedule();

    os_exit_critical(primask);
    return OS_OK;
}

/*
 * 功能：返回队列中当前消息数量。
 * 说明：用于调试/监控；指针为空时返回 0。
 */
uint32_t os_queue_count(os_queue_t *queue)
{
    return (queue == NULL) ? 0U : queue->count;
}
