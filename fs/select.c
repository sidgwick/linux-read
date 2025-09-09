/*
 * This file contains the procedures for the handling of select
 *
 * Created for Linux based loosely upon Mathius Lattner's minix
 * patches by Peter MacDonald. Heavily edited by Linus.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>

#include <asm/segment.h>
#include <asm/system.h>

#include <const.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/*
 * Ok, Peter made a complicated, but straightforward multiple_wait() function.
 * I have rewritten this, taking some shortcuts: This code may not be easy to
 * follow, but it should be free of race-conditions, and it's practical. If you
 * understand what I'm doing here, then you understand how the linux
 * sleep/wakeup mechanism works.
 *
 * Two very simple procedures, add_wait() and free_wait() make all the work. We
 * have to have interrupts disabled throughout the select, but that's not really
 * such a loss: sleeping automatically frees interrupts when we aren't in this
 * task.
 */

typedef struct {
    struct task_struct *old_task;      /* 指向发起等待的进程 */
    struct task_struct **wait_address; /* 指向发起等待的进程的指针地址 */
} wait_entry;

typedef struct {
    int nr;
    wait_entry entry[NR_OPEN * 3]; /* 每个文件描述符, 可以监控 read/write/exception 三种状态 */
} select_table;

/**
 * @brief 将 wait_address 代表的任务, 追加到 select_table 里面
 *
 * @param wait_address 等待事件的任务
 * @param p 等待表
 */
static void add_wait(struct task_struct **wait_address, select_table *p)
{
    int i;

    if (!wait_address) {
        return;
    }

    /* 如果已经在表中, 直接退出 */
    for (i = 0; i < p->nr; i++) {
        if (p->entry[i].wait_address == wait_address) {
            return;
        }
    }

    /* pipe/tty 里面有个记录等待事件的指针字段(统一都叫做 i_wait), 这里把这个字段上记录的以前
     * 的等待进程(如有的话), 记录在 wait_address 和 old_task 里面
     *
     * 然后把当前进程塞道这个等待字段(i_wait)里面来 */
    p->entry[p->nr].wait_address = wait_address;
    p->entry[p->nr].old_task = *wait_address;
    *wait_address = current;
    p->nr++;
}

/**
 * @brief 尝试将等待表归零
 *
 * 这里和 __sleep_on 函数里面的处理差不多, 不同的是这里是多个文件描述符, 在 for 循环里面处理
 *
 * @param p
 */
static void free_wait(select_table *p)
{
    int i;
    struct task_struct **tpp; /* task pointer pointer */

    /* 遍历等待表中的 entry
     * 如果 wait_address 不为空, 就尝试唤醒 wait_address 对应的任务, 并挂起当前任务 */
    for (i = 0; i < p->nr; i++) {
        tpp = p->entry[i].wait_address;

        /* wait_address 指向的是 i_wait 的二级指针, 在 select 期间, 完全有可能有其他的进程
         * 通过 sleep_on 函数修改了 i_wait, 这时候就出现了 (*tpp != current) 的情况, 通过
         * 将发起 sleep_on 的那个进程唤醒, 让那边先得到执行. 在 sleep_on 的执行里面, 会对此前
         * 记录在 i_wait 上的那个的进程(发起 select 的进程, 也就是这里的 current)执行唤醒,
         * 然后程序就可以继续执行下去了 */
        while (*tpp && *tpp != current) {
            (*tpp)->state = 0;
            current->state = TASK_UNINTERRUPTIBLE;
            schedule();
        }

        if (!*tpp) {
            printk("free_wait: NULL");
        }

        /* wait_address 指向的内容可以变化, 但是 old_task 永远都是 add_wait 的时候的那个进程 */
        if ((*tpp = p->entry[i].old_task)) {
            (**tpp).state = 0;
        }
    }

    p->nr = 0;
}

/**
 * @brief 获取 inode 对应的控制终端
 *
 * @param inode 设备文件 inode
 * @return struct tty_struct* 设备文件对应的 tty 结构
 */
static struct tty_struct *get_tty(struct m_inode *inode)
{
    int major, minor;

    /* tty 设备肯定是字符设备 */
    if (!S_ISCHR(inode->i_mode)) {
        return NULL;
    }

    if ((major = MAJOR(inode->i_zone[0])) != 5 && major != 4) {
        return NULL;
    }

    /* MAJOR == 4: /dev/ttyx
     * MAJOR == 5: /dev/tty
     * 所以通过这里就能看出来 /dev/tty 特指当亲进程使用的那个控制终端 */
    if (major == 5) {
        minor = current->tty;
    } else {
        minor = MINOR(inode->i_zone[0]);
    }

    if (minor < 0) {
        return NULL;
    }

    return TTY_TABLE(minor);
}

/**
 * @brief 检查 inode 是否可读
 *
 * The check_XX functions check out a file. We know it's either
 * a pipe, a character device or a fifo (fifo's not implemented)
 *
 * @param wait
 * @param inode
 * @return int
 */
static int check_in(select_table *wait, struct m_inode *inode)
{
    struct tty_struct *tty;

    if ((tty = get_tty(inode))) {
        /* tty 设备的话, 检查它的辅助队列 */
        if (!EMPTY(tty->secondary)) {
            return 1;
        } else {
            add_wait(&tty->secondary->proc_list, wait);
        }
    } else if (inode->i_pipe) {
        /* pipe 的话, 检查它不为空 */
        if (!PIPE_EMPTY(*inode)) {
            return 1;
        } else {
            add_wait(&inode->i_wait, wait);
        }
    }

    return 0;
}

/**
 * @brief 检查 inode 是否可写
 *
 * @param wait
 * @param inode
 * @return int
 */
static int check_out(select_table *wait, struct m_inode *inode)
{
    struct tty_struct *tty;

    if ((tty = get_tty(inode))) {
        if (!FULL(tty->write_q)) {
            return 1;
        } else {
            add_wait(&tty->write_q->proc_list, wait);
        }
    } else if (inode->i_pipe) {
        if (!PIPE_FULL(*inode)) {
            return 1;
        } else {
            add_wait(&inode->i_wait, wait);
        }
    }

    return 0;
}

/**
 * @brief 检查 inode 是否出现异常
 *
 * @param wait
 * @param inode
 * @return int
 */
static int check_ex(select_table *wait, struct m_inode *inode)
{
    struct tty_struct *tty;

    if ((tty = get_tty(inode))) {
        /* tty 设备永远不会异常 */
        if (!FULL(tty->write_q)) {
            return 0;
        } else {
            return 0;
        }
    } else if (inode->i_pipe) {
        /* 管道文件, 读写端都要存在才是正常 */
        if (inode->i_count < 2) {
            return 1;
        } else {
            add_wait(&inode->i_wait, wait);
        }
    }

    return 0;
}

/**
 * @brief
 *
 * @param in
 * @param out
 * @param ex
 * @param inp
 * @param outp
 * @param exp
 * @return int
 */
int do_select(fd_set in, fd_set out, fd_set ex, fd_set *inp, fd_set *outp, fd_set *exp)
{
    int count;
    select_table wait_table;
    int i;
    fd_set mask;

    mask = in | out | ex;

    /* 现在仅支持字符设备, 管道文件, fifo 文件的处理 */
    for (i = 0; i < NR_OPEN; i++, mask >>= 1) {
        if (!(mask & 1)) {
            continue;
        }

        if (!current->filp[i]) {
            return -EBADF;
        }

        if (!current->filp[i]->f_inode) {
            return -EBADF;
        }

        if (current->filp[i]->f_inode->i_pipe) {
            continue;
        }

        if (S_ISCHR(current->filp[i]->f_inode->i_mode)) {
            continue;
        }

        if (S_ISFIFO(current->filp[i]->f_inode->i_mode)) {
            continue;
        }

        return -EBADF;
    }

repeat:
    wait_table.nr = 0;
    *inp = *outp = *exp = 0;
    count = 0;
    mask = 1;

    /*                            mask = mask << 1      */
    for (i = 0; i < NR_OPEN; i++, mask += mask) {
        if (mask & in) {
            if (check_in(&wait_table, current->filp[i]->f_inode)) {
                *inp |= mask;
                count++;
            }
        }

        if (mask & out) {
            if (check_out(&wait_table, current->filp[i]->f_inode)) {
                *outp |= mask;
                count++;
            }
        }

        if (mask & ex) {
            if (check_ex(&wait_table, current->filp[i]->f_inode)) {
                *exp |= mask;
                count++;
            }
        }
    }

    /* 注意: 因为已经 cli, 这个函数里不会因为中断被打断执行
       1. (没有信号出现) 且 (有需要等待的 fd) 且 (没有文件描述符更新)
       2. (没有信号出现) 且 (进程没有等待超时) 且 (没有文件描述符更新) */
    if (!(current->signal & ~current->blocked) && (wait_table.nr || current->timeout) && !count) {
        current->state = TASK_INTERRUPTIBLE;
        schedule();

        /* 执行到这里, 是因为信号/描述符状态有改变导致的, 此时使用 free_wait
         * 激活等待进程的状态, 然后重新统计描述符的变化情况
         *
         * 注意看这里每个循环都 free */
        free_wait(&wait_table);
        goto repeat;
    }

    free_wait(&wait_table);
    return count;
}

/**
 * @brief select 系统调用
 *
 * Note that we cannot return -ERESTARTSYS, as we change our input
 * parameters. Sad, but there you are. We could do some tweaking in
 * the library function ...
 *
 * @param buffer
 * @return int
 *
 * TODO: select 系统调用的参数怎么穿进来的?
 */
int sys_select(unsigned long *buffer)
{
    /* Perform the select(nd, in, out, ex, tv) system call. */
    int i;
    fd_set res_in, in = 0, *inp;
    fd_set res_out, out = 0, *outp;
    fd_set res_ex, ex = 0, *exp;
    fd_set mask;
    struct timeval *tvp;
    unsigned long timeout;

    /* buffer = [mask(nfds), inp, outp, exp, tvp]
     *
     * 假如 nfds = 8, 则 ~((~0) << 8) =
     * ~((~0) << 8)
     * = ~((1111_1111_1111_1111) << 8)
     * = ~(1111_1111_1111_1100)
     * = 0000_0000_0000_0011
     * 所以这个操作实际上就是 组装一个低 nfds 位都是1 的掩码出来  */

    mask = ~((~0) << get_fs_long(buffer++));
    inp = (fd_set *)get_fs_long(buffer++);
    outp = (fd_set *)get_fs_long(buffer++);
    exp = (fd_set *)get_fs_long(buffer++);
    tvp = (struct timeval *)get_fs_long(buffer);

    if (inp) {
        in = mask & get_fs_long(inp);
    }

    if (outp) {
        out = mask & get_fs_long(outp);
    }

    if (exp) {
        ex = mask & get_fs_long(exp);
    }

    timeout = 0xffffffff;
    if (tvp) {
        timeout = get_fs_long((unsigned long *)&tvp->tv_usec) / (1000000 / HZ);
        timeout += get_fs_long((unsigned long *)&tvp->tv_sec) * HZ;
        timeout += jiffies;
    }

    current->timeout = timeout;
    cli();
    i = do_select(in, out, ex, &res_in, &res_out, &res_ex);

    if (current->timeout > jiffies) {
        /* 未超时 */
        timeout = current->timeout - jiffies;
    } else {
        timeout = 0;
    }

    sti();
    current->timeout = 0;

    if (i < 0) {
        return i;
    }

    if (inp) {
        verify_area(inp, 4);
        put_fs_long(res_in, inp);
    }

    if (outp) {
        verify_area(outp, 4);
        put_fs_long(res_out, outp);
    }

    if (exp) {
        verify_area(exp, 4);
        put_fs_long(res_ex, exp);
    }

    if (tvp) {
        verify_area(tvp, sizeof(*tvp));
        put_fs_long(timeout / HZ, (unsigned long *)&tvp->tv_sec);
        timeout %= HZ;
        timeout *= (1000000 / HZ);
        put_fs_long(timeout, (unsigned long *)&tvp->tv_usec);
    }

    /* 因为信号导致的 select 返回 */
    if (!i && (current->signal & ~current->blocked)) {
        return -EINTR;
    }

    return i;
}
