/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *     0(%esp) - %eax
 *     4(%esp) - %ebx
 *     8(%esp) - %ecx
 *     C(%esp) - %edx
 *    10(%esp) - original %eax    (-1 if not system call)
 *    14(%esp) - %fs
 *    18(%esp) - %es
 *    1C(%esp) - %ds
 *    20(%esp) - %eip
 *    24(%esp) - %cs
 *    28(%esp) - %eflags
 *    2C(%esp) - %oldesp
 *    30(%esp) - %oldss
 *
 * stack = (EAX, EBX, ECX, EDX, -1/EAX, FS, ES, DS, EIP, CS, EFLAGS, ESP*, SS*, ...)
 *
 * system_call.s 文件包含系统调用(system-call)底层处理子程序
 * 由于有些代码比较类似, 所以同时也包括时钟中断处理(timer-interrupt)句柄.
 * 硬盘和软盘的中断处理程序也在这里
 *
 * 注意: 这段代码处理信号(signal)识别, 在每次时钟中断和系统调用之后都会进行识别. 一般
 * 中断过程并不处理信号识别, 因为会给系统造成混乱
 *
 * 上面 Linus 原注释中一般中断过程是指除了系统调用中断(int 0x80)和
 * 时钟中断(int 0x20) 以外的其他中断. 这些中断会在内核态或用户态随机发生,
 * 若在这些中断过程中也处理信号识别的话, 就有可能与系统调用中断和时钟中断
 * 过程中对信号的识别处理过程相冲突, 违反了内核代码非抢占原则. 因此系统既无
 * 必要在这些 '其他' 中断中处理信号, 也不允许这样做 */

SIG_CHLD    = 17 # 定义 SIG_CHLD 信号(子进程停止或结束)

# 各寄存器在栈上的偏移位置
EAX        = 0x00
EBX        = 0x04
ECX        = 0x08
EDX        = 0x0C
ORIG_EAX    = 0x10 # 如果不是系统调用(是其它中断)时，该值为 -1
FS        = 0x14
ES        = 0x18
DS        = 0x1C
EIP        = 0x20
CS        = 0x24
EFLAGS        = 0x28
OLDESP        = 0x2C
OLDSS        = 0x30

# 以下这些是任务结构(task_struct)中变量的偏移值
state    = 0         # these are offsets into the task-struct. 进程状态码
counter    = 4         # 任务运行时间计数(递减)(滴答数). 运行时间片
priority = 8        # 运行优先数, 任务开始运行时counter=priority, 越大则运行时间越长
signal    = 12        # 信号位图, 每个比特位代表一种信号, 信号值=位偏移值+1
sigaction = 16        # MUST be 16 (=len of sigaction), sigaction结构长度必须是16字节
blocked = (33*16)   # 受阻塞信号位图的偏移量

# 以下定义在 sigaction 结构中的偏移量
# offsets within sigaction
sa_handler = 0       # 信号处理过程的句柄（描述符）
sa_mask = 4          # 信号屏蔽码
sa_flags = 8         # 信号集
sa_restorer = 12     # 恢复函数指针

nr_system_calls = 82 # Linux 0.12 版内核中的系统调用总数

ENOSYS = 38 # 系统调用号出错码

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl system_call,sys_fork,timer_interrupt,sys_execve
.globl hd_interrupt,floppy_interrupt,parallel_interrupt
.globl device_not_available, coprocessor_error

# 系统调用号错误时将返回出错码 -ENOSYS
.align 2
bad_sys_call:
    pushl $-ENOSYS # EAX = -ENOSYS
    jmp ret_from_sys_call

# 重新执行调度程序入口
# 调度程序 schedule 在(kernel/sched.c), 当调度程序返回时就从 ret_from_sys_call 处继续执行
.align 2
reschedule:
    pushl $ret_from_sys_call
    jmp schedule

# 系统调用入口
.align 2
system_call:
    push %ds
    push %es
    push %fs
    pushl %eax        # save the orig_eax

    # EBX, ECX, EDX 是给系统调用的参数, 这里入栈了
    pushl %edx
    pushl %ecx        # push %ebx,%ecx,%edx as parameters
    pushl %ebx        # to the system call

    movl $0x10,%edx        # set up ds,es to kernel space
    mov %dx,%ds
    mov %dx,%es
    movl $0x17,%edx        # fs points to local data space
    mov %dx,%fs

    cmpl NR_syscalls,%eax # %eax - NR_syscalls, cmp 指令影响 CF
    jae bad_sys_call # jmp above or equal 要求 `CF > 0`, 这说明 %eax 里面的功能号太大了, 处理不了
    call sys_call_table(,%eax,4) # 执行系统调用
    pushl %eax # 系统调用结果
2:
    movl current, %eax  # 当前任务
    cmpl $0, state(%eax) # 当前任务的 state == 0
    jne reschedule # 状态 != 0, 就去调度别的任务
    cmpl $0, counter(%eax) # 时间片用完了, 也去调度其他任务
    je reschedule

    # 继续 ret_from_sys_call

# 以下这段代码执行从系统调用 C 函数返回后, 对信号进行识别处理
# 其他中断服务程序退出时也将跳转到这里进行处理后才退出中断过程
ret_from_sys_call:
    # task0 不处理信号
    movl current, %eax
    cmpl task, %eax    # task[0] cannot have signals
                        # task 数组在 sched.c 文件定义
    je 3f

    # 因为任务在内核态执行时不可抢占, 所以这里通过对原调用程序代码选择符的检查来判断调用程
    # 序是否是用户任务, 如果不是就说明是某个中断服务程序跳过来的, 直接退出中断
    cmpw $0x0f, CS(%esp)        # was old code segment supervisor ?
    jne 3f

    # 原堆栈不在用户段中, 也说明本次系统调用的调用者不是用户任务，退出
    cmpw $0x17, OLDSS(%esp)        # was stack segment = 0x17 ?
    jne 3f

    # BSF(Bit Scan Forward): 位扫描找 1, 低 -> 高
    # BSR(Bit Scan Reverse): 位扫描找 1, 高 -> 低
    #
    # 找到是 1 的位后, 把位置数给参数一并置 ZF=0
    # 找不到1时, 置 ZF=1

    movl signal(%eax), %ebx     # signal = 12, 信号位图
    movl blocked(%eax), %ecx    # blocked = (33*16), 信号屏蔽位图
    notl %ecx           # 屏蔽位图取反
    andl %ebx, %ecx     # 信号位图且没有被屏蔽的
    bsfl %ecx, %ecx     # 位扫描寻找待处理的信号
    je 3f               # 如果没有待处理的信号
    btrl %ecx, %ebx     # Bit Test and Reset, 检测信号集的 ECX 位, 并清零
    movl %ebx, signal(%eax) # 更新信号集
    incl %ecx   # ECX + 1 = SIGNAL_NUMBER
    pushl %ecx
    call do_signal
    popl %ecx

    # 循环, eax & eax != 0 的时候跳转
    #
    # 这里有多个信号的时候, 一次系统调用只处理一个信号
    testl %eax, %eax
    jne 2b        # see if we need to switch tasks, or do more signals

3:    popl %eax
    popl %ebx
    popl %ecx
    popl %edx
    addl $4, %esp    # skip orig_eax
    pop %fs
    pop %es
    pop %ds
    iret

# 这是一个外部的基于硬件的异常
# 当协处理器检测到自己发生错误时, 就会通过 ERROR 引脚通知 CPU
# 下面代码用于处理协处理器发出的出错信号, 并跳转去执行 C 函数 math_error (在 kernel/math/error.c)
# 返回后将跳转到标号 ret_from_sys_call 处继续执行
.align 2
coprocessor_error:
    push %ds
    push %es
    push %fs
    pushl $-1        # fill in -1 for orig_eax
    pushl %edx
    pushl %ecx
    pushl %ebx
    pushl %eax
    movl $0x10,%eax # 10-0-00 内核数据段
    mov %ax,%ds
    mov %ax,%es
    movl $0x17,%eax # 10-1-11 局部数据段
    mov %ax,%fs
    pushl $ret_from_sys_call # 手动给 math_error 设置好返回地址
    jmp math_error

# 如果控制寄存器 CR0 中 EM 标志置位, 则当 CPU 执行一个协处理器指令时就会引发该
# 中断, 这样 CPU 就可以有机会让这个中断处理程序模拟协处理器指令
#
# CR0 的交换标志 TS 是在 CPU 执行任务转换时设置的. TS 可以用来确定什么时候协处理器中的
# 内容与 CPU 正在执行的任务不匹配了, 当 CPU 在运行一个协处理器转义指令时发现 TS 置位时,
# 就会引发该中断. 此时就可以保存前一个任务的协处理器内容, 并恢复新任务的协处理器执行
# 状态(176行). 参见 kernel/sched.c
#
# 该中断最后将转移到标号 ret_from_sys_call 处执行下去(检测并处理信号)
#
# 总结下来会有 2 中情况引发此中断:
#   1 - EM 置位, CPU 执行协处理器指令
#   2 - TS 置位, CPU 执行协处理器指令
.align 2
device_not_available:
    push %ds
    push %es
    push %fs
    pushl $-1        # fill in -1 for orig_eax
    pushl %edx
    pushl %ecx
    pushl %ebx
    pushl %eax

    movl $0x10,%eax
    mov %ax,%ds
    mov %ax,%es
    movl $0x17,%eax
    mov %ax,%fs

    pushl $ret_from_sys_call
    clts                    # clear TS so that we can use math
    movl %cr0, %eax
    testl $0x4, %eax        # .... 0100 ~ EM (math emulation bit)
    je math_state_restore  # 转去保存协处理器状态
    pushl %ebp
    pushl %esi
    pushl %edi
    pushl $0                # temporary storage for ORIG_EIP

    # TODO: 所以这里是如何跳过了后续的协处理器指令?
    call math_emulate      # 软件模拟协处理器指令执行

    addl $4, %esp
    popl %edi
    popl %esi
    popl %ebp
    ret

# int32, 时钟中断处理程序
# 中断频率设置为 100Hz, 定时芯片 8253/8254 是在(kernel/sched.c)处初始化的
# 因此这里 jiffies 每 10 毫秒加 1, 这段代码将 jiffies 增 1, 发送结束中断指令给 8259 控制器
# 然后用当前特权级作为参数调用 C 函数 do_timer(long CPL), 当调用返回时转去检测并处理信号
.align 2
timer_interrupt:
    push %ds        # save ds,es and put kernel data space
    push %es        # into them. %fs is used by system_call
    push %fs
    pushl $-1        # fill in -1 for orig_eax
    pushl %edx        # we save %eax,%ecx,%edx as gcc doesn't
    pushl %ecx        # save those across function calls. %ebx
    pushl %ebx        # is saved as we use that in ret_sys_call
    pushl %eax

    movl $0x10,%eax # ds, es 现在保存的是内核数据段
    mov %ax,%ds
    mov %ax,%es
    movl $0x17,%eax # fs 保存的是任务的数据段
    mov %ax,%fs
    incl jiffies   # 增加系统滴答数

    # 通知 8259A 中断处理完成, 以继续中断
    # 由于初始化中断控制芯片时没有采用自动 EOI, 所以这里需要发指令结束该硬件中断
    movb $0x20,%al  # EOI to interrupt controller #1
    outb %al,$0x20

    # CS = 0x24 = 36
    # stack = (EAX, EBX, ECX, EDX, -1, FS, ES, DS, EIP, CS, EFLAGS, ESP*, SS*, ...)
    movl CS(%esp), %eax
    andl $3, %eax        # (0011 & %eax) is CPL (0 or 3, 0=supervisor)
    pushl %eax
    call do_timer        # 'do_timer(long CPL)' does everything from
    addl $4,%esp        # task switching to accounting ...
    jmp ret_from_sys_call

# 这是 sys_execve 系统调用.
# 取中断调用程序的代码指针作为参数调用 C 函数 do_execve (在 fs/exec.c)
# stack = (EIP0, EBX, ECX, EDX, ORIGI_EAX, FS, ES, DS, EIP, CS, EFLAGS, ESP, SS, ...)
#          ^     ^                                     ^
#          |     sys_call ---------------------------- int0x80
#          sys_execve
# TODO: EIP(%esp) 到底是如何运作的???
.align 2
sys_execve:
    lea EIP(%esp), %eax # eax 指向的是堆栈中保存用户程序 eip 指针处
    pushl %eax
    call do_execve
    addl $4, %esp
    ret

/* fork 系统调用
 * 首先用 find_empty_process 找到最新可用的 pid
 * 然后调用 copy_process 创建出新进程 */
.align 2
sys_fork:
    call find_empty_process    # 准备 pid
    testl %eax,%eax
    js 1f
    push %gs
    pushl %esi
    pushl %edi
    pushl %ebp
    pushl %eax
    call copy_process
    addl $20,%esp
1:    ret

hd_interrupt:
    pushl %eax
    pushl %ecx
    pushl %edx
    push %ds
    push %es
    push %fs
    movl $0x10,%eax
    mov %ax,%ds
    mov %ax,%es
    movl $0x17,%eax
    mov %ax,%fs

    # EOI to interrupt controller #1
    # 由于初始化中断控制芯片时没有采用自动 EOI, 所以这里需要发指令结束该硬件中断
    movb $0x20,%al
    outb %al,$0xA0
    jmp 1f            # give port chance to breathe
1:    jmp 1f

# do_hd 定义为一个函数指针, 将被赋值 read_intr 或 write_intr 函数地址
# 放到 edx 寄存器后就将 do_hd 指针变量置为 NULL. 然后测试得到的函数指针,
# 若该指针为空, 则赋予该指针指向 C 函数 unexpected_hd_interrupt, 以处理未知硬盘中断
# 每次 hd_interrupt 之前都应该重新设置 do_hd
1:    xorl %edx, %edx
    movl %edx, hd_timeout # hd_timeout 置为 0, 表示控制器已在规定时间内产生了中断
    xchgl do_hd, %edx
    testl %edx, %edx
    jne 1f
    movl $unexpected_hd_interrupt, %edx # 默认的中断处理函数
1:    outb %al, $0x20 # EOI
    call *%edx        # "interesting" way of handling intr.
    pop %fs
    pop %es
    pop %ds
    popl %edx
    popl %ecx
    popl %eax
    iret


# 其处理过程与上面对硬盘的处理基本一样(kernel/blk_drv/floppy.c)
# 首先向 8259A 中断控制器主芯片发送 EOI 指令, 然后取变量 do_floppy 中的函数指针放入 eax 寄存器中,
# 并置 do_floppy 为 NULL, 接着判断 eax 函数指针是否为空. 如为空, 则给 eax 赋值指向
# unexpected_floppy_interrupt, 用于显示出错信息. 随后调用 eax 指向的函数:
#  - rw_interrupt,
#  - seek_interrupt
#  - recal_interrupt
#  - reset_interrupt
#  - unexpected_floppy_interrupt
floppy_interrupt:
    pushl %eax
    pushl %ecx
    pushl %edx
    push %ds
    push %es
    push %fs

    movl $0x10,%eax
    mov %ax,%ds
    mov %ax,%es
    movl $0x17,%eax
    mov %ax,%fs

    movb $0x20,%al
    outb %al,$0x20        # EOI to interrupt controller #1

    # do_floppy = NULL
    # 每次 floppy_interrupt 之前都应该重新设置 do_floppy
    xorl %eax, %eax
    xchgl do_floppy, %eax
    testl %eax, %eax
    jne 1f
    movl $unexpected_floppy_interrupt, %eax
1:  call *%eax        # "interesting" way of handling intr.
    pop %fs
    pop %es
    pop %ds
    popl %edx
    popl %ecx
    popl %eax
    iret

# 并行口中断处理程序, 对应硬件中断请求信号 IRQ7
# 本版本内核还未实现. 这里只是发送 EOI 指令
parallel_interrupt:
    pushl %eax

    movb $0x20,%al
    outb %al,$0x20

    popl %eax
    iret
