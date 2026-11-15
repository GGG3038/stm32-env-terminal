/**
  ******************************************************************************
  * @file    os.h
  * @brief   轻量级 RTOS 公共接口
  *
  * 特性：
  *   - 基于优先级的抢占式调度，32 级优先级
  *   - 同优先级时间片轮转
  *   - 任务创建/删除/挂起/恢复/延时/让出
  *   - 计数信号量、互斥锁（优先级继承）、定长消息队列
  *   - 全静态内存，无 malloc
  *
  * 使用步骤：
  *   1. HAL_Init() / SystemClock_Config() / 外设初始化
  *   2. os_init()
  *   3. os_task_create() 创建任务（栈由用户提供）
  *   4. os_start()  永不返回
  *
  * 中断规则：
  *   - 阻塞类 API（wait/lock/recv/send/delay）只能在任务上下文调用
  *   - 中断中只能调用带 _isr 后缀的 API
  ******************************************************************************
  */
#ifndef __OS_H
#define __OS_H

#include <stdint.h>
#include <stddef.h>
#include "os_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OS_VERSION_MAJOR    1U
#define OS_VERSION_MINOR    0U
#define OS_VERSION_PATCH    0U

/* 超时常量 */
#define OS_WAIT_FOREVER     0xFFFFFFFFU
#define OS_NO_WAIT          0U

/* 返回值 */
typedef enum
{
    OS_OK = 0,
    OS_ERR = -1,            /* 通用错误 */
    OS_ERR_PARAM = -2,      /* 参数非法 */
    OS_ERR_TIMEOUT = -3,    /* 超时 */
    OS_ERR_FULL = -4,       /* 信号量/队列已满 */
    OS_ERR_EMPTY = -5,      /* 队列为空 */
    OS_ERR_CONTEXT = -6,    /* 上下文错误（如在中断中调用阻塞 API） */
    OS_ERR_STATE = -7,      /* 对象状态错误 */
    OS_ERR_NOMEM = -8,      /* TCB 用尽 */
    OS_ERR_ABORT = -9       /* 等待被挂起/删除等操作中止 */
} os_status_t;

typedef struct os_tcb os_task_t;
typedef void (*os_task_entry_t)(void *arg);

/* 任务状态，仅用于查询 */
typedef enum
{
    OS_TASK_UNUSED = 0,
    OS_TASK_READY,
    OS_TASK_RUNNING,
    OS_TASK_BLOCKED,
    OS_TASK_SUSPENDED,
    OS_TASK_DELETED
} os_task_state_t;

typedef struct
{
    const char *name;
    os_task_state_t state;
    uint8_t base_prio;       /* 创建时指定的优先级 */
    uint8_t eff_prio;        /* 含优先级继承后的有效优先级 */
    uint32_t stack_words;    /* 栈总大小，单位 word */
    uint32_t stack_used_words; /* 历史最大使用量，单位 word */
} os_task_info_t;

/*----------------------------------------------------------------------------
 * 内核
 *---------------------------------------------------------------------------*/
os_status_t os_init(void);
void os_start(void);                     /* 启动调度器，永不返回 */
uint32_t os_tick_get(void);              /* 当前系统节拍数 */
void os_tick_handler(void);              /* 供 SysTick 中断调用 */
uint32_t os_enter_critical(void);        /* 返回进入前的 PRIMASK */
void os_exit_critical(uint32_t state);

/* 弱定义，用户可重写。默认执行 __WFI() */
void os_idle_hook(void);

/*----------------------------------------------------------------------------
 * 任务
 *---------------------------------------------------------------------------*/
os_status_t os_task_create(os_task_t **task,
                           const char *name,
                           os_task_entry_t entry,
                           void *arg,
                           uint32_t *stack,
                           uint32_t stack_words,
                           uint8_t prio);
os_status_t os_task_delete(os_task_t *task);     /* NULL 表示删除自己 */
os_status_t os_task_suspend(os_task_t *task);    /* NULL 表示挂起自己 */
os_status_t os_task_resume(os_task_t *task);
void os_task_yield(void);
void os_task_delay(uint32_t ticks);
os_status_t os_task_set_priority(os_task_t *task, uint8_t prio);
os_task_t *os_task_self(void);
os_status_t os_task_get_info(os_task_t *task, os_task_info_t *info);
uint32_t os_task_stack_used(os_task_t *task);    /* 返回使用过的 word 数 */
const char *os_task_name(os_task_t *task);

/*----------------------------------------------------------------------------
 * 计数信号量
 *---------------------------------------------------------------------------*/
typedef struct os_sem
{
    volatile uint32_t count;
    uint32_t max_count;
    os_task_t *wait_head;
    os_task_t *wait_tail;
} os_sem_t;

os_status_t os_sem_init(os_sem_t *sem, uint32_t initial, uint32_t max_count);
os_status_t os_sem_wait(os_sem_t *sem, uint32_t timeout);
os_status_t os_sem_post(os_sem_t *sem);
os_status_t os_sem_post_isr(os_sem_t *sem);
uint32_t os_sem_get_count(os_sem_t *sem);

/*----------------------------------------------------------------------------
 * 互斥锁（带优先级继承，不可递归，不可在中断中使用）
 *---------------------------------------------------------------------------*/
typedef struct os_mutex
{
    os_task_t *owner;
    uint16_t nest_count;
    struct os_mutex *owner_next; /* 任务持有的互斥锁链表 */
    os_task_t *wait_head;
    os_task_t *wait_tail;
} os_mutex_t;

os_status_t os_mutex_init(os_mutex_t *mutex);
os_status_t os_mutex_lock(os_mutex_t *mutex, uint32_t timeout);
os_status_t os_mutex_unlock(os_mutex_t *mutex);
os_task_t *os_mutex_get_owner(os_mutex_t *mutex);

/*----------------------------------------------------------------------------
 * 定长消息队列（消息为 void*，即 4 字节）
 *---------------------------------------------------------------------------*/
typedef struct os_queue
{
    void **buffer;
    uint32_t capacity;
    volatile uint32_t head;
    volatile uint32_t count;
    os_task_t *send_wait_head;
    os_task_t *send_wait_tail;
    os_task_t *recv_wait_head;
    os_task_t *recv_wait_tail;
} os_queue_t;

os_status_t os_queue_init(os_queue_t *queue, void **buffer, uint32_t capacity);
os_status_t os_queue_send(os_queue_t *queue, void *msg, uint32_t timeout);
os_status_t os_queue_recv(os_queue_t *queue, void **msg, uint32_t timeout);
os_status_t os_queue_send_isr(os_queue_t *queue, void *msg);
os_status_t os_queue_recv_isr(os_queue_t *queue, void **msg);
uint32_t os_queue_count(os_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif /* __OS_H */
