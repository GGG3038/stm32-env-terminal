/**
  ******************************************************************************
  * @file    os_port.c
  * @brief   Cortex-M3 移植层：SVC / PendSV 上下文切换
  *
  * SVC 优先级 0，PendSV / SysTick 优先级 15。
  * 任务运行在 Thread 模式 + PSP，异常处理运行在 Handler 模式 + MSP。
  ******************************************************************************
  */
#include "os_internal.h"

/*---------------------------------------------------------------------------
 * 临界区：保存并恢复 PRIMASK
 *--------------------------------------------------------------------------*/
/*
 * 功能：进入内核临界区。
 * 说明：保存当前 PRIMASK 并关闭可屏蔽中断；返回旧状态，供
 *       os_exit_critical 精确恢复，因此支持临界区嵌套。
 * 返回：进入前的 PRIMASK 值。
 */
uint32_t os_enter_critical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return primask;
}

/*
 * 功能：退出内核临界区。
 * 说明：恢复 os_enter_critical 保存的 PRIMASK；若进入前中断是开着的则
 *       重新开中断，否则保持关闭，避免嵌套时提前开中断。
 * 参数：state 由 os_enter_critical 返回的旧 PRIMASK。
 */
void os_exit_critical(uint32_t state)
{
    __DMB();
    __set_PRIMASK(state);
}

/*---------------------------------------------------------------------------
 * SVC：启动第一个任务
 *
 * 进入时使用 MSP，LR = 0xFFFFFFF9（Thread/MSP）。
 * 从 TCB 取出首次启动栈帧，恢复 R4-R11，把 PSP 指向硬件栈帧，
 * 打开 SysTick，然后以 0xFFFFFFFD（Thread/PSP）返回，硬件自动弹出
 * R0-R3/R12/LR/PC/xPSR，任务开始运行。
 *
 * SysTick 必须在这里才打开：此时 PSP 已经有效，不会出现 PendSV
 * 在 PSP=0 时切换导致的 HardFault。
 *--------------------------------------------------------------------------*/
__asm void SVC_Handler(void)
{
    extern os_current_tcb;

    PRESERVE8

    ldr     r3, =os_current_tcb     /* r3 = &os_current_tcb              */
    ldr     r1, [r3]                /* r1 = os_current_tcb               */
    ldr     r0, [r1]                /* r0 = tcb->sp (R4-R11 保存区)      */
    ldmia   r0!, {r4-r11}           /* 恢复 R4-R11                       */
    msr     psp, r0                 /* PSP 指向硬件异常栈帧              */
    isb

    ldr     r0, =0xE000E010         /* SYST_CSR                          */
    mov     r1, #7                  /* ENABLE | TICKINT | CLKSOURCE      */
    str     r1, [r0]

    orr     lr, lr, #0x04           /* EXC_RETURN: Thread mode + PSP     */
    bx      lr
}

/*---------------------------------------------------------------------------
 * PendSV：任务上下文切换
 *
 * 1. 硬件已把 R0-R3/R12/LR/PC/xPSR 压到任务栈 (PSP)
 * 2. 软件保存 R4-R11，并把新 PSP 写入 tcb->sp
 * 3. 调用 C 函数 os_pendsv_switch() 选择下一个任务
 * 4. 从新任务 TCB 恢复 R4-R11 和 PSP，异常返回
 *--------------------------------------------------------------------------*/
__asm void PendSV_Handler(void)
{
    extern os_current_tcb;
    extern os_pendsv_switch;

    PRESERVE8

    mrs     r0, psp
    isb
    ldr     r3, =os_current_tcb
    ldr     r2, [r3]                /* r2 = 当前 TCB                     */
    stmdb   r0!, {r4-r11}           /* 保存 R4-R11                       */
    str     r0, [r2]                /* tcb->sp = r0                      */

    push    {r3, lr}                /* 8 字节，保持 MSP 8 字节对齐       */
    cpsid   i                       /* 切换过程中禁止中断                */
    bl      os_pendsv_switch
    cpsie   i
    pop     {r3, lr}

    ldr     r1, =os_current_tcb
    ldr     r2, [r1]
    ldr     r0, [r2]                /* r0 = 新任务 tcb->sp               */
    ldmia   r0!, {r4-r11}           /* 恢复 R4-R11                       */
    msr     psp, r0                 /* PSP 指向硬件栈帧                  */
    isb
    bx      lr                      /* 异常返回，硬件恢复 R0-R3/PC/xPSR  */
}

/*---------------------------------------------------------------------------
 * 启动第一个任务
 *--------------------------------------------------------------------------*/
/*
 * 功能：触发 SVC 异常，启动第一个任务。
 * 说明：执行 svc 0 后由 SVC_Handler 恢复首个任务的上下文并打开 SysTick；
 *       本函数执行后不再返回，后面的 nop/bx lr 仅为汇编占位。
 */
__asm void os_port_start_first_task(void)
{
    PRESERVE8

    svc     0
    nop
    nop
    bx      lr
}
