/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define DEBUG_PROC_TREE /* 定义符号 '调试进程树' */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <asm/segment.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>

int sys_pause(void);
int sys_close(int fd);

/* 释放指定进程占用的任务槽及其任务数据结构占用的内存页面
 * 该函数在后面的 sys_kill 和 sys_waitpid 函数中被调用
 * 扫描任务指针数组表 task 以寻找指定的任务, 如果找到, 则首先清空该任务槽,
 * 然后释放 该任务数据结构所占用的内存页面, 最后执行调度函数并在返回时立即退出
 * 如果在任务数组表中没有找到指定任务对应的项, 则内核 panic */
void release(struct task_struct *p)
{
    int i;

    /* 如果给定的任务结构指针为 NULL 则退出
     * 如果该指针指向当前进程则显示警告信息退出 */
    if (!p)
        return;

    /* 不能释放自己 */
    if (p == current) {
        printk("task releasing itself\n\r");
        return;
    }

    /* 扫描任务结构指针数组, 寻找指定的任务 p
     * 如果找到, 则置空任务指针数组中对应项, 并且更新任务结构之间的关联指针,
     * 释放任务 p 数据结构占用的内存页面. 最后在执行调度程序, 返回后退出
     * 如果没有找到指定的任务 p, 则说明内核代码出错了, 则显示出错信息并死机
     * 更新链接部分的代码会把指定任务 p 从双向链表中删除 */
    for (i = 1; i < NR_TASKS; i++)
        if (task[i] == p) {
            task[i] = NULL;

            /* Update links */

            /* 如果 p 不是最后(最老)的子进程,
             * 则让比其老的比邻进程指向比它新的比邻进程 如果 p 不是最新的子进程,
             * 则让比其新的比邻子进程指向比邻的老进程 如果 p 是最新的子进程,
             * 则还需要更新其父进程的最新子进程指针 cptr 为指向 p 的比邻子进程
             * 指针 osptr (old sibling pointer) 指向比 p 先创建的兄弟进程
             * 指针 ysptr (younger sibling pointer) 指向比 p 后创建的兄弟进程
             * 指针 pptr (parent pointer) 指向 p 的父进程
             * 指针 cptr (child pointer) 是父进程指向最新(最后)创建的子进程 */
            if (p->p_osptr)
                p->p_osptr->p_ysptr = p->p_ysptr;
            if (p->p_ysptr)
                p->p_ysptr->p_osptr = p->p_osptr;
            else
                p->p_pptr->p_cptr = p->p_osptr;

            /* 注意这里只是释放了 p 结构占用的内存页面 */
            free_page((long)p);
            schedule();
            return;
        }

    panic("trying to release non-existent task");
}

#ifdef DEBUG_PROC_TREE
/* Check to see if a task_struct pointer is present in the task[] array
 * Return 0 if found, and 1 if not found. */
int bad_task_ptr(struct task_struct *p)
{
    int i;

    if (!p)
        return 0;

    for (i = 0; i < NR_TASKS; i++)
        if (task[i] == p)
            return 0;

    return 1;
}

/* This routine scans the pid tree and make sure the rep invarient still
 * holds.  Used for debugging only, since it's very slow....
 *
 * It looks a lot scarier than it really is.... we're doing nothing more
 * than verifying the doubly-linked list found in p_ysptr and p_osptr,
 * and checking it corresponds with the process tree defined by p_cptr and
 * p_pptr */
void audit_ptree()
{
    int i;

    for (i = 1; i < NR_TASKS; i++) {
        if (!task[i])
            continue;
        if (bad_task_ptr(task[i]->p_pptr))
            printk("Warning, pid %d's parent link is bad\n", task[i]->pid);
        if (bad_task_ptr(task[i]->p_cptr))
            printk("Warning, pid %d's child link is bad\n", task[i]->pid);
        if (bad_task_ptr(task[i]->p_ysptr))
            printk("Warning, pid %d's ys link is bad\n", task[i]->pid);
        if (bad_task_ptr(task[i]->p_osptr))
            printk("Warning, pid %d's os link is bad\n", task[i]->pid);
        if (task[i]->p_pptr == task[i])
            printk("Warning, pid %d parent link points to self\n");
        if (task[i]->p_cptr == task[i])
            printk("Warning, pid %d child link points to self\n");
        if (task[i]->p_ysptr == task[i])
            printk("Warning, pid %d ys link points to self\n");
        if (task[i]->p_osptr == task[i])
            printk("Warning, pid %d os link points to self\n");
        if (task[i]->p_osptr) {
            if (task[i]->p_pptr != task[i]->p_osptr->p_pptr)
                printk("Warning, pid %d older sibling %d parent is %d\n", task[i]->pid,
                       task[i]->p_osptr->pid, task[i]->p_osptr->p_pptr->pid);
            if (task[i]->p_osptr->p_ysptr != task[i])
                printk("Warning, pid %d older sibling %d has mismatched ys "
                       "link\n",
                       task[i]->pid, task[i]->p_osptr->pid);
        }
        if (task[i]->p_ysptr) {
            if (task[i]->p_pptr != task[i]->p_ysptr->p_pptr)
                printk("Warning, pid %d younger sibling %d parent is %d\n", task[i]->pid,
                       task[i]->p_osptr->pid, task[i]->p_osptr->p_pptr->pid);
            if (task[i]->p_ysptr->p_osptr != task[i])
                printk("Warning, pid %d younger sibling %d has mismatched os "
                       "link\n",
                       task[i]->pid, task[i]->p_ysptr->pid);
        }
        if (task[i]->p_cptr) {
            if (task[i]->p_cptr->p_pptr != task[i])
                printk("Warning, pid %d youngest child %d has mismatched "
                       "parent link\n",
                       task[i]->pid, task[i]->p_cptr->pid);
            if (task[i]->p_cptr->p_ysptr)
                printk("Warning, pid %d youngest child %d has non-NULL ys "
                       "link\n",
                       task[i]->pid, task[i]->p_cptr->pid);
        }
    }
}
#endif /* DEBUG_PROC_TREE */

/*
 * 向指定任务 p 发送信号 sig, 权限为 priv
 * 参数:
 *    - sig: 信号值
 *    - p: 指定任务的指针
 *    - priv: 强制发送信号的标志, 即不需要考虑进程用户属性或级别而能发送信号的权利
 *
 * 该函数首先判断参数的正确性, 然后判断条件是否满足
 * 如果满足就向指定进程发送信号 sig 并退出, 否则返回未许可错误号 */
static inline int send_sig(long sig, struct task_struct *p, int priv)
{
    if (!p) {
        return -EINVAL;
    }

    /* 要发送信号, 需要满足下面条件的一个:
     *     1. 显示执行 priv
     *     2. 当前进程 current 和进程 p 的 effective uid 相同
     *     3. 是超级用户 */
    if (!priv && (current->euid != p->euid) && !suser()) {
        return -EPERM;
    }

    /* 若需要发送的信号是 SIGKILL 或 SIGCONT, 那么如果此时接收信号的进程 p
     * 正处于停止状态就置其为就绪(运行)状态. 然后修改进程 p 的信号位图 signal,
     * 去掉(复位)会导致进程停止的信号 SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU */
    if ((sig == SIGKILL) || (sig == SIGCONT)) {
        if (p->state == TASK_STOPPED) {
            p->state = TASK_RUNNING;
        }

        p->exit_code = 0;
        p->signal &= ~((1 << (SIGSTOP - 1)) | (1 << (SIGTSTP - 1)) | (1 << (SIGTTIN - 1)) |
                       (1 << (SIGTTOU - 1)));
    }

    /* If the signal will be ignored, don't even post it
     * 信号处于被屏蔽的状态, 那么甚至都不需要发送信号 */
    if ((int)p->sigaction[sig - 1].sa_handler == 1) {
        return 0;
    }

    /* Depends on order SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU
     * 以下判断依赖于SIGSTOP、SIGTSTP、SIGTTIN和SIGTTOU的次序
     *
     * 如果信号是 SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU 之一,
     * 那么说明要让接收信号的进程 p 停止运行. 因此若 p 的信号位图中有 SIGCONT
     * 置位就需要复位位图中继续运行的信号 SIGCONT 比特位 */
    if ((sig >= SIGSTOP) && (sig <= SIGTTOU)) {
        p->signal &= ~(1 << (SIGCONT - 1));
    }

    /* Actually deliver the signal */
    p->signal |= (1 << (sig - 1));
    return 0;
}

/* 根据进程组号 pgrp 取得进程组所属的会话号
 * 扫描任务数组, 寻找进程组号为 pgrp 的进程, 并返回其会话号.
 * 如果没有找到指定进程组号为 pgrp 的任何进程, 则返回 -1 */
int session_of_pgrp(int pgrp)
{
    struct task_struct **p;

    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if ((*p)->pgrp == pgrp) {
            return ((*p)->session);
        }
    }

    return -1;
}

/**
 * @brief  终止进程组(向进程组发送信号)
 *
 * 向指定进程组 pgrp 中的每个进程发送指定信号 sig
 *
 * @param pgrp 指定的进程组号
 * @param sig 指定的信号
 * @param priv 强制发送标记
 *
 * @retval 0              向组内任意一个进程发送成功最后就会返回
 * @retval -ESRCH         如果没有找到指定进程组号 pgrp 的任何一个进程
 * @retval 发送失败的错误码  若找到进程组号是 pgrp 的进程, 但是发送信号失败
 */
int kill_pg(int pgrp, int sig, int priv)
{
    struct task_struct **p;
    int err, retval = -ESRCH;
    int found = 0;

    if (sig < 1 || sig > 32 || pgrp <= 0) {
        return -EINVAL;
    }

    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if ((*p)->pgrp == pgrp) {
            if (sig && (err = send_sig(sig, *p, priv))) {
                retval = err;
            } else {
                found++;
            }
        }
    }

    return (found ? 0 : retval);
}

/* 终止进程(向进程发送信号)
 * 参数:
 *      - pid: 进程号
 *      - sig: 指定信号
 *      - priv: 权限
 *
 * 即向进程号为 pid 的进程发送指定信号 sig
 * 若找到指定 pid 的进程, 那么若信号发送成功, 则返回0, 否则返回信号发送出错号.
 * 如果没有找到指定进程号 pid 的进程, 则返回出错号 -ESRCH (指定进程不存在) */
int kill_proc(int pid, int sig, int priv)
{
    struct task_struct **p;

    if (sig < 1 || sig > 32) {
        return -EINVAL;
    }

    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if ((*p)->pid == pid) {
            return (sig ? send_sig(sig, *p, priv) : 0);
        }
    }

    return (-ESRCH);
}

/* */

/**
 * @brief 系统调用 kill
 *
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 *
 * 如果 pid > 0, 则信号被发送给进程号是 pid 的进程
 * 如果 pid = 0, 那么信号就会被发送给当前进程的进程组中所有的进程
 * 如果 pid = -1, 则信号 sig 就会发送给除第一个进程(初始进程)外的所有进程
 * 如果 pid < -1, 则信号 sig 将发送给进程组 -pid 的所有进程
 * 如果信号 sig = 0, 则不发送信号, 但仍会进行错误检查. 如果成功则返回 0
 *
 * TODO-DONE: send_sig 对 sig=0 如何处理?
 * 答: 没有 0 号信号
 *
 * 该函数扫描任务数组表, 并根据 pid 对满足条件的进程发送指定信号 sig.
 * 若 pid 等于 0 表明当前进程是进程组组长, 需要向所有组内的进程强制发送信号
 *
 * @param pid 是进程号
 * @param sig 是需要发送的信号
 * @return int
 * @note 此函数可用于向任何进程或进程组发送任何信号, 而并非只是杀死进程
 */
int sys_kill(int pid, int sig)
{
    struct task_struct **p = NR_TASKS + task;
    int err, retval = 0;

    if (!pid) { /* pid = 0, 当前进程组所有进程都收到信号 */
        return (kill_pg(current->pid, sig, 0));
    }

    if (pid == -1) { /* pid = -1, 给所有进程发送信号 */
        while (--p > &FIRST_TASK) {
            if ((err = send_sig(sig, *p, 0))) {
                retval = err;
            }
        }
        return (retval);
    }

    if (pid < 0) { /* pid < 0, abs(pid) 指定了目标进程组 */
        return (kill_pg(-pid, sig, 0));
    }

    /* Normal kill */
    return (kill_proc(pid, sig, 0));
}

/**
 * @brief 判断进程组是不是孤儿进程组
 *
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are
 * to receive a SIGHUP and a SIGCONT.
 *
 * "I ask you, have you ever known what it is to be an orphan?"
 *
 * 僵尸进程: 指的是子进程先于父进程退出, 而父进程未对它进行回收(wait)所产生的
 * 孤儿进程: 指的是父进程先于子进程退出, 仍在运行的子进程成为孤儿进程
 * 孤儿进程组: 这个进程组里面的进程, 他们的父进程要么是这个组的成员, 要么在别的会话中
 * 更多资料参考: https://blog.csdn.net/q1007729991/article/details/57413719
 *
 * @param pgrp 进程组号
 * @retval 0 不是孤儿进程组
 * @retval 1 是孤儿进程组
 */
int is_orphaned_pgrp(int pgrp)
{
    struct task_struct **p;

    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if (!(*p) || ((*p)->pgrp != pgrp) || ((*p)->state == TASK_ZOMBIE) ||
            ((*p)->p_pptr->pid == 1)) {
            continue;
        }

        // 父子进程不属于相通的进程组, 且父进程和子进程属于相同的会话,
        // 符合这种条件的进程组, 肯定不是孤儿进程组
        if (((*p)->p_pptr->pgrp != pgrp) && ((*p)->p_pptr->session == (*p)->session)) {
            return 0;
        }
    }

    return (1); /* (sighing) "Often!" */
}

/* 判断进程组中是否含有处于停止状态的作业(进程组)
 * 查找方法是扫描整个任务数组, 检查属于指定组 pgrp 的任何进程是否处于停止状态
 * 有则返回 1, 无则返回 0 */
static int has_stopped_jobs(int pgrp)
{
    struct task_struct **p;

    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if ((*p)->pgrp != pgrp) {
            continue;
        }

        if ((*p)->state == TASK_STOPPED) {
            return (1);
        }
    }

    return (0);
}

/**
 * @brief 程序退出处理函数
 *
 * 被系统调用处理函数 sys_exit 调用
 *
 * 该函数将根据当前进程自身的特性对其进行处理, 并把当前进程状态
 * 设置成僵死状态 TASK_ZOMBIE, 最后调用调度函数 schedule 去
 * 执行其它进程, 不再返回
 *
 * @param code
 * @return volatile
 */
volatile void do_exit(long code)
{
    struct task_struct *p;
    int i;

    /* 释放代码段和数据段所占的页面 */
    free_page_tables(get_base(current->ldt[1]), get_limit(0x0f));
    free_page_tables(get_base(current->ldt[2]), get_limit(0x17));

    /* 关闭打开的文件句柄 */
    for (i = 0; i < NR_OPEN; i++) {
        if (current->filp[i]) {
            sys_close(i);
        }
    }

    iput(current->pwd);
    current->pwd = NULL;
    iput(current->root);
    current->root = NULL;
    iput(current->executable);
    current->executable = NULL;
    iput(current->library);
    current->library = NULL;
    current->state = TASK_ZOMBIE; /* 注意看进程退出的时候状态变成僵死状态 */
    current->exit_code = code;

    /* Check to see if any process groups have become orphaned
     * as a result of our exiting, and if they have any stopped
     * jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
     *
     * Case i: Our father is in a different pgrp than we are
     * and we were the only connection outside, so our pgrp
     * is about to become orphaned.
     *
     * 上面 `current->state = TASK_ZOMBIE` 之后, 就可以直接用 is_orphaned_pgrp
     * 判断 pgrp 是不是孤儿进程组了
     *
     * 这里
     *  - `current->p_pptr->pgrp != current->pgrp` 保证了, current 是目前组里
     *    辈分最高的那个进程, 只有杀死了这样的进程才有可能出现孤儿进程组
     *
     * TODO: 如果没有 stopped jobs, 连 SIGHUP 也不会发送吗 ????
     *
     * 如果进程的终止导致进程组变成孤儿进程组, 那么进程组中的所有进程就会与它们的作业控制
     * shell 断开联系, 作业控制 shell 将不再具有该进程组存在的任何信息. 而该进程组中处
     * 于停止状态的进程将会永远消失
     *
     * 为了解决这个问题, 含有停止状态进程的新近产生的孤儿进程组就需要接收到一个 SIGHUP 信号
     * 和一个 SIGCONT 信号, 用于指示它们已经从它们的会话(session)中断开联系
     * SIGHUP 信号将导致进程组中成员被终止, 除非它们捕获或忽略了 SIGHUP 信号
     * SIGCONT 信号将使那些没有被 SIGHUP 信号终止的进程继续运行 */
    if ((current->p_pptr->pgrp != current->pgrp) &&
        (current->p_pptr->session == current->session) && is_orphaned_pgrp(current->pgrp) &&
        has_stopped_jobs(current->pgrp)) {
        kill_pg(current->pgrp, SIGHUP, 1);
        kill_pg(current->pgrp, SIGCONT, 1);
    }

    /* Let father know we died */
    current->p_pptr->signal |= (1 << (SIGCHLD - 1));

    /* This loop does two things:
     *
     * A.  Make init inherit all the child processes
     * B.  Check to see if any process groups have become orphaned
     *    as a result of our exiting, and if they have any stopped
     *    jons, send them a SIGUP and then a SIGCONT.  (POSIX 3.2.2.2)
     *
     * 处理可能受影响的子进程
     *  1. 让 init 收养子进程
     *  2. 按照 POSIX 标准, 给因为此次退出而变成孤儿进程组的进程发送 `SIGUP + SIGCONT` */
    if ((p = current->p_cptr)) {
        while (1) {
            p->p_pptr = task[1]; /* init 进程收养子进程 */

            /* 子进程是僵死状态, 通知 init */
            if (p->state == TASK_ZOMBIE) {
                task[1]->signal |= (1 << (SIGCHLD - 1));
            }

            /* process group orphan check
             * Case ii: Our child is in a different pgrp
             * than we are, and it was the only connection
             * outside, so the child pgrp is now orphaned.
             *
             * 子进程和当前进程不属于同一组, 检查子进程的组是不是变成了孤儿进程组
             * 是的话给进程组发送 SIGHUP 和 SIGCONT 信号 */
            if ((p->pgrp != current->pgrp) && (p->session == current->session) &&
                is_orphaned_pgrp(p->pgrp) && has_stopped_jobs(p->pgrp)) {
                kill_pg(p->pgrp, SIGHUP, 1);
                kill_pg(p->pgrp, SIGCONT, 1);
            }

            /* 遍历所有的子进程 */
            if (p->p_osptr) {
                p = p->p_osptr;
                continue;
            }

            /* This is it; link everything into init's children
             * and leave
             *
             * 到这里 p 指向的是子进程里面最老的那个, 被 init 收养之后
             *  1. 比 p 还老的兄弟进程就是 init 原来的子进程
             *  2. init 原来子进程现在有更年轻的兄弟进程 p (注意不是最年轻的那个, 只是比自己次年轻)
             *  3. init 最新的子进程, 现在是 current->p_cptr, 顺便说这个子进程最年轻 */
            p->p_osptr = task[1]->p_cptr;
            task[1]->p_cptr->p_ysptr = p;
            task[1]->p_cptr = current->p_cptr;
            current->p_cptr = 0;
            break;
        }
    }

    /* 如果当前进程是某个会话首领, 且它有控制终端, 则首先向使用该控制终端的
     * 进程组发送挂断信号 SIGHUP, 然后释放该终端
     *
     * 接着扫描任务数组, 把属于当前进程会话中进程的终端置空(取消) */
    if (current->leader) {
        struct task_struct **p;
        struct tty_struct *tty;

        if (current->tty >= 0) {
            tty = TTY_TABLE(current->tty);
            if (tty->pgrp > 0) {
                kill_pg(tty->pgrp, SIGHUP, 1);
            }

            tty->pgrp = 0;
            tty->session = 0;
        }

        for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
            if ((*p)->session == current->session) {
                (*p)->tty = -1;
            }
        }
    }

    if (last_task_used_math == current) {
        last_task_used_math = NULL;
    }

#ifdef DEBUG_PROC_TREE
    audit_ptree();
#endif
    schedule();
}

/* 系统调用 exit
 *
 * error_code 是用户程序提供的退出状态信息, 只有低字节有效. 把 error_code
 * 左移 8 比特是 wait 或 waitpid 函数的要求. 低字节将用来保存 wait 的状态信息,
 * 低字节在信号处理中也用来保存信号信息
 *
 * 如果进程处于暂停状态(TASK_STOPPED), 那么其低字节就等于 0x7f
 * wait 或 waitpid 利用这些宏就可以取得子进程的退出状态码或子进程终止的原因(信号) */
int sys_exit(int error_code)
{
    do_exit((error_code & 0xff) << 8);
    return 0;
}

/**
 * @brief 系统调用 waitpid
 *
 * 挂起当前进程, 直到 pid 指定的子进程:
 *  1. 退出(终止)
 *  2. 收到要求终止该进程的信号
 *  3. 需要调用一个信号句柄(信号处理程序)
 *
 * 如果 pid 所指的子进程早已退出(已成所谓的僵死进程), 则本调用将立刻返回, 子进程使用的所有资源将释放
 * 如果 pid > 0, 表示等待进程号等于 pid 的子进程
 * 如果 pid = 0, 表示等待进程组号等于当前进程组号的任何子进程
 * 如果 pid < -1, 表示等待进程组号等于 pid 绝对值的任何子进程
 * 如果 pid = -1, 表示等待任何子进程
 *
 * 若 options = WUNTRACED, 表示如果子进程是停止的, 也马上返回(无须跟踪)
 * 若 options = WNOHANG, 表示如果没有子进程退出或终止就马上返回
 * 如果返回状态指针 stat_addr 不为空, 则就将状态信息保存到那里
 *
 * @param pid 进程号
 * @param stat_addr 保存状态信息位置的指针
 * @param options waitpid 选项
 * @return int
 */
int sys_waitpid(pid_t pid, unsigned long *stat_addr, int options)
{
    int flag;
    struct task_struct *p;
    unsigned long oldblocked;

    verify_area(stat_addr, 4);

repeat:
    flag = 0;
    for (p = current->p_cptr; p; p = p->p_osptr) {
        if (pid > 0) {
            if (p->pid != pid) {
                continue;
            }
        } else if (!pid) {
            if (p->pgrp != current->pgrp) {
                continue;
            }
        } else if (pid != -1) {
            if (p->pgrp != -pid) {
                continue;
            }
        }

        /* else pid == -1 是任意子进程都需要处理 */

        /* 经过上面的判断之后, 已经找到了需要等待的目标任务 p, 接下来根据任务状态进行处理
         *
         * 当子进程 p 处于停止状态时, 如果此时参数选项 options 中 WUNTRACED 标志没有置位, 表示
         * 程序无须立刻返回, 或者子进程此时的退出码等于 0, 于是继续扫描处理其他子进程
         * 如果 WUNTRACED 置位且子进程退出码不为 0, 则把退出码移入高字节, 或上状态信息 0x7f 后放入
         * stat_addr, 在复位子进程退出码后就立刻返回子进程号 pid
         * 这里 0x7f 表示的返回状态使 WIFSTOPPED 宏为真  */
        switch (p->state) {
        case TASK_STOPPED:
            /* 用 exit_code 判断可避免 waitpid 函数多次报告 p 的状态 */
            if (!(options & WUNTRACED) || !p->exit_code) {
                continue;
            }

            put_fs_long((p->exit_code << 8) | 0x7f, stat_addr);
            p->exit_code = 0;
            return p->pid;
        case TASK_ZOMBIE:
            /* 如果子进程 p 处于僵死状态, 则首先把它在用户态和内核态运行的时间分别累计到
             * 当前进程(父进程)中, 然后取出子进程的 pid 和退出码, 把退出码放入返回状态
             * 位置 stat_addr 处并释放该子进程. 最后返回子进程的退出码和 pid */
            current->cutime += p->utime;
            current->cstime += p->stime;
            flag = p->pid;
            put_fs_long(p->exit_code, stat_addr);
            release(p);

#ifdef DEBUG_PROC_TREE
            audit_ptree();
#endif
            return flag;
        default:
            /* 如果这个子进程 p 的状态既不是停止也不是僵死, 那么就置 flag = 1
             * 表示找到过一个符合要求的子进程, 但是它处于运行态或睡眠态 */
            flag = 1;
            continue;
        }
    }

    /* 经过上面处理, flag 置位说明有符合等待要求的子进程, 且它没有处于退出或僵死状态 */
    if (flag) {
        /* 此时如果已设置 WNOHANG 选项(表示若没有子进程处于退出或终止态就立刻返回)
         * 就立刻返回 0 */
        if (options & WNOHANG) {
            return 0;
        }

        /* 把当前进程置为可中断等待状态并, 保留并修改当前进程信号阻塞位图,
         * 允许其接收到 SIGCHLD 信号, 然后执行调度程序, 等这里的 child 退出即可 */
        current->state = TASK_INTERRUPTIBLE;
        oldblocked = current->blocked;
        current->blocked &= ~(1 << (SIGCHLD - 1)); /* 只允许接口 SIGCHILD */
        schedule();

        /* 当系统又开始执行本进程时, 如果本进程收到除SIGCHLD以外的其他未屏蔽信号,
         * 则以退出码 '重新启动系统调用' 返回. 否则跳转到函数开始处 repeat 标号
         * 处重复处理, 重复这一步的处理大概率 current 已经 TASK_STOPPED 了 */
        current->blocked = oldblocked;
        if (current->signal & ~(current->blocked | (1 << (SIGCHLD - 1)))) {
            return -ERESTARTSYS;
        } else {
            goto repeat;
        }
    }

    /* 若 flag = 0, 表示没有找到符合要求的子进程, 则返回出错码(子进程不存在) */
    return -ECHILD;
}
