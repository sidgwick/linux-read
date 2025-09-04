/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

.globl page_fault

/*
 * 中断发生的时候, 根据有没有发生特权级处理器会压栈:
 * 1. 特权级变化 SS, ESP, EFLAGES, CS, IP + 错误码(如有)
 * 2. 特权级不变 EFLAGES, CS, IP + 错误码(如有)
 * 压进去栈顶的错误码, 需要再中断处理程序中, 手动 pop 出去 */

/**
 * page_fault 函数用于处理页面异常中断 */
page_fault:
    xchgl %eax,(%esp) # 栈顶位置有出错码, 顺手还把 %eax 换到栈里面了
    pushl %ecx
    pushl %edx
    push %ds
    push %es
    push %fs
    movl $0x10,%edx # 设置短描述符, 都用 GDT 里面 2# 描述符(4GB 数据段)
    mov %dx,%ds
    mov %dx,%es
    mov %dx,%fs
    movl %cr2,%edx # 具体页异常的线性地址
    pushl %edx # 线性地址入栈
    pushl %eax # 出错码入栈, 里面有具体错误信息, 特别注意最低位是 P 位
    testl $1,%eax
    jne 1f # JNE ~ ZF!=0 ~ (<P=1 & 1> = 1)
    call do_no_page # 处理缺页异常
    jmp 2f
1:  call do_wp_page # 处理非缺页异常
2:    addl $8,%esp # 清理栈上面参数
    pop %fs
    pop %es
    pop %ds
    popl %edx
    popl %ecx
    popl %eax   # 原来栈顶是 error-code, 后来被换成 eax 内容, 因此这里弹出到 eax
                # 这个弹出(错误码位置)必须要做, 做完之后才能 iret
    iret
