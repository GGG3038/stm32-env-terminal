/**
  ******************************************************************************
  * @file    os_internal.h
  * @brief   RTOS 内部数据结构与内核函数声明（仅供内核源码使用）
  ******************************************************************************
  */
#ifndef __OS_INTERNAL_H
#define __OS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "os.h"
#include "os_port.h"

/*---------------------------------------------------------------------------
 * 等待类型，用于超时/挂起时判断是否需要做优先级继承回退
 *--------------------------------------------------------------------------*/
#define OS_WAIT_NONE            0U
#define OS_WAIT_SEM             1U
#define OS_WAIT_MUTEX           2U
#define OS_WAIT_QUEUE_SEND      3U
#define OS_WAIT_QUEUE_RECV      4U

/*---------------------------------------------------------------------------
 * 任务控制块
 *
 * 注意：sp 必须是第一个成员，PendSV 汇编依赖该布局。
 *--------------------------------------------------------------------------*/
struct os_tcb
{
    uint32_t *sp;                   /* 当前栈指针（保存的上下文），偏移必须为 0 */

    const char *name;
    os_task_entry_t entry;
    void *arg;
    uint32_t *stack_base;           /* 栈底（低地址） */
    uint32_t stack_words;           /* 栈大小，word */

    uint8_t base_prio;              /* 创建时优先级 */
    uint8_t eff_prio;               /* 有效优先级（含继承） */
    volatile uint8_t state;         /* os_task_state_t */
    uint8_t suspend_count;          /* 嵌套挂起计数 */
    uint8_t has_timeout;            /* 当前等待是否带超时 */
    uint8_t wait_type;              /* OS_WAIT_xxx */
    volatile int8_t wait_result;    /* 等待结果，os_status_t */

    uint32_t wake_deadline;         /* 绝对超时节拍 */

    void *wait_obj;                 /* 当前等待的内核对象 */
    os_task_t **wait_head;          /* 所在等待链表头指针的地址 */
    os_task_t **wait_tail;          /* 所在等待链表尾指针的地址 */

    struct os_tcb *ready_next;      /* 就绪链表节点（环形双向） */
    struct os_tcb *ready_prev;
    struct os_tcb *wait_next;       /* 等待链表节点（双向） */
    struct os_tcb *wait_prev;

    struct os_mutex *owned_mutexes; /* 持有的互斥锁链表 */
};

/*---------------------------------------------------------------------------
 * 内核全局量
 *--------------------------------------------------------------------------*/
extern os_task_t * volatile os_current_tcb;
extern volatile uint32_t os_tick;
extern volatile uint8_t os_started;
extern volatile uint32_t os_assert_line;

/*---------------------------------------------------------------------------
 * 内部函数
 *--------------------------------------------------------------------------*/
void os_assert_fail(const char *file, int line);

void os_schedule(void);
void os_block_current(os_task_t **head, os_task_t **tail,
                      void *obj, uint8_t wait_type, uint32_t timeout);
void os_wait_unlink(os_task_t *task, os_status_t result);
os_task_t *os_wait_wake_one(os_task_t **head, os_task_t **tail,
                            os_status_t result);
void os_task_set_eff_prio(os_task_t *task, uint8_t prio);
void os_task_recompute_prio(os_task_t *task);
void os_task_exit(void) __attribute__((noreturn));

/* 由 os_ipc.c 提供，供内核在任务删除/超时时回调 */
void os_mutex_waiter_removed(os_mutex_t *mutex);
void os_mutex_release_all(os_task_t *task);

/*---------------------------------------------------------------------------
 * 断言与静态断言
 *--------------------------------------------------------------------------*/
#if (OS_CFG_USE_ASSERT != 0U)
#define OS_ASSERT(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            os_assert_fail(__FILE__, __LINE__);                             \
        }                                                                   \
    } while (0)
#else
#define OS_ASSERT(cond)     do { } while (0)
#endif

#define OS_SA_CAT_(a, b)    a##b
#define OS_SA_CAT(a, b)     OS_SA_CAT_(a, b)
#define OS_STATIC_ASSERT(cond)                                              \
    typedef char OS_SA_CAT(os_static_assert_, __LINE__)[(cond) ? 1 : -1]

/* TCB 首成员必须是 sp，PendSV 汇编按 [tcb] 取栈指针 */
OS_STATIC_ASSERT(offsetof(struct os_tcb, sp) == 0U);

#endif /* __OS_INTERNAL_H */
