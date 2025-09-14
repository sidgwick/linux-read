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

/* 获取当前任务信号屏蔽位图(屏蔽码或阻塞码)
 * sgetmask 可分解为 signal-get-mask */
int sys_sgetmask()
{
    return current->blocked;
}

/* 设置新的信号屏蔽位图
 * 信号 SIGKILL 和 SIGSTOP 不能被屏蔽, 返回值是原信号屏蔽位图 */
int sys_ssetmask(int newmask)
{
    int old = current->blocked;

    current->blocked = newmask & ~(1 << (SIGKILL - 1)) & ~(1 << (SIGSTOP - 1));
    return old;
}

/* sigpending 系统调用的实现
 * sya_call 里面会把 %fs 指向局部数据区, 这里检测并取得进程收到的但被
 * 屏蔽(阻塞)的信号, 还未处理信号的位图将被放入 set 中
 *
 * sigpending 的时候, CPU 运行在 CPL=0, 因此 DS 也是指向内核数据段的,
 * 要更新用户数据段的内容, 就需要段超越前缀 */
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

/* atomically swap in the new signal mask, and wait for a signal.
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
 */
/* 自动地更换成新的信号屏蔽码, 并等待信号的到来
 *
 * 我们需要对系统调用(syscall)做一些处理. 我们会从系统调用库接口取得某些信息
 * 注意，我们需要把调用规则与libc库中的子程序统一考虑。
 *
 * "set" 正是POSIX标准1003.1-1988的3.3.7节中所描述的信号屏蔽码sigmask。
 *       其中认为类型sigset_t能够作为一个32位量传递。
 *
 * "restart"中保持有重启指示标志。如果为非0值，那么我们就设置原来的屏蔽码，
 *       并且正常返回。如果它为0，那么我们就把当前的屏蔽码保存在oldmask中
 *       并且阻塞进程，直到收到任何一个信号为止。
 */
int sys_sigsuspend(int restart, unsigned long old_mask, unsigned long set)
{
    extern int sys_pause(void);

    /* 如果 restart 标志不为 0, 表示是重新让程序运行起来, 于是恢复前面保存在
     * old_mask 中的 原进程阻塞码. 并返回码 -EINTR (系统调用被信号中断) */
    if (restart) {
        /* we're restarting */
        current->blocked = old_mask;
        return -EINTR;
    }

    /* 否则 restart 标志的值是 0, 表示第 1 次调用.
     * 首先设置 restart 标志(置为 1), 保存进程当前阻塞码 blocked 到 old_mask 中,
     * 并把进程的阻塞码替换成 set. 然后调用 pause 让进程睡眠, 等待信号的到来.
     * 当进程 收到一个信号时, pause 就会返回, 并且进程会去执行信号处理函数,
     * 然后本调用 返回 -ERESTARTNOINTR 码退出,
     * 这个返回码说明在处理完信号后要求返回到本系统 调用中继续运行,
     * 即本系统调用不会被中断 */

    /* we're not restarting.  do the work */
    *(&restart) = 1;
    *(&old_mask) = current->blocked;
    current->blocked = set;

    /* pause 系统调用将导致调用它的进程进入睡眠状态, 直到收到一个信号
     * 该信号或者会终止进程的执行, 或者导致进程去执行相应的信号捕获函数 */
    (void)sys_pause(); /* return after a signal arrives */

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

/* 系统调用的中断处理程序中真正的信号预处理程序(在 kernel/sys_call.s)
 *
 * 这段代码的主要作用是将信号处理句柄插入到用户程序堆栈中,
 * 并在本系统调用结束返回 后立刻执行信号句柄程序, 然后继续执行用户的程序.
 *
 * 入参是在 int0x80 和 sys_call.s 里面压入的 */
int do_signal(long signr, long eax, long ebx, long ecx, long edx, long orig_eax, long fs, long es,
              long ds, long eip, long cs, long eflags, unsigned long *esp, long ss)
{
    unsigned long sa_handler;
    long old_eip = eip; /* 这个 EIP 是在 int0x80 的时候压栈的那个 EIP,
                           是用户任务的下一条指令 */
    struct sigaction *sa = current->sigaction + signr - 1;
    int longs;

    unsigned long *tmp_esp;

#ifdef notdef
    printk("pid: %d, signr: %x, eax=%d, oeax = %d, int=%d\n", current->pid, signr, eax, orig_eax,
           sa->sa_flags & SA_INTERRUPT);
#endif
    /* origin_eax != -1 表示, 是因为某个系统调用软中断(int0x80)到
     * 这里来的(其他情况都是 -1, 参考 sys_call.s)
     *
     * TODO: 看一下这两个返回 ERESTARTSYS 的系统调用
     *
     * 在 kernel/exit.c 的 waitpid 函数中, 如果收到了 SIGCHLD 信号, 或者在读管道
     * 函数 fs/pipe.c 中管道当前读数据但没有读到任何数据等情况下,
     * 进程收到了任何一个非阻塞的信号, 则都会以 -ERESTARTSYS 返回值返回.
     * 它表示进程可以被中断, 但是在继续执行后会重新启动系统调用.
     *
     * 返回码 -ERESTARTNOINTR 说明在处理完信号后要求返回到原系统调用中继续运行,
     * 即系统 调用不会被中断
     *
     * 因此下面语句说明如果是在系统调用中调用的本函数, 并且相应系统调用的返回码
     * eax 等于 -ERESTARTSYS 或 -ERESTARTNOINTR
     * 时进行下面的处理(实际上还没有真正回到用户程序中) */
    if ((orig_eax != -1) && ((eax == -ERESTARTSYS) || (eax == -ERESTARTNOINTR))) {

        if ((eax == -ERESTARTSYS) &&
            ((sa->sa_flags & SA_INTERRUPT) || signr < SIGCONT || signr > SIGTTOU))
            /* 如果系统调用返回 -ERESTARTSYS , 并且 sigaction 中含有标志
             * SA_INTERRUPT 或者信号值小于 SIGCONT 或者信号值大于 SIGTTOU,
             * 即信号不是SIGCONT/SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU),
             * 则修改系统调用的返回值为 eax = -EINTR, 即被信号中断的系统调用 */
            *(&eax) = -EINTR;
        else {
            /* 否则就恢复进程寄存器 eax 在调用系统调用之前的值, 并且把原程序指令指针回
             * 调 2 字节, 即当返回用户程序时, 让程序重新启动执行被信号中断的系统调用 */
            *(&eax) = orig_eax;
            *(&eip) = old_eip -= 2;
        }
    }

    sa_handler = (unsigned long)sa->sa_handler;
    if (sa_handler == 1) { /* 忽略处理 SIG_IGN = 1 */
        return (1);        /* Ignore, see if there are more signals... */
    }

    if (!sa_handler) { /* 默认处理 SIG_DFL = 0 */
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
            if (!(current->p_pptr->sigaction[SIGCHLD - 1].sa_flags & SA_NOCLDSTOP))
                current->p_pptr->signal |= (1 << (SIGCHLD - 1));
            return (1); /* Reschedule another event */

        case SIGQUIT:
        case SIGILL:
        case SIGTRAP:
        case SIGIOT:
        case SIGFPE:
        case SIGSEGV:
            if (core_dump(signr))
                /* TODO: 0x80 和 WIFSTOPPED 等 waitpid 函数返回的状态字有关系, 可以仔细研究下 */
                do_exit(signr | 0x80); /* fall through */
        default:
            do_exit(signr);
        }
    }

    /* 走到这里的, 需要做自定义处理
     * OK, we're invoking a handler */
    if (sa->sa_flags & SA_ONESHOT)
        sa->sa_handler = NULL;

    /* 下面操作破坏了原始 EIP 位置的代码段数据
     * 但是这个破坏是有意(也有益)的, 如此操作可以让系统调用软中断 iret 的时候,
     * 跳转到 sa_handler 去执行, iret 之后对 0 特权级的栈没有任何影响
     *
     * 等到 sa_handler 执行的时候, 特权级 3 栈上的参数是下面几个 put_fs_long
     * 放上去的内容, 他们分别是:
     *      - sa->sa_restorer
     *      - signr
     *      - current->blocked
     *      - eax
     *      - ecx
     *      - edx
     *      - eflags
     *      - old_eip
     * 于是乎, sa_handler 返回的时候, 又跳转到了 old_eip 位置继续执行
     * 这个栈操作安排确实妙不可言, 大神对 C/汇编 的栈帧理解太到位了 */

    /* 将内核态栈上用户调用系统调用下一条代码指令指针 eip 指向该信号处理句柄
     * 之后从 int0x80 iret 的时候, 就会到对应的信号处理句柄去执行 */
    *(&eip) = sa_handler;

    // 将原调用程序的用户堆栈指针向下扩展 7(或者 8)
    // 个长字(用来存放调用信号句柄的参数等)
    // 并检查内存使用情况(例如如果内存超界则分配新页等)
    longs = (sa->sa_flags & SA_NOMASK) ? 7 : 8;
    *(&esp) -= longs;
    verify_area(esp, longs * 4);

    // 在用户堆栈中从下到上存放 sa_restorer, 信号 signr, 屏蔽码 blocked
    // (如果SA_NOMASK 置位), eax, ecx, edx, eflags 和用户程序原代码指针
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
