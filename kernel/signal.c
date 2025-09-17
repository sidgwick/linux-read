/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <asm/segment.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#include <errno.h>
#include <signal.h>

/**
 * @brief 获取当前任务信号屏蔽位图(也叫屏蔽码或阻塞码)
 *
 * sgetmask always successfully returns the signal mask
 * 名字 sgetmask 是 signal-get-mask 的意思
 *
 * @return int returns the signal mask of the calling process
 */
int sys_sgetmask()
{
    return current->blocked;
}

/**
 * @brief 设置新的信号屏蔽位图
 *
 * NOTICE: 信号 SIGKILL 和 SIGSTOP 不能被屏蔽
 *
 * sets the signal mask of the calling process to the value given in newmask.
 * The previous signal mask is returned
 *
 * @param newmask
 * @return int 原信号屏蔽位图
 */
int sys_ssetmask(int newmask)
{
    int old = current->blocked;

    current->blocked = newmask & ~(1 << (SIGKILL - 1)) & ~(1 << (SIGSTOP - 1));
    return old;
}

/**
 * @brief sigpending 系统调用的实现
 *
 * 此函数查询进程有哪些信号正在等待处理
 *
 * sigpending() returns the set of signals that are pending for delivery to the calling
 * thread (i.e., the signals which have been raised while blocked). The mask of pending
 * signals is returned in set.
 *
 * return 0 on success, On failure, -1 is returned and errno is set to indicate the error
 *
 * @param set 此参数接受待处理信号结果掩码
 * @return int 成功返回 0, 失败返回 -1, 并设置错误码
 */
int sys_sigpending(sigset_t *set)
{
    /* fill in "set" with signals pending but blocked.
     * 用还未处理并且被阻塞信号的位图填入 set 指针所指位置处 */

    /* 首先验证进程提供的用户存储空间应有4个字节 */
    verify_area(set, 4);

    /* 然后把还未处理并且被阻塞信号的位图填入 set 指针所指位置处 */
    put_fs_long(current->blocked & current->signal, (unsigned long *)set);

    return 0;
}

/**
 * @brief 挂起进程并等待信号到来
 *
 * atomically swap in the new signal mask, and wait for a signal.
 *
 * we need to play some games with syscall restarting.  We get help
 * from the syscall library interface.  Note that we need to coordinate
 * the calling convention with the libc routine.
 *
 * "set" is just the sigmask as described in 1003.1-1988, 3.3.7.
 *     It is assumed that sigset_t can be passed as a 32 bit quantity.
 *
 * "restart" holds a restart indication.  If it's non-zero, then we
 *     install the old mask, and return normally.  If it's zero, we store
 *     the current mask in old_mask and block until a signal comes in.
 *
 * ----------- 上面是该函数的原始评论 -----------
 *
 * 在最新的 linux 系统上, 此函数签名已经变成 `int sigsuspend(const sigset_t *mask)`, 手册:
 *
 * sigsuspend 临时将信号掩码设置为为 mask 参数给定的掩码, 然后挂起进程, 直到有能发起信号处理函数
 * 的信号出现或者进程终止信号出现
 *
 * > DESCRIPTION
 * >  sigsuspend() temporarily replaces the signal mask of the calling thread with the mask
 * >  given by mask and then suspends the thread until delivery of a signal whose action is
 * >  to invoke a signal handler or to terminate a process.
 * >
 * >  If the signal terminates the process, then sigsuspend() does not return. If the signal
 * >  is caught, then sigsuspend() returns after the signal handler returns, and the signal mask
 * >  is restored to the state before the call to sigsuspend().
 * >
 * >  It is not possible to block SIGKILL or SIGSTOP; specifying these signals in mask, has no
 * >  effect on the thread's signal mask.
 * >
 * > RETURN VALUE
 * >  sigsuspend() always returns -1, with errno set to indicate the error (normally, EINTR).
 *
 * @param restart
 * @param old_mask
 * @param set
 * @return int
 */
int sys_sigsuspend(int restart, unsigned long old_mask, unsigned long set)
{
    extern int sys_pause(void);

    /* 如果 restart 标志不为 0, 表示是从下面的 sys_pause 调用中被信号打断解除阻塞后
     * 重试的 sys_sigsuspend 系统调用, 之前我们已经在 old_mask 里面保存了原始的屏蔽码,
     * 恢复之后返回 EINTR 即可完成 sys_sigsuspend 的预期效果 */
    if (restart) {
        /* we're restarting */
        current->blocked = old_mask;
        return -EINTR;
    }

    /* restart 标志的值是 0, 表示第 1 次调用 sys_sigsuspend 函数, 为了重试调用能正确
     * 进上面的 if 块, 这里先更新栈上的变量.
     *
     * 首先设置 restart 标志(置为 1), 保存进程当前阻塞码 blocked 到 old_mask 中, 并把
     * 进程的阻塞码替换成 set. 然后调用 pause 让进程睡眠, 等待信号的到来.
     *
     * 当进程收到一个信号时, pause 就会返回, 并且进程会去执行信号处理函数, 然后本调用返回
     * -ERESTARTNOINTR 码退出, 这个返回码要求 do_signal 在处理完信号后重入本系统调用再
     * 次执行, 到那时, 上面的 if 块恢复原始的屏蔽码, 然后从系统调用返回 */

    /* we're not restarting.  do the work */
    *(&restart) = 1;
    *(&old_mask) = current->blocked;
    current->blocked = set;

    /* pause 系统调用将导致调用它的进程进入 TASK_INTERRUPTIBLE 状态, 此状态可以被收到信号
     * 唤醒, 该信号或者会终止进程的执行, 或者导致进程去执行相应的信号捕获函数
     *
     * TODO-DONE: ret_from_sys_call/do_signal 还必须要检查信号是不是有信号处理函数?
     * 答: 不是上面的理解. 这里说的执行相应的信号捕获函数, 也包括默认的 IGNORE 处理, 实际上
     *     只要发生了 (signal & ~blocked) 里面出现的信号, 就会使得进程从睡眠状态唤醒*/
    (void)sys_pause(); /* return after a signal arrives */

    /* ERESTARTNOINTR 错误码只有这里使用
     * 此码会强制 do_signal 函数安排 sys_sigsuspend 的重新执行 */

    return -ERESTARTNOINTR; /* handle the signal, and come back */
}

/* 复制 sigaction 数据到 fs 数据段 to 处
 * 即从内核空间复制到用户(任务)数据段中
 * 首先验证 to 处的内存空间是否足够大, 然后把一个 sigaction 结构信息复制到 fs
 * 段(用户) 空间中. 宏函数 put_fs_byte 在 include/asm/segment.h 中实现 */
static inline void save_old(char *from, char *to)
{
    int i;

    verify_area(to, sizeof(struct sigaction));
    for (i = 0; i < sizeof(struct sigaction); i++) {
        put_fs_byte(*from, to);
        from++;
        to++;
    }
}

/* 把 sigaction 数据从 fs 数据段 from 位置复制到 to 处
 * 即从用户数据空间取到内核数据段中 */
static inline void get_new(char *from, char *to)
{
    int i;

    for (i = 0; i < sizeof(struct sigaction); i++)
        *(to++) = get_fs_byte(from++);
}

/* signal 系统调用
 * 类似于 sigaction, 为指定的信号安装新的信号句柄(信号处理程序)
 * 信号句柄可以是用户指定的函数, 也可以是默认句柄(SIG_DFL)或忽略(SIG_IGN)
 * 参数:
 *    - signum: 指定的信号
 *    - handler: 指定的句柄
 *    - restorer: 恢复函数指针, 该函数由 Libc 库提供, 用于在信号处理程序结束
 *                后恢复系统调用返回时几个寄存器的原有值以及系统调用的返回值,
 * 就好 象系统调用没有执行过信号处理程序而直接返回到用户程序一样.
 * 函数返回原信号句柄 */
int sys_signal(int signum, long handler, long restorer)
{
    struct sigaction tmp;

    /* 首先验证信号值在有效范围(1-32)内, 并且不得是信号 SIGKILL 或者 SIGSTOP,
     * 因为这 两个信号不能被进程捕获 */
    if (signum < 1 || signum > 32 || signum == SIGKILL || signum == SIGSTOP)
        return -EINVAL;

    // 然后根据提供的参数组建 sigaction 结构内容
    // sa_handler 是指定的信号处理句柄
    // sa_mask 是执行信号处理句柄时的信号屏蔽码
    // sa_flags 是执行时的一些标志组合, 这里设定该信号处理句柄只使用 1 次后就恢
    //          复到默认值, 并允许信号在自己的处理句柄中收到
    tmp.sa_handler = (void (*)(int))handler;
    tmp.sa_mask = 0;
    tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
    tmp.sa_restorer = (void (*)(void))restorer;
    handler = (long)current->sigaction[signum - 1].sa_handler; /* 原句柄 */
    current->sigaction[signum - 1] = tmp;                      /* 新句柄 */
    return handler;
}

/* sigaction 系统调用, 用于改变进程在收到一个信号时的操作
 *
 * 参数:
 *     - signum 可以是除了 SIGKILL, SIGSTOP 以外的任何信号
 *     - action 新操作, 不为空则新操作被安装
 *     - oldaction: 如此指针不为空, 则原操作被保留到 oldaction
 * 成功则返回 0, 否则为 -EINVAL */
int sys_sigaction(int signum, const struct sigaction *action, struct sigaction *oldaction)
{
    struct sigaction tmp;

    if (signum < 1 || signum > 32 || signum == SIGKILL || signum == SIGSTOP)
        return -EINVAL;

    tmp = current->sigaction[signum - 1];
    get_new((char *)action, (char *)(signum - 1 + current->sigaction));
    if (oldaction)
        save_old((char *)&tmp, (char *)oldaction);

    if (current->sigaction[signum - 1].sa_flags & SA_NOMASK)
        current->sigaction[signum - 1].sa_mask = 0;
    else
        current->sigaction[signum - 1].sa_mask |= (1 << (signum - 1));

    return 0;
}

/* Routine writes a core dump image in the current directory.
 * Currently not implemented. */
int core_dump(long signr)
{
    return (0); /* We didn't do a dump */
}

/**
 * @brief 信号处理
 *
 * 系统调用的中断处理程序中真正的信号预处理程序(在 kernel/sys_call.s)
 *
 * 这段代码的主要作用是将信号处理句柄插入到用户程序堆栈中, 并在本系统调用结束返回
 * 后立刻执行信号句柄程序, 然后继续执行用户的程序.
 *
 * 调用到本函数的时候, 栈的状态如下:
 * S = (EIP, signr, EAX, EBX, ECX, EDX, ORIGI_EAX, FS, ES, DS, EIP, CS, EFLAGS, ESP, SS, ...)
 *      ^    ^      ^                                          ^
 *      |    |      sys_call                                   int0x80
 *      |    ret_from_sys_call(注意这里不是 call 调用)
 *      call(do_signal)
 *
 * TODO-DONE: 注意 esp 这个参数类型
 * 答: 不管是申明为 `long` 还是 `long *`, 它都还是指向栈上的那个位置. 无非是在 do_signal 里面对它的解释不同,
 *     在函数里我们在计算自定义 handler 参数逻辑的时候, 确实要把 esp 当做一个指针类型来使用, 这里申明为指针类型
 *     可以方便在里面直接使用
 *
 * @param signr 信号编号
 * @param eax 系统调用返回值
 * @param ebx
 * @param ecx
 * @param edx
 * @param orig_eax
 * @param fs
 * @param es
 * @param ds
 * @param eip
 * @param cs
 * @param eflags
 * @param esp
 * @param ss
 * @return int
 */
int do_signal(long signr, long eax, long ebx, long ecx, long edx, long orig_eax, long fs, long es,
              long ds, long eip, long cs, long eflags, unsigned long *esp, long ss)
{
    unsigned long sa_handler;
    long old_eip = eip; /* 是进程用户代码下一条指令地址 */
    struct sigaction *sa = current->sigaction + signr - 1;
    int longs;

    unsigned long *tmp_esp;

#ifdef notdef
    printk("pid: %d, signr: %x, eax=%d, oeax = %d, int=%d\n", current->pid, signr, eax, orig_eax,
           sa->sa_flags & SA_INTERRUPT);
#endif
    /* origin_eax != -1 表示, 是因为某个系统调用软中断(int 0x80)到这里来的(其他不是软中断的情况都是 -1,
     * 参考 sys_call.s)
     *
     * eax 里面保存了 sys_call 的返回值, 如果 sys_call 返回了 ERESTARTSYS, 这是 sys_call 实现和系统的约定,
     * 此时需要重新发起 sys_call
     *
     * TODO-DONE: 看一下这两个返回 ERESTARTSYS 的系统调用
     * 答: 如果系统调用以 TASK_INTERRUPTIBLE 状态阻塞, 那么调度器在收到发给进程的信号之后, 就会从睡眠状态唤醒进程,
     *     此时系统调用未完成, 就可以返回 ERESTARTSYS.
     *
     *     具体的例子:
     *     在 kernel/exit.c 的 waitpid 函数中, 如果收到了 SIGCHLD 信号, 或者在读管道函数 fs/pipe.c 中管道当前
     *     读数据但没有读到任何数据等情况下, 进程收到了任何一个非阻塞的信号, 则都会以 -ERESTARTSYS 返回值返回.
     *     它表示进程可以被信号中断, 但是在信号处理完成后会重新执行被中断的系统调用 */
    if ((orig_eax != -1) && ((eax == -ERESTARTSYS) || (eax == -ERESTARTNOINTR))) {
        /* SIGCONT-SIGTTOU 这个范围内的信号有 SIGCONT/SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU
         * 这些信号都是用于作业控制的, 它们与终端和进程组管理密切相关
         *
         * 允许被这些信号打断的系统调用重新执行的背景有:
         *  - 在早期的 Unix 系统中, 作业控制信号(如 Ctrl+Z)会中断前台进程
         *  - 当用户将进程放到后台再恢复时(SIGCONT), 期望进程能继续正常工作
         *  - 如果系统调用因为作业控制信号而失败, 用户体验会很差
         *
         * 对于其他信号(如 SIGINT/SIGTERM/SIGUSR1 等), 不予重启相关系统调用的原因有:
         *  - 明确的用户意图: 这些信号通常表示用户明确希望中断或终止程序
         *  - 编程语义清晰: 程序员发送这些信号时, 通常期望程序立即响应, 而不是继续执行被中断的操作
         *  - 避免意外行为: 如果 SIGINT 这样的信号也能重启系统调用, 用户会感觉程序 "无法中断" */
        if ((eax == -ERESTARTSYS) &&
            ((sa->sa_flags & SA_INTERRUPT) || signr < SIGCONT || signr > SIGTTOU)) {
            *(&eax) = -EINTR;
        } else {
            /* 如果决定重新发起系统调用, 需要修正 eax 为 int0x80 之前的值,
             * 并且把 eip 回拨 2 字节, 使 eip 指向 int0x80 指令 */
            *(&eax) = orig_eax;
            *(&eip) = old_eip -= 2;
        }
    }

    sa_handler = (unsigned long)sa->sa_handler;
    if (sa_handler == 1) { /* 忽略处理 SIG_IGN = 1 */
        return (1);        /* Ignore, see if there are more signals... */
    }

    /* 没有配置自定义 handler 的话, 使用默认处理(SIG_DFL = 0) */
    if (!sa_handler) {
        switch (signr) {
        case SIGCONT:
        case SIGCHLD:
            return (1); /* Ignore, ... */

        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            current->state = TASK_STOPPED;
            current->exit_code = signr;
            if (!(current->p_pptr->sigaction[SIGCHLD - 1].sa_flags & SA_NOCLDSTOP)) {
                current->p_pptr->signal |= (1 << (SIGCHLD - 1));
            }
            return (1); /* Reschedule another event */

        case SIGQUIT:
        case SIGILL:
        case SIGTRAP:
        case SIGIOT:
        case SIGFPE:
        case SIGSEGV:
            if (core_dump(signr)) {
                /* TODO: 0x80 和 WIFSTOPPED 等 waitpid 函数返回的状态字有关系, 可以仔细研究下 */
                do_exit(signr | 0x80); /* fall through */
            }
        default:
            do_exit(signr);
        }
    }

    /* 走到这里的, 需要做自定义 handler 调用处理
     * OK, we're invoking a handler */
    if (sa->sa_flags & SA_ONESHOT) {
        sa->sa_handler = NULL;
    }

    /* 下面操作破坏了原始 EIP/ESP
     * 但是这个破坏是有意(也有益)的, 如此操作可以让系统调用软中断 iret 的时候,
     * 跳转到 sa_handler 去执行, iret 之后对 0 特权级的栈没有任何影响 */

    /* 将内核态栈上用户调用系统调用下一条代码指令指针 eip 指向该信号处理句柄
     * 之后从 int0x80 iret 的时候, 就会到对应的信号处理句柄去执行 */
    *(&eip) = sa_handler;

    // 将原调用程序的用户堆栈指针向下扩展 7(或者 8) 个长字(用来存放调用信号句柄的参数等)
    // 并检查内存使用情况(例如如果内存超界则分配新页等)
    longs = (sa->sa_flags & SA_NOMASK) ? 7 : 8;
    *(&esp) -= longs;
    verify_area(esp, longs * 4);

    /* do_signal 函数能被调用的前提, 简单理解是发起了系统调用(CPL 从 3 变为 0)
     * 因此要注意下面的操作, 实际上实在操作 CPL=3 的栈
     *
     * 下面在维护适用于 handler 运行的栈, 执行完成之后, 栈的样子变为:
     *
     * S = (sa_restorer, signr, *blocked, eax, ecx, edx, eflags, eip, ...int0x80 前的栈内容...)
     *      ^            ^                                       ^
     *      |            |                                       视作 call(sa_restorer) 时候入栈的 EIP
     *      |            handler 参数
     *      把他视作 call(handler) 时候入栈的 EIP
     *
     * 说明:
     *  - 如果 SA_NOMASK 置位才有 blocked
     *  - 根据要不要重新发起系统调用, eip 里面保存的值是 int0x80 或者 int0x80 下一条指令地址
     *
     * 当程序从 iret 退回, 就会去运行 handler, 此时 handler 函数大概是这样的:
     *
     * ```asm
     * sig_handler:
     *     push   %ebp
     *     mov    %esp,%ebp
     *       ##### 处理 logic
     *     leave
     *     ret
     * ```
     *
     * 能看到, sig_handler 就是一个正常的 C 函数, 但是仔细观察上面的栈, 就能发现从 sig_handler 返回的时候,
     * 是跳转到了 sa_restorer 执行(glibc 提供此函数), 在这个函数里面会负责打扫栈中的 signr, __blocked__,
     * eax, ecx, edx, eflags 这部分数据.
     *
     * 下面列出来 glibc 实现的 sa_restorer:
     *
     * ```asm
     * ___sig_restore:
     *     addl $4 ,%esp # signr
     *     popl %eax
     *     popl %ecx
     *     popl %edx
     *     popfl
     *     ret
     *
     * ___masksig_restore:
     *     addl $4, %esp      # signr
     *     call ____ssetmask  # old blocking
     *     addl $4, %esp
     *     popl %eax
     *     popl %ecx
     *     popl %edx
     *     popfl
     *     ret
     * ```
     *
     * 顺便把设置 sigaction 的逻辑也贴出来(glibc):
     *
     * ```c
     * int sigaction(int sig, struct sigaction *sa, struct sigaction *old)
     * {
     *     if (sa->sa_flags & SA_NOMASK)
     *         sa->sa_restorer = ___sig_restore;
     *     else
     *         sa->sa_restorer = ___masksig_restore;
     *
     *     __asm__("int $0x80"
     *             : "=a"(sig)
     *             : "0"(__NR_sigaction), "b"(sig), "c"(sa), "d"(old));
     *
     *     if (sig >= 0)
     *         return 0;
     *
     *     errno = -sig;
     *     return -1;
     * }
     *
     * sigset_t ___ssetmask(sigset_t mask)
     * {
     *     long res;
     *     __asm__("int $0x80" : "=a"(res)
     *             : "0"(__NR_ssetmask), "b"(mask));
     *     return res;
     * }
     * ```
     */

    tmp_esp = esp;
    put_fs_long((long)sa->sa_restorer, tmp_esp++);
    put_fs_long(signr, tmp_esp++);

    /* 如果允许信号自己的处理句柄程序收到信号自己,
     * 则需要将进程的信号阻塞码压入堆栈*/
    if (!(sa->sa_flags & SA_NOMASK))
        put_fs_long(current->blocked, tmp_esp++);

    put_fs_long(eax, tmp_esp++);
    put_fs_long(ecx, tmp_esp++);
    put_fs_long(edx, tmp_esp++);
    put_fs_long(eflags, tmp_esp++);
    put_fs_long(old_eip, tmp_esp++);

    /* 信号处理过程中, 不会继续处理 sa_mask 指定的那部分信号 */
    current->blocked |= sa->sa_mask;

    return (0); /* Continue, execute handler, EAX = 0 */
}
